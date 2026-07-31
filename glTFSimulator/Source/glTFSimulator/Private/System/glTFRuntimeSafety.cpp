// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file glTFRuntimeSafety.cpp
 * @brief Implements global serialization and session quarantine around glTFRuntime entry points.
 */
#include "System/glTFRuntimeSafety.h"

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeCounter.h"
#include "glTFRuntimeParser.h"
#include "Misc/ScopeLock.h"
#include "System/GlbValidation.h"
#include "System/SafeFileIO.h"

namespace glTFRuntimeSafetyPrivate
{
    struct FQueuedOperation
    {
        uint64 Ticket = 0;
        TWeakObjectPtr<UObject> Owner;
        FString Label;
        FglTFRuntimeSafety::FQueuedStart Start;
        FglTFRuntimeSafety::FRejected Rejected;
    };

    struct FFailureRecord
    {
        int32 Count = 0;
        FString LastReason;
    };

    struct FState
    {
        // Parser and failure maps are accessed from worker threads, so each has a dedicated lock.
        FCriticalSection ParserLock;
        FCriticalSection FailureLock;
        TMap<FString, FFailureRecord> Failures;

        // Control flags are read by both game-thread and worker-thread entry points.
        FThreadSafeCounter CircuitOpenFlag;
        FThreadSafeCounter ShuttingDownFlag;
        FCriticalSection ControlLock;
        FString CircuitReason;

        // Queue and active-operation fields are game-thread-only by design.
        TArray<FQueuedOperation> Queue;
        uint64 NextTicket = 1;
        uint64 ActiveTicket = 0;
        double ActiveStartedAtSeconds = 0.0;
        FString ActiveLabel;
        FTSTicker::FDelegateHandle WatchdogHandle;
    };

    FState& GetState()
    {
        static FState State;
        return State;
    }

    void RejectOperation(FQueuedOperation& Operation, const FString& Reason)
    {
        if (Operation.Rejected)
        {
            Operation.Rejected(Reason);
        }
    }

    constexpr int32 MaximumQueuedOperations = 4096;

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
}

TSharedPtr<FglTFRuntimeParser> FglTFRuntimeSafety::CreateParserSerialized(
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
        UE_LOG(LogTemp, Error, TEXT("Refused glTFRuntime parser because the safety circuit is open. Path=%s Reason=%s"),
            *FilePath, *CoordinatorReason);
        return nullptr;
    }

    FString QuarantineReason;
    if (IsPathQuarantined(FilePath, &QuarantineReason))
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT("Refused quarantined glTF file. Path=%s Reason=%s"),
            *FilePath,
            *QuarantineReason);
        return nullptr;
    }

    // The plugin has mutable parser/cache state. A single construction lock avoids concurrent
    // parser allocation and third-party decoder initialization across character/world actors.
    FScopeLock ParserScope(&glTFRuntimeSafetyPrivate::GetState().ParserLock);

    // Recheck after waiting for the parser lock; shutdown or the circuit breaker may have opened.
    if (glTFRuntimeSafetyPrivate::IsShuttingDown() ||
        glTFRuntimeSafetyPrivate::IsCircuitOpen(&CoordinatorReason))
    {
        return nullptr;
    }
    return FglTFRuntimeParser::FromFilename(FilePath, Config);
}

uint64 FglTFRuntimeSafety::EnqueueOperation(
    UObject* Owner,
    const FString& Label,
    FQueuedStart Start,
    FRejected Rejected)
{
    if (!IsInGameThread())
    {
        TWeakObjectPtr<UObject> WeakOwner(Owner);
        FSafeFileIO::DispatchTrackedGameThread(
            [WeakOwner, Label, Start = MoveTemp(Start), Rejected = MoveTemp(Rejected)]() mutable
            {
                FglTFRuntimeSafety::EnqueueOperation(
                    WeakOwner.Get(),
                    Label,
                    MoveTemp(Start),
                    MoveTemp(Rejected));
            });
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
    if (glTFRuntimeSafetyPrivate::IsShuttingDown() ||
        glTFRuntimeSafetyPrivate::IsCircuitOpen(&CoordinatorReason) ||
        !IsValid(Owner) || !Start || State.Queue.Num() >= glTFRuntimeSafetyPrivate::MaximumQueuedOperations)
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
            else if (!IsValid(Owner) || !Start)
            {
                Rejected(TEXT("Operation owner or start callback is invalid"));
            }
            else
            {
                Rejected(TEXT("The serialized glTFRuntime queue reached its safety limit"));
            }
        }
        return 0;
    }

    glTFRuntimeSafetyPrivate::FQueuedOperation Operation;
    Operation.Ticket = State.NextTicket++;
    Operation.Owner = Owner;
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
        FSafeFileIO::DispatchTrackedGameThread([Ticket]()
        {
            FglTFRuntimeSafety::CompleteOperation(Ticket);
        });
        return;
    }

    glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();
    if (Ticket == 0 || State.ActiveTicket != Ticket)
    {
        return;
    }

    State.ActiveTicket = 0;
    State.ActiveStartedAtSeconds = 0.0;
    State.ActiveLabel.Reset();
    PumpQueue_GameThread();
}

void FglTFRuntimeSafety::CancelQueuedOperations(UObject* Owner)
{
    if (!IsInGameThread())
    {
        TWeakObjectPtr<UObject> WeakOwner(Owner);
        FSafeFileIO::DispatchTrackedGameThread([WeakOwner]()
        {
            FglTFRuntimeSafety::CancelQueuedOperations(WeakOwner.Get());
        });
        return;
    }

    if (!Owner)
    {
        return;
    }

    glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();
    for (int32 Index = State.Queue.Num() - 1; Index >= 0; --Index)
    {
        if (State.Queue[Index].Owner.Get() == Owner)
        {
            glTFRuntimeSafetyPrivate::FQueuedOperation Removed = MoveTemp(State.Queue[Index]);
            State.Queue.RemoveAtSwap(Index, 1, EAllowShrinking::No);
            glTFRuntimeSafetyPrivate::RejectOperation(Removed, TEXT("Operation was cancelled before it started"));
        }
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
        UE_LOG(
            LogTemp,
            Error,
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

    for (glTFRuntimeSafetyPrivate::FQueuedOperation& Operation : State.Queue)
    {
        glTFRuntimeSafetyPrivate::RejectOperation(Operation, TEXT("glTFRuntime coordinator is shutting down"));
    }
    State.Queue.Reset();
    if (State.WatchdogHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(State.WatchdogHandle);
        State.WatchdogHandle.Reset();
    }
}


bool FglTFRuntimeSafety::FlushPendingOperations(const double TimeoutSeconds)
{
    const double StartSeconds = FPlatformTime::Seconds();
    while (GetPendingOperationCount() > 0)
    {
        if (TimeoutSeconds >= 0.0 && FPlatformTime::Seconds() - StartSeconds >= TimeoutSeconds)
        {
            return false;
        }

        // Native plugin callbacks are marshalled to the game thread. During module shutdown, pump
        // that queue so a completed worker can release its active ticket before this DLL unloads.
        if (IsInGameThread())
        {
            FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        }
        FPlatformProcess::SleepNoStats(0.005f);
    }
    return true;
}

int32 FglTFRuntimeSafety::GetPendingOperationCount()
{
    const glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();
    return State.Queue.Num() + (State.ActiveTicket != 0 ? 1 : 0);
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
    if (glTFRuntimeSafetyPrivate::IsCircuitOpen() || State.ActiveTicket == 0 || State.ActiveStartedAtSeconds <= 0.0)
    {
        return true;
    }

    const double ElapsedSeconds = FPlatformTime::Seconds() - State.ActiveStartedAtSeconds;
    if (ElapsedSeconds < NativeOperationTimeoutSeconds)
    {
        return true;
    }

    const FString CircuitReason = FString::Printf(
        TEXT("glTFRuntime circuit breaker opened after operation [%llu] '%s' exceeded %.0f seconds"),
        State.ActiveTicket,
        *State.ActiveLabel,
        NativeOperationTimeoutSeconds);
    glTFRuntimeSafetyPrivate::OpenCircuit(CircuitReason);
    UE_LOG(LogTemp, Error, TEXT("%s. No additional glTFRuntime jobs will start this session."), *CircuitReason);

    for (glTFRuntimeSafetyPrivate::FQueuedOperation& Operation : State.Queue)
    {
        glTFRuntimeSafetyPrivate::RejectOperation(Operation, CircuitReason);
    }
    State.Queue.Reset();
    return true;
}

void FglTFRuntimeSafety::PumpQueue_GameThread()
{
    check(IsInGameThread());
    glTFRuntimeSafetyPrivate::FState& State = glTFRuntimeSafetyPrivate::GetState();
    if (glTFRuntimeSafetyPrivate::IsShuttingDown() ||
        glTFRuntimeSafetyPrivate::IsCircuitOpen() ||
        State.ActiveTicket != 0)
    {
        return;
    }

    while (!State.Queue.IsEmpty())
    {
        glTFRuntimeSafetyPrivate::FQueuedOperation Operation = MoveTemp(State.Queue[0]);
        State.Queue.RemoveAt(0, 1, EAllowShrinking::No);

        if (!Operation.Owner.IsValid() || !Operation.Start)
        {
            glTFRuntimeSafetyPrivate::RejectOperation(Operation, TEXT("Operation owner expired before execution"));
            continue;
        }

        State.ActiveTicket = Operation.Ticket;
        State.ActiveStartedAtSeconds = FPlatformTime::Seconds();
        State.ActiveLabel = Operation.Label;
        UE_LOG(LogTemp, Verbose, TEXT("Starting serialized glTFRuntime operation [%llu] %s"), Operation.Ticket, *Operation.Label);
        Operation.Start(Operation.Ticket);
        return;
    }
}
