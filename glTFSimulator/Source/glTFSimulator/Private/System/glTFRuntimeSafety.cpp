// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * File role: glTFRuntimeSafety.cpp
 * 역할: glTFRuntime의 native 작업과 해제를 직렬 조정합니다.
 * 핵심 기능: 작업 티켓·대기 큐, GC 참조 보호, 지연 cache 해제, 종료 drain.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

/**
 * @file glTFRuntimeSafety.cpp
 * @brief Serialized native scheduling, cache lifetime barriers, and session quarantine.
 */
#include "System/glTFRuntimeSafety.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"

#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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
        int64 FileSize = INDEX_NONE;
        FDateTime Timestamp;
    };

    void CaptureSourceFingerprint(const FString& NormalizedPath, int64& OutFileSize, FDateTime& OutTimestamp)
    {
        OutFileSize = IFileManager::Get().FileSize(*NormalizedPath);
        OutTimestamp = OutFileSize == INDEX_NONE
            ? FDateTime()
            : IFileManager::Get().GetTimeStamp(*NormalizedPath);
    }

    bool HasSourceFingerprintChanged(const FString& NormalizedPath, const FFailureRecord& Record)
    {
        // URI-style runtime references do not have a disk fingerprint. Their records are cleared on
        // success or by an explicit retry, while real source GLBs are invalidated automatically
        // after the author edits/replaces the file.
        if (NormalizedPath.StartsWith(TEXT("gwd://"), ESearchCase::IgnoreCase)
            || NormalizedPath.StartsWith(TEXT("gworld://"), ESearchCase::IgnoreCase))
        {
            return false;
        }

        int64 CurrentSize = INDEX_NONE;
        FDateTime CurrentTimestamp;
        CaptureSourceFingerprint(NormalizedPath, CurrentSize, CurrentTimestamp);
        return CurrentSize != Record.FileSize || CurrentTimestamp != Record.Timestamp;
    }

    struct FState
    {
        // One logical native gate covers parser construction, synchronous loads, asynchronous mesh
        // finalization, and parser cache teardown. The mutex protects the phase; it is never held
        // while third-party code executes.
        FCriticalSection NativeGateLock;
        enum class ENativePhase : uint8 { Idle, ParserCreation, MeshOperation, SynchronousOperation, CacheClear };
        ENativePhase NativePhase = ENativePhase::Idle;

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

    FString NormalizeFailureKey(const FString& SourceOrReference)
    {
        FString Clean = SourceOrReference.TrimStartAndEnd();
        if (Clean.StartsWith(TEXT("gwd://"), ESearchCase::IgnoreCase)
            || Clean.StartsWith(TEXT("gworld://"), ESearchCase::IgnoreCase))
        {
            Clean.ToLowerInline();
            return Clean;
        }
        return GlbValidation::NormalizePath(Clean);
    }

    constexpr int32 MaximumQueuedOperations = 4096;
    constexpr int32 MaximumConcurrentNativeOperations = 1;

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

    bool TryEnterNativePhase(const FState::ENativePhase Phase)
    {
        FState& State = GetState();
        FScopeLock Lock(&State.NativeGateLock);
        if (State.NativePhase != FState::ENativePhase::Idle)
        {
            return false;
        }
        State.NativePhase = Phase;
        return true;
    }

    void LeaveNativePhase(const FState::ENativePhase Phase)
    {
        FState& State = GetState();
        FScopeLock Lock(&State.NativeGateLock);
        ensureMsgf(State.NativePhase == Phase, TEXT("glTFRuntime native gate phase mismatch"));
        if (State.NativePhase == Phase)
        {
            State.NativePhase = FState::ENativePhase::Idle;
        }
    }

    bool IsNativeGateIdle()
    {
        FState& State = GetState();
        FScopeLock Lock(&State.NativeGateLock);
        return State.NativePhase == FState::ENativePhase::Idle;
    }

    /** Worker-thread RAII token for the single process-wide native gate. */
    class FParserCreationSlot
    {
    public:
        bool Acquire()
        {
            while (!IsShuttingDown() && !IsCircuitOpen())
            {
                if (TryEnterNativePhase(FState::ENativePhase::ParserCreation))
                {
                    bHeld = true;
                    return true;
                }

                // Parser creation is already performed on the shared worker pool. A short sleep
                // avoids a hot spin while preserving cancellation/shutdown responsiveness.
                FPlatformProcess::SleepNoStats(0.001f);
            }
            return false;
        }

        ~FParserCreationSlot()
        {
            Release();
        }

        void Release()
        {
            if (!bHeld) return;
            LeaveNativePhase(FState::ENativePhase::ParserCreation);
            bHeld = false;
        }

    private:
        bool bHeld = false;
    };
}

TSharedPtr<FglTFRuntimeParser> FglTFRuntimeSafety::CreateParserSafely(
    const FString& FilePath,
    const FglTFRuntimeConfig& Config,
    FString* OutError)
{
    if (OutError)
    {
        OutError->Reset();
    }

    auto Fail = [OutError](const FString& Reason) -> TSharedPtr<FglTFRuntimeParser>
    {
        if (OutError)
        {
            *OutError = Reason;
        }
        return nullptr;
    };

    if (IsInGameThread())
    {
        const FString Reason = TEXT("glTFRuntime parser construction was requested on the game thread");
        UE_LOG(LogTemp, Error, TEXT("%s: %s"), *Reason, *FilePath);
        return Fail(Reason);
    }

    FString CoordinatorReason;
    if (glTFRuntimeSafetyPrivate::IsShuttingDown())
    {
        const FString Reason = TEXT("glTFRuntime coordinator is shutting down");
        UE_LOG(LogTemp, Warning, TEXT("Refused glTFRuntime parser construction during shutdown: %s"), *FilePath);
        return Fail(Reason);
    }
    if (glTFRuntimeSafetyPrivate::IsCircuitOpen(&CoordinatorReason))
    {
        const FString Reason = FString::Printf(TEXT("glTFRuntime safety circuit is open: %s"), *CoordinatorReason);
        UE_LOG(LogTemp, Error,
            TEXT("Refused glTFRuntime parser because the safety circuit is open. Path=%s Reason=%s"),
            *FilePath,
            *CoordinatorReason);
        return Fail(Reason);
    }

    FString QuarantineReason;
    if (IsPathQuarantined(FilePath, &QuarantineReason))
    {
        const FString Reason = FString::Printf(TEXT("source is quarantined from an earlier failure: %s"), *QuarantineReason);
        UE_LOG(LogTemp, Error, TEXT("Refused quarantined glTF file. Path=%s Reason=%s"),
            *FilePath,
            *QuarantineReason);
        return Fail(Reason);
    }

    glTFRuntimeSafetyPrivate::FParserCreationSlot ParserSlot;
    if (!ParserSlot.Acquire())
    {
        return Fail(TEXT("glTFRuntime parser gate could not be acquired"));
    }

    // Each request creates a distinct parser and mutable cache. The single construction slot avoids
    // overlapping large native allocations while map startup discovers several independent GLBs.
    if (glTFRuntimeSafetyPrivate::IsShuttingDown() ||
        glTFRuntimeSafetyPrivate::IsCircuitOpen(&CoordinatorReason))
    {
        return Fail(CoordinatorReason.IsEmpty()
            ? TEXT("glTFRuntime parser creation was cancelled")
            : CoordinatorReason);
    }

    TSharedPtr<FglTFRuntimeParser> Parser = FglTFRuntimeParser::FromFilename(FilePath, Config);

    // FromFilename is glTFRuntime's normal path and is also what its official async loader uses.
    // If it fails after our own GLB preflight successfully opened the same file, retry once from
    // bytes. This avoids platform/path-layer false negatives while preserving the source directory
    // for any explicitly external URI referenced by an otherwise valid GLB.
    if (!Parser.IsValid() && FPaths::FileExists(FilePath))
    {
        TArray64<uint8> FileBytes;
        if (FFileHelper::LoadFileToArray(FileBytes, *FilePath))
        {
            FglTFRuntimeConfig FallbackConfig = Config;
            FallbackConfig.bSearchContentDir = false;
            if (FallbackConfig.bAllowExternalFiles && FallbackConfig.OverrideBaseDirectory.IsEmpty())
            {
                FallbackConfig.OverrideBaseDirectory = FPaths::GetPath(FilePath);
            }
            Parser = FglTFRuntimeParser::FromData(
                FileBytes.GetData(), FileBytes.Num(), FallbackConfig);
            if (Parser.IsValid())
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("glTFRuntime FromFilename failed but direct-data fallback succeeded. Path=%s"),
                    *FilePath);
            }
        }
    }

    ParserSlot.Release();
    // This continuation participates in the same shutdown drain as the worker that constructed
    // the parser, preventing a raw module callback from surviving DLL unload.
    FSafeFileIO::DispatchTrackedGameThread([]()
    {
        FglTFRuntimeSafety::NotifyGateAvailable_GameThread();
    });

    if (!Parser.IsValid())
    {
        return Fail(FString::Printf(
            TEXT("glTFRuntime rejected the GLB after validated filename and direct-data load attempts (size=%lld)"),
            IFileManager::Get().FileSize(*FilePath)));
    }

    ClearRecoverableFailure(FilePath);
    return Parser;
}

bool FglTFRuntimeSafety::ExecuteSynchronousOperation(
    const FString& Label,
    TFunctionRef<void()> Operation)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("Synchronous glTFRuntime work must run on the game thread")))
    {
        return false;
    }
    FString Reason;
    if (glTFRuntimeSafetyPrivate::IsShuttingDown() ||
        glTFRuntimeSafetyPrivate::IsCircuitOpen(&Reason) ||
        !glTFRuntimeSafetyPrivate::TryEnterNativePhase(
            glTFRuntimeSafetyPrivate::FState::ENativePhase::SynchronousOperation))
    {
        UE_LOG(LogTemp, Error,
            TEXT("Rejected synchronous glTFRuntime operation because the global native gate is busy. Label=%s Reason=%s"),
            *Label, Reason.IsEmpty() ? TEXT("another native operation is active") : *Reason);
        return false;
    }

    {
        ON_SCOPE_EXIT
        {
            glTFRuntimeSafetyPrivate::LeaveNativePhase(
                glTFRuntimeSafetyPrivate::FState::ENativePhase::SynchronousOperation);
        };
        Operation();
    }

    // Operation() and its plugin stack have fully returned and the scope guard has released the
    // gate. Pump synchronously to remove watchdog latency without leaving an untracked module
    // callback queued across shutdown/DLL unload.
    NotifyGateAvailable_GameThread();
    return true;
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
    glTFRuntimeSafetyPrivate::LeaveNativePhase(
        glTFRuntimeSafetyPrivate::FState::ENativePhase::MeshOperation);

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

        if (!glTFRuntimeSafetyPrivate::TryEnterNativePhase(
            glTFRuntimeSafetyPrivate::FState::ENativePhase::CacheClear))
        {
            return;
        }

        // ClearCache touches the parser's mutable cache and shares the process-wide native gate
        // with every parser constructor and mesh operation.
        Asset->ClearCache();
        if (Asset->IsRooted())
        {
            Asset->RemoveFromRoot();
        }
        Asset->ClearFlags(RF_Public | RF_Standalone);
        State.PendingAssetReleases.RemoveAtSwap(Index, 1, EAllowShrinking::No);
        glTFRuntimeSafetyPrivate::LeaveNativePhase(
            glTFRuntimeSafetyPrivate::FState::ENativePhase::CacheClear);
    }
}

void FglTFRuntimeSafety::NotifyGateAvailable_GameThread()
{
    check(IsInGameThread());
    ProcessPendingAssetReleases_GameThread();
    PumpQueue_GameThread();
}

void FglTFRuntimeSafety::ReportRecoverableFailure(
    const FString& FilePath,
    const FString& Reason)
{
    const FString NormalizedPath = glTFRuntimeSafetyPrivate::NormalizeFailureKey(FilePath);
    if (NormalizedPath.IsEmpty())
    {
        return;
    }

    glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();
    FScopeLock FailureScope(&State.FailureLock);
    glTFRuntimeSafetyPrivate::FFailureRecord& Record = State.Failures.FindOrAdd(NormalizedPath);

    // Editing/replacing a source GLB is a new input and must not inherit a previous file's failure
    // count merely because it kept the same path. This also makes Live Coding authoring retries sane.
    if (Record.Count > 0
        && glTFRuntimeSafetyPrivate::HasSourceFingerprintChanged(NormalizedPath, Record))
    {
        Record = glTFRuntimeSafetyPrivate::FFailureRecord();
    }

    if (Record.Count == 0)
    {
        glTFRuntimeSafetyPrivate::CaptureSourceFingerprint(
            NormalizedPath, Record.FileSize, Record.Timestamp);
    }
    ++Record.Count;
    Record.LastReason = Reason.Left(2048);

    if (Record.Count == 2)
    {
        UE_LOG(LogTemp, Error,
            TEXT("Quarantined model source/reference after %d recoverable failures. Key=%s Reason=%s"),
            Record.Count,
            *NormalizedPath,
            *Record.LastReason);
    }
}

void FglTFRuntimeSafety::ClearRecoverableFailure(const FString& FilePath)
{
    const FString NormalizedPath = glTFRuntimeSafetyPrivate::NormalizeFailureKey(FilePath);
    if (NormalizedPath.IsEmpty())
    {
        return;
    }

    glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();
    FScopeLock FailureScope(&State.FailureLock);
    State.Failures.Remove(NormalizedPath);
}

void FglTFRuntimeSafety::ResetRecoverableFailures()
{
    glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();
    FScopeLock FailureScope(&State.FailureLock);
    State.Failures.Reset();
}

bool FglTFRuntimeSafety::IsPathQuarantined(
    const FString& FilePath,
    FString* OutReason)
{
    const FString NormalizedPath = glTFRuntimeSafetyPrivate::NormalizeFailureKey(FilePath);
    if (NormalizedPath.IsEmpty())
    {
        return false;
    }

    glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();
    FScopeLock FailureScope(&State.FailureLock);

    glTFRuntimeSafetyPrivate::FFailureRecord* Record = State.Failures.Find(NormalizedPath);
    if (Record && Record->Count > 0
        && glTFRuntimeSafetyPrivate::HasSourceFingerprintChanged(NormalizedPath, *Record))
    {
        State.Failures.Remove(NormalizedPath);
        Record = nullptr;
        UE_LOG(LogTemp, Display,
            TEXT("Cleared stale glTF quarantine because the source changed. Path=%s"),
            *NormalizedPath);
    }

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
    NotifyGateAvailable_GameThread();
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

    while (State.ActiveOperations.Num() < glTFRuntimeSafetyPrivate::MaximumConcurrentNativeOperations &&
        glTFRuntimeSafetyPrivate::IsNativeGateIdle())
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

            if (!glTFRuntimeSafetyPrivate::TryEnterNativePhase(
                glTFRuntimeSafetyPrivate::FState::ENativePhase::MeshOperation))
            {
                // A worker parser acquired the gate between the idle probe and this claim.
                // Restore queue order; parser completion will pump the queue on the game thread.
                State.Queue.Insert(MoveTemp(Operation), 0);
                for (glTFRuntimeSafetyPrivate::FQueuedOperation& Rejected : RejectedBeforeStart)
                {
                    glTFRuntimeSafetyPrivate::RejectOperation(
                        Rejected,
                        TEXT("Operation owner/asset expired or the asset entered safe release before execution"));
                }
                return;
            }

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
