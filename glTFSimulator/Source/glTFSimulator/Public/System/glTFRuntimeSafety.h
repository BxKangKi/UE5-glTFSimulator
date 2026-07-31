// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file glTFRuntimeSafety.h
 * @brief Serializes high-risk glTFRuntime work and centralizes session quarantine decisions.
 */
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class FglTFRuntimeParser;
struct FglTFRuntimeConfig;

/**
 * Process-local safety coordinator for third-party glTF parser and mesh-build calls.
 *
 * Native access violations and memory corruption cannot be safely recovered by a C++ try/catch.
 * This service therefore reduces exposure by validating first, preventing overlapping parser/mesh
 * jobs, and quarantining repeatedly failing external files for the remainder of the session.
 */
class GLTFSIMULATOR_API FglTFRuntimeSafety
{
public:
    using FQueuedStart = TFunction<void(uint64)>;
    using FRejected = TFunction<void(const FString&)>;

    /** Creates a parser under a process-wide worker lock. Never call this from the game thread. */
    static TSharedPtr<FglTFRuntimeParser> CreateParserSerialized(
        const FString& FilePath,
        const FglTFRuntimeConfig& Config);

    /**
     * Enqueues one game-thread glTFRuntime mesh operation. Start receives a ticket that the caller
     * must release through CompleteOperation from every success, failure, and cancellation callback.
     */
    static uint64 EnqueueOperation(
        UObject* Owner,
        const FString& Label,
        FQueuedStart Start,
        FRejected Rejected = FRejected());

    /** Releases the active ticket and starts the next valid queued operation. */
    static void CompleteOperation(uint64 Ticket);

    /** Removes queued work owned by Owner. Active native work is allowed to finish safely. */
    static void CancelQueuedOperations(UObject* Owner);

    /** Records a recoverable failure and quarantines a path after repeated failures. */
    static void ReportRecoverableFailure(const FString& FilePath, const FString& Reason);

    /** Returns true when a path has been quarantined for the current process. */
    static bool IsPathQuarantined(const FString& FilePath, FString* OutReason = nullptr);

    /** Returns true after a native operation timeout opens the session circuit breaker. */
    static bool IsCircuitOpen(FString* OutReason = nullptr);

    /** Clears queued state and rejects new operations during module shutdown. */
    static void BeginShutdown();

    /** Waits for an active native operation to return its terminal callback before module unload. */
    static bool FlushPendingOperations(double TimeoutSeconds);

    /** Returns one for an active operation plus the number of operations waiting in the queue. */
    static int32 GetPendingOperationCount();

private:
    static void PumpQueue_GameThread();
    static bool TickWatchdog(float DeltaSeconds);
};
