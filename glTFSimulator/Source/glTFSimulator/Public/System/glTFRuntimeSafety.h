// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file glTFRuntimeSafety.h
 * @brief Coordinates bounded parallel glTFRuntime work and safe runtime-asset cache teardown.
 */
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class FglTFRuntimeParser;
class UglTFRuntimeAsset;
struct FglTFRuntimeConfig;

/**
 * Process-local safety coordinator for third-party glTF parser and mesh-build calls.
 *
 * glTFRuntime keeps mutable caches inside each parser. Operations that share one
 * UglTFRuntimeAsset therefore remain strictly serialized, while operations belonging to
 * different assets may run in parallel under a conservative global memory-pressure cap.
 *
 * Native access violations and memory corruption cannot be recovered with C++ exceptions.
 * This coordinator instead validates external files before entry, keeps UObject work on the
 * game thread, delays cache destruction until native callbacks have finished, and quarantines
 * repeatedly failing files for the remainder of the process.
 */
class GLTFSIMULATOR_API FglTFRuntimeSafety
{
public:
    using FQueuedStart = TFunction<void(uint64)>;
    using FRejected = TFunction<void(const FString&)>;

    /**
     * Creates an independent parser on a worker thread.
     *
     * Parser creation is bounded but no longer globally serialized, so different GLB files can
     * parse concurrently without allowing an unbounded peak-memory spike.
     */
    static TSharedPtr<FglTFRuntimeParser> CreateParserSafely(
        const FString& FilePath,
        const FglTFRuntimeConfig& Config);

    /**
     * Enqueues one game-thread glTFRuntime operation for Asset.
     *
     * Start receives a ticket that the caller must return through CompleteOperation from every
     * terminal success/failure/cancellation callback. The same Asset never has two active native
     * operations, but different assets can use separate slots concurrently.
     */
    static uint64 EnqueueOperation(
        UObject* Owner,
        UglTFRuntimeAsset* Asset,
        const FString& Label,
        FQueuedStart Start,
        FRejected Rejected = FRejected());

    /** Releases an active ticket, performs any now-safe cache teardown, and pumps queued work. */
    static void CompleteOperation(uint64 Ticket);

    /** Removes queued work owned by Owner. Game-thread only; active native work is allowed to finish. */
    static void CancelQueuedOperations(UObject* Owner);

    /**
     * Requests final release of a runtime asset and its parser cache.
     *
     * This function is idempotent and game-thread only. It rejects not-yet-started work for the
     * asset, keeps the UObject strongly referenced, and calls ClearCache only after the asset has
     * no active native operation. Callers may drop their own UPROPERTY immediately afterwards.
     */
    static void RequestAssetRelease(UglTFRuntimeAsset* Asset);

    /** Records a recoverable failure and quarantines a path after repeated failures. */
    static void ReportRecoverableFailure(const FString& FilePath, const FString& Reason);

    /** Returns true when a path has been quarantined for the current process. */
    static bool IsPathQuarantined(const FString& FilePath, FString* OutReason = nullptr);

    /** Returns true after a native-operation timeout opens the session circuit breaker. */
    static bool IsCircuitOpen(FString* OutReason = nullptr);

    /** Stops new work, rejects queued operations, and begins draining active callbacks. */
    static void BeginShutdown();

    /** Waits for active native operations and deferred cache releases before module unload. */
    static bool FlushPendingOperations(double TimeoutSeconds);

    /** Returns queued + active operations + deferred asset releases. Game-thread only. */
    static int32 GetPendingOperationCount();

private:
    static void PumpQueue_GameThread();
    static void ProcessPendingAssetReleases_GameThread();
    static bool TickWatchdog(float DeltaSeconds);
};
