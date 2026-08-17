// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file glTFRuntimeSafety.cpp
 * @brief Serialized native scheduling, cache lifetime barriers, and session quarantine.
 */
#include "System/glTFRuntimeSafety.h"

#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "System/GlbValidation.h"
#include "System/SafeFileIO.h"
#include "UObject/StrongObjectPtr.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeParser.h"

namespace glTFRuntimeSafetyPrivate
{
    /** One native operation waiting for a slot. Queue state is game-thread-only. */
    struct FQueuedOperation
    {
        uint64 Ticket = 0;
        TWeakObjectPtr<UObject> Owner;
        TWeakObjectPtr<UglTFRuntimeAsset> Asset;
        FString Label;
        FglTFRuntimeSafety::FQueuedStart Start;
        FglTFRuntimeSafety::FRejected Rejected;
    };

    /** Metadata retained while one native operation owns a slot. */
    struct FActiveOperation
    {
        FActiveOperation(
            const uint64 InTicket,
            UObject* InOwner,
            UglTFRuntimeAsset* InAsset,
            FString InLabel,
            const double InStartedAtSeconds)
            : Ticket(InTicket)
            , Owner(InOwner)
            , Asset(InAsset)
            , Label(MoveTemp(InLabel))
            , StartedAtSeconds(InStartedAtSeconds)
        {
        }

        uint64 Ticket = 0;
        // Once native work starts, both UObjects must outlive every plugin callback. Queued jobs
        // remain weak so cancellation can discard them, but active jobs deliberately become strong.
        TStrongObjectPtr<UObject> Owner;
        TStrongObjectPtr<UglTFRuntimeAsset> Asset;
        FString Label;
        double StartedAtSeconds = 0.0;
    };

    /**
     * A strong reference is required while the original actor/action is dropping its UPROPERTY.
     * The reference is released only after ClearCache has run on the game thread.
     */
    struct FPendingAssetRelease
    {
        explicit FPendingAssetRelease(UglTFRuntimeAsset* InAsset)
            : Asset(InAsset)
        {
        }

        TStrongObjectPtr<UglTFRuntimeAsset> Asset;
    };

    struct FFailureRecord
    {
        int32 Count = 0;
        FString LastReason;
    };

    struct FState
    {
        // Parser construction is worker-thread-only and globally serialized. This avoids overlapping
        // third-party parser allocations while several model files are discovered during map entry.
        FCriticalSection ParserGateLock;
        int32 ActiveParserCreations = 0;

        // Failure records are queried by both game and worker threads.
        FCriticalSection FailureLock;
        TMap<FString, FFailureRecord> Failures;

        // Control flags are read by both game-thread and worker-thread entry points.
        FThreadSafeCounter CircuitOpenFlag;
        FThreadSafeCounter ShuttingDownFlag;
        FCriticalSection ControlLock;
        FString CircuitReason;

        // Everything below is game-thread-only. Keeping a single owning thread avoids a second
        // synchronization layer around UObject weak/strong pointer transitions and callbacks.
        TArray<FQueuedOperation> Queue;
        TMap<uint64, FActiveOperation> ActiveOperations;
        TArray<FPendingAssetRelease> PendingAssetReleases;
        uint64 NextTicket = 1;
        bool bPumpingQueue = false;
        FTSTicker::FDelegateHandle WatchdogHandle;
    };

    FState& GetState()
    {
        static FState State;
        return State;
    }

    constexpr int32 MaximumQueuedOperations = 4096;
    constexpr int32 MaximumConcurrentNativeOperations = 1;
    constexpr int32 MaximumConcurrentParserCreations = 1;

    bool IsShuttingDown()
    {
        return GetState().ShuttingDownFlag.GetValue() != 0;
    }

    bool IsCircuitOpen(FString* OutReason = nullptr)
    {
        FState& State = GetState();
        const bool bOpen = State.CircuitOpenFlag.GetValue() != 0;
        if (bOpen && OutReason)
        {
            FScopeLock ControlScope(&State.ControlLock);
            *OutReason = State.CircuitReason;
        }
        return bOpen;
    }

    void OpenCircuit(const FString& Reason)
    {
        FState& State = GetState();
        {
            FScopeLock ControlScope(&State.ControlLock);
            State.CircuitReason = Reason.Left(2048);
        }
        if (State.CircuitOpenFlag.GetValue() == 0)
        {
            State.CircuitOpenFlag.Increment();
        }
    }

    void RejectOperation(FQueuedOperation& Operation, const FString& Reason)
    {
        if (Operation.Rejected)
        {
            Operation.Rejected(Reason);
        }
    }

    bool IsAssetActive(const FState& State, const UglTFRuntimeAsset* Asset)
    {
        if (!Asset)
        {
            return false;
        }

        for (const TPair<uint64, FActiveOperation>& Pair : State.ActiveOperations)
        {
            if (Pair.Value.Asset.Get() == Asset)
            {
                return true;
            }
        }
        return false;
    }

    bool IsAssetPendingRelease(const FState& State, const UglTFRuntimeAsset* Asset)
    {
        if (!Asset)
        {
            return false;
        }

        for (const FPendingAssetRelease& Pending : State.PendingAssetReleases)
        {
            if (Pending.Asset.Get() == Asset)
            {
                return true;
            }
        }
        return false;
    }

    /** Moves matching queue entries out before invoking rejection callbacks, avoiding re-entrant mutation. */
    TArray<FQueuedOperation> RemoveQueuedOperationsForOwner(FState& State, const UObject* Owner)
    {
        TArray<FQueuedOperation> Removed;
        if (!Owner)
        {
            return Removed;
        }

        for (int32 Index = State.Queue.Num() - 1; Index >= 0; --Index)
        {
            if (State.Queue[Index].Owner.Get() == Owner)
            {
                Removed.Add(MoveTemp(State.Queue[Index]));
                State.Queue.RemoveAtSwap(Index, 1, EAllowShrinking::No);
            }
        }
        return Removed;
    }

    /** Moves every queued operation for one asset out before callbacks are invoked. */
    TArray<FQueuedOperation> RemoveQueuedOperationsForAsset(FState& State, const UglTFRuntimeAsset* Asset)
    {
        TArray<FQueuedOperation> Removed;
        if (!Asset)
        {
            return Removed;
        }

        for (int32 Index = State.Queue.Num() - 1; Index >= 0; --Index)
        {
            if (State.Queue[Index].Asset.Get() == Asset)
            {
                Removed.Add(MoveTemp(State.Queue[Index]));
                State.Queue.RemoveAtSwap(Index, 1, EAllowShrinking::No);
            }
        }
        return Removed;
    }

    /** Worker-thread RAII token for the serialized parser-construction gate. */
    class FParserCreationSlot
    {
    public:
        bool Acquire()
        {
            while (!IsShuttingDown() && !IsCircuitOpen())
            {
                {
                    FScopeLock Lock(&GetState().ParserGateLock);
                    if (GetState().ActiveParserCreations < MaximumConcurrentParserCreations)
                    {
                        ++GetState().ActiveParserCreations;
                        bHeld = true;
                        return true;
                    }
                }

                // Parser creation is already performed on the shared worker pool. A short sleep
                // avoids a hot spin while preserving cancellation/shutdown responsiveness.
                FPlatformProcess::SleepNoStats(0.001f);
            }
            return false;
        }

        ~FParserCreationSlot()
        {
            if (bHeld)
            {
                FScopeLock Lock(&GetState().ParserGateLock);
                GetState().ActiveParserCreations = FMath::Max(0, GetState().ActiveParserCreations - 1);
            }
        }

    private:
        bool bHeld = false;
    };
}

TSharedPtr<FglTFRuntimeParser> FglTFRuntimeSafety::CreateParserSafely(
    const FString& FilePath,
    const FglTFRuntimeConfig& Config)
{
    if (IsInGameThread())
    {
        UE_LOG(LogTemp, Error, TEXT("Refused glTFRuntime parser construction on the game thread: %s"), *FilePath);
        return nullptr;
    }

    FString CoordinatorReason;
    if (glTFRuntimeSafetyPrivate::IsShuttingDown())
    {
        UE_LOG(LogTemp, Warning, TEXT("Refused glTFRuntime parser construction during shutdown: %s"), *FilePath);
        return nullptr;
    }
    if (glTFRuntimeSafetyPrivate::IsCircuitOpen(&CoordinatorReason))
    {
        UE_LOG(LogTemp, Error,
            TEXT("Refused glTFRuntime parser because the safety circuit is open. Path=%s Reason=%s"),
            *FilePath,
            *CoordinatorReason);
        return nullptr;
    }

    FString QuarantineReason;
    if (IsPathQuarantined(FilePath, &QuarantineReason))
    {
        UE_LOG(LogTemp, Error, TEXT("Refused quarantined glTF file. Path=%s Reason=%s"),
            *FilePath,
            *QuarantineReason);
        return nullptr;
    }

    glTFRuntimeSafetyPrivate::FParserCreationSlot ParserSlot;
    if (!ParserSlot.Acquire())
    {
        return nullptr;
    }

    // Each request creates a distinct parser and mutable cache. The single construction slot avoids
    // overlapping large native allocations while map startup discovers several independent GLBs.
    if (glTFRuntimeSafetyPrivate::IsShuttingDown() ||
        glTFRuntimeSafetyPrivate::IsCircuitOpen(&CoordinatorReason))
    {
        return nullptr;
    }
    return FglTFRuntimeParser::FromFilename(FilePath, Config);
}

uint64 FglTFRuntimeSafety::EnqueueOperation(
    UObject* Owner,
    UglTFRuntimeAsset* Asset,
    const FString& Label,
    FQueuedStart Start,
    FRejected Rejected)
{
    // EnqueueOperation returns a ticket synchronously. Silently redispatching from a worker would
    // return 0 to the caller while a real request starts later, leaving its in-flight/ticket state
    // unsynchronized. All project call sites are GT-guarded, so reject misuse instead.
    if (!ensureMsgf(IsInGameThread(), TEXT("FglTFRuntimeSafety::EnqueueOperation must run on the game thread")))
    {
        return 0;
    }

    glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();
    if (!State.WatchdogHandle.IsValid())
    {
        State.WatchdogHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateStatic(&FglTFRuntimeSafety::TickWatchdog),
            1.0f);
    }

    FString CoordinatorReason;
    const bool bPendingRelease = glTFRuntimeSafetyPrivate::IsAssetPendingRelease(State, Asset);
    if (glTFRuntimeSafetyPrivate::IsShuttingDown() ||
        glTFRuntimeSafetyPrivate::IsCircuitOpen(&CoordinatorReason) ||
        !IsValid(Owner) || !IsValid(Asset) || !Start || bPendingRelease ||
        State.Queue.Num() >= glTFRuntimeSafetyPrivate::MaximumQueuedOperations)
    {
        if (Rejected)
        {
            if (glTFRuntimeSafetyPrivate::IsShuttingDown())
            {
                Rejected(TEXT("glTFRuntime coordinator is shutting down"));
            }
            else if (!CoordinatorReason.IsEmpty())
            {
                Rejected(CoordinatorReason);
            }
            else if (bPendingRelease)
            {
                Rejected(TEXT("The glTFRuntime asset is already pending safe release"));
            }
            else if (!IsValid(Owner) || !IsValid(Asset) || !Start)
            {
                Rejected(TEXT("Operation owner, runtime asset, or start callback is invalid"));
            }
            else
            {
                Rejected(TEXT("The glTFRuntime queue reached its safety limit"));
            }
        }
        return 0;
    }

    glTFRuntimeSafetyPrivate::FQueuedOperation Operation;
    Operation.Ticket = State.NextTicket++;
    Operation.Owner = Owner;
    Operation.Asset = Asset;
    Operation.Label = Label.Left(512);
    Operation.Start = MoveTemp(Start);
    Operation.Rejected = MoveTemp(Rejected);
    const uint64 Ticket = Operation.Ticket;
    State.Queue.Add(MoveTemp(Operation));
    PumpQueue_GameThread();
    return Ticket;
}

void FglTFRuntimeSafety::CompleteOperation(const uint64 Ticket)
{
    if (!IsInGameThread())
    {
        // Do not route this through FSafeFileIO: its shutdown gate intentionally rejects new
        // dispatches. The active-ticket map itself keeps this module and both UObjects logically
        // alive until this game-thread completion removes the ticket during the shutdown drain.
        AsyncTask(ENamedThreads::GameThread, [Ticket]()
        {
            FglTFRuntimeSafety::CompleteOperation(Ticket);
        });
        return;
    }

    glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();
    if (Ticket == 0 || !State.ActiveOperations.Contains(Ticket))
    {
        return;
    }

    State.ActiveOperations.Remove(Ticket);

    // A release requested during the callback is finalized before another job can reuse that parser.
    ProcessPendingAssetReleases_GameThread();
    PumpQueue_GameThread();
}

void FglTFRuntimeSafety::CompleteOperationAfterCallback(const uint64 Ticket)
{
    if (Ticket == 0)
    {
        return;
    }

    // Always enqueue, even when already on the game thread. glTFRuntime executes the project
    // delegate before its async context calls UnregisterGCObject(); releasing the ticket inline
    // could therefore run ClearCache against a callback wrapper that has not finished unwinding.
    AsyncTask(ENamedThreads::GameThread, [Ticket]()
    {
        FglTFRuntimeSafety::CompleteOperation(Ticket);
    });
}

void FglTFRuntimeSafety::CancelQueuedOperations(UObject* Owner)
{
    // Constructing/reading weak UObject pointers and mutating the queue are deliberately kept on
    // one owning thread. Every project caller is a UObject lifecycle or callback path on the GT.
    if (!ensureMsgf(IsInGameThread(), TEXT("FglTFRuntimeSafety::CancelQueuedOperations must run on the game thread")))
    {
        return;
    }

    if (!Owner)
    {
        return;
    }

    glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();
    TArray<glTFRuntimeSafetyPrivate::FQueuedOperation> Removed =
        glTFRuntimeSafetyPrivate::RemoveQueuedOperationsForOwner(State, Owner);
    for (glTFRuntimeSafetyPrivate::FQueuedOperation& Operation : Removed)
    {
        glTFRuntimeSafetyPrivate::RejectOperation(Operation, TEXT("Operation was cancelled before it started"));
    }

    ProcessPendingAssetReleases_GameThread();
    PumpQueue_GameThread();
}

void FglTFRuntimeSafety::RequestAssetRelease(UglTFRuntimeAsset* Asset)
{
    // The caller must transfer the last strong reference on the game thread. Silently posting a
    // weak pointer from a worker would leave a GC window before the deferred release can own it.
    if (!ensureMsgf(IsInGameThread(), TEXT("FglTFRuntimeSafety::RequestAssetRelease must run on the game thread")))
    {
        return;
    }

    if (!IsValid(Asset))
    {
        return;
    }

    glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();
    if (glTFRuntimeSafetyPrivate::IsAssetPendingRelease(State, Asset))
    {
        return;
    }

    // Acquire the strong reference before callers null their UPROPERTY. This closes the GC window
    // between actor teardown and a terminal native callback.
    State.PendingAssetReleases.Emplace(Asset);

    TArray<glTFRuntimeSafetyPrivate::FQueuedOperation> Removed =
        glTFRuntimeSafetyPrivate::RemoveQueuedOperationsForAsset(State, Asset);
    for (glTFRuntimeSafetyPrivate::FQueuedOperation& Operation : Removed)
    {
        glTFRuntimeSafetyPrivate::RejectOperation(
            Operation,
            TEXT("Operation was cancelled because its glTFRuntime asset is being released"));
    }

    ProcessPendingAssetReleases_GameThread();
    PumpQueue_GameThread();
}

void FglTFRuntimeSafety::ProcessPendingAssetReleases_GameThread()
{
    check(IsInGameThread());
    glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();

    for (int32 Index = State.PendingAssetReleases.Num() - 1; Index >= 0; --Index)
    {
        UglTFRuntimeAsset* Asset = State.PendingAssetReleases[Index].Asset.Get();
        if (!IsValid(Asset))
        {
            State.PendingAssetReleases.RemoveAtSwap(Index, 1, EAllowShrinking::No);
            continue;
        }

        if (glTFRuntimeSafetyPrivate::IsAssetActive(State, Asset))
        {
            continue;
        }

        // ClearCache touches the parser's mutable cache and must never overlap native work for the
        // same asset. Removing legacy root/standalone flags here also centralizes every final release.
        Asset->ClearCache();
        if (Asset->IsRooted())
        {
            Asset->RemoveFromRoot();
        }
        Asset->ClearFlags(RF_Public | RF_Standalone);
        State.PendingAssetReleases.RemoveAtSwap(Index, 1, EAllowShrinking::No);
    }
}

void FglTFRuntimeSafety::ReportRecoverableFailure(
    const FString& FilePath,
    const FString& Reason)
{
    const FString NormalizedPath = GlbValidation::NormalizePath(FilePath);
    if (NormalizedPath.IsEmpty())
    {
        return;
    }

    glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();
    FScopeLock FailureScope(&State.FailureLock);
    glTFRuntimeSafetyPrivate::FFailureRecord& Record = State.Failures.FindOrAdd(NormalizedPath);
    ++Record.Count;
    Record.LastReason = Reason.Left(2048);

    if (Record.Count >= 2)
    {
        UE_LOG(LogTemp, Error,
            TEXT("Quarantined glTF file after %d recoverable failures. Path=%s Reason=%s"),
            Record.Count,
            *NormalizedPath,
            *Record.LastReason);
    }
}

bool FglTFRuntimeSafety::IsPathQuarantined(
    const FString& FilePath,
    FString* OutReason)
{
    const FString NormalizedPath = GlbValidation::NormalizePath(FilePath);
    glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();
    FScopeLock FailureScope(&State.FailureLock);

    const glTFRuntimeSafetyPrivate::FFailureRecord* Record = State.Failures.Find(NormalizedPath);
    const bool bQuarantined = Record && Record->Count >= 2;
    if (bQuarantined && OutReason)
    {
        *OutReason = Record->LastReason;
    }
    return bQuarantined;
}

bool FglTFRuntimeSafety::IsCircuitOpen(FString* OutReason)
{
    return glTFRuntimeSafetyPrivate::IsCircuitOpen(OutReason);
}

void FglTFRuntimeSafety::BeginShutdown()
{
    glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();

    // Raise the atomic flag immediately so worker-thread parser requests stop before queue cleanup.
    if (State.ShuttingDownFlag.GetValue() == 0)
    {
        State.ShuttingDownFlag.Increment();
    }

    if (!IsInGameThread())
    {
        FSafeFileIO::DispatchTrackedGameThread([]()
        {
            FglTFRuntimeSafety::BeginShutdown();
        });
        return;
    }

    TArray<glTFRuntimeSafetyPrivate::FQueuedOperation> Removed = MoveTemp(State.Queue);
    State.Queue.Reset();
    for (glTFRuntimeSafetyPrivate::FQueuedOperation& Operation : Removed)
    {
        glTFRuntimeSafetyPrivate::RejectOperation(Operation, TEXT("glTFRuntime coordinator is shutting down"));
    }

    ProcessPendingAssetReleases_GameThread();
    if (State.WatchdogHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(State.WatchdogHandle);
        State.WatchdogHandle.Reset();
    }
}

bool FglTFRuntimeSafety::FlushPendingOperations(const double TimeoutSeconds)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("FglTFRuntimeSafety::FlushPendingOperations must run on the game thread")))
    {
        return false;
    }

    const double StartSeconds = FPlatformTime::Seconds();
    while (GetPendingOperationCount() > 0)
    {
        if (TimeoutSeconds >= 0.0 && FPlatformTime::Seconds() - StartSeconds >= TimeoutSeconds)
        {
            return false;
        }

        // Plugin callbacks are marshalled to the game thread. Pump them so terminal callbacks can
        // return their tickets and finalize deferred cache releases before this module unloads.
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        ProcessPendingAssetReleases_GameThread();
        FPlatformProcess::SleepNoStats(0.005f);
    }
    return true;
}

int32 FglTFRuntimeSafety::GetPendingOperationCount()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("glTFRuntime queue state must be read on the game thread")))
    {
        return 0;
    }

    const glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();
    return State.Queue.Num() + State.ActiveOperations.Num() + State.PendingAssetReleases.Num();
}

bool FglTFRuntimeSafety::TickWatchdog(const float DeltaSeconds)
{
    (void)DeltaSeconds;
    check(IsInGameThread());

    constexpr double NativeOperationTimeoutSeconds = 180.0;
    glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();
    if (glTFRuntimeSafetyPrivate::IsShuttingDown())
    {
        return false;
    }
    if (glTFRuntimeSafetyPrivate::IsCircuitOpen() || State.ActiveOperations.IsEmpty())
    {
        return true;
    }

    const double NowSeconds = FPlatformTime::Seconds();
    const glTFRuntimeSafetyPrivate::FActiveOperation* TimedOut = nullptr;
    for (const TPair<uint64, glTFRuntimeSafetyPrivate::FActiveOperation>& Pair : State.ActiveOperations)
    {
        if (Pair.Value.StartedAtSeconds > 0.0 &&
            NowSeconds - Pair.Value.StartedAtSeconds >= NativeOperationTimeoutSeconds)
        {
            TimedOut = &Pair.Value;
            break;
        }
    }

    if (!TimedOut)
    {
        return true;
    }

    const FString CircuitReason = FString::Printf(
        TEXT("glTFRuntime circuit breaker opened after operation [%llu] '%s' exceeded %.0f seconds"),
        TimedOut->Ticket,
        *TimedOut->Label,
        NativeOperationTimeoutSeconds);
    glTFRuntimeSafetyPrivate::OpenCircuit(CircuitReason);
    UE_LOG(LogTemp, Error, TEXT("%s. No additional glTFRuntime jobs will start this session."), *CircuitReason);

    TArray<glTFRuntimeSafetyPrivate::FQueuedOperation> Removed = MoveTemp(State.Queue);
    State.Queue.Reset();
    for (glTFRuntimeSafetyPrivate::FQueuedOperation& Operation : Removed)
    {
        glTFRuntimeSafetyPrivate::RejectOperation(Operation, CircuitReason);
    }
    return true;
}

void FglTFRuntimeSafety::PumpQueue_GameThread()
{
    check(IsInGameThread());
    glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();
    if (State.bPumpingQueue || glTFRuntimeSafetyPrivate::IsShuttingDown() ||
        glTFRuntimeSafetyPrivate::IsCircuitOpen())
    {
        return;
    }

    State.bPumpingQueue = true;
    ON_SCOPE_EXIT
    {
        State.bPumpingQueue = false;
    };

    while (State.ActiveOperations.Num() < glTFRuntimeSafetyPrivate::MaximumConcurrentNativeOperations)
    {
        int32 SelectedIndex = INDEX_NONE;
        TArray<glTFRuntimeSafetyPrivate::FQueuedOperation> RejectedBeforeStart;
        glTFRuntimeSafetyPrivate::FQueuedOperation Operation;
        bool bHasSelectedOperation = false;

        // Preserve queue order and reject stale entries before selecting the next globally
        // serialized native operation. The per-asset test remains as a defensive invariant.
        for (int32 Index = 0; Index < State.Queue.Num(); ++Index)
        {
            glTFRuntimeSafetyPrivate::FQueuedOperation& Candidate = State.Queue[Index];
            UObject* Owner = Candidate.Owner.Get();
            UglTFRuntimeAsset* Asset = Candidate.Asset.Get();

            if (!IsValid(Owner) || !IsValid(Asset) || !Candidate.Start ||
                glTFRuntimeSafetyPrivate::IsAssetPendingRelease(State, Asset))
            {
                RejectedBeforeStart.Add(MoveTemp(Candidate));
                State.Queue.RemoveAt(Index, 1, EAllowShrinking::No);
                --Index;
                continue;
            }

            if (!glTFRuntimeSafetyPrivate::IsAssetActive(State, Asset))
            {
                SelectedIndex = Index;
                break;
            }
        }

        if (SelectedIndex != INDEX_NONE)
        {
            // Remove and register the selected entry before invoking any rejection callback.
            // Callbacks are user code and may otherwise mutate Queue and invalidate SelectedIndex.
            Operation = MoveTemp(State.Queue[SelectedIndex]);
            State.Queue.RemoveAt(SelectedIndex, 1, EAllowShrinking::No);

            glTFRuntimeSafetyPrivate::FActiveOperation Active(
                Operation.Ticket,
                Operation.Owner.Get(),
                Operation.Asset.Get(),
                Operation.Label,
                FPlatformTime::Seconds());
            State.ActiveOperations.Add(Active.Ticket, MoveTemp(Active));
            bHasSelectedOperation = true;
        }

        if (!bHasSelectedOperation)
        {
            // Rejection callbacks are user code and may enqueue/cancel more work. Invoke them only
            // after the queue scan has finished so callback re-entry cannot invalidate Queue indices.
            for (glTFRuntimeSafetyPrivate::FQueuedOperation& Rejected : RejectedBeforeStart)
            {
                glTFRuntimeSafetyPrivate::RejectOperation(
                    Rejected,
                    TEXT("Operation owner/asset expired or the asset entered safe release before execution"));
            }

            // A rejection callback may have appended a newly runnable operation. Re-scan once the
            // callbacks are complete; otherwise there is no runnable asset until an active job ends.
            if (!RejectedBeforeStart.IsEmpty())
            {
                continue;
            }
            return;
        }

        UE_LOG(LogTemp, Verbose,
            TEXT("Starting serialized glTFRuntime operation [%llu] %s (active=%d/%d)"),
            Operation.Ticket,
            *Operation.Label,
            State.ActiveOperations.Num(),
            glTFRuntimeSafetyPrivate::MaximumConcurrentNativeOperations);

        // Start may synchronously fail and call CompleteOperation. The bPumpingQueue guard prevents
        // recursive pumping while this outer loop safely observes the updated active map.
        Operation.Start(Operation.Ticket);

        // Start the selected operation before invoking unrelated rejection callbacks. Otherwise a
        // rejection callback could request release of the just-selected asset after it was marked
        // active but before native work actually began.
        for (glTFRuntimeSafetyPrivate::FQueuedOperation& Rejected : RejectedBeforeStart)
        {
            glTFRuntimeSafetyPrivate::RejectOperation(
                Rejected,
                TEXT("Operation owner/asset expired or the asset entered safe release before execution"));
        }
    }
}
