// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * File role: glTFRuntimeSafety.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

/**
 * @file glTFRuntimeSafety.h
 * @brief Bounds parallel RuntimeLOD builds while coordinating exclusive parser/cache teardown.
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
 * glTFRuntime keeps mutable caches and runtime mesh-build state inside its parsers. Source parser
 * construction, synchronous calls and cache teardown remain exclusive, while baked RuntimeLOD
 * finalizers use independent parser/build contexts and therefore run with bounded concurrency.
 *
 * Native access violations and memory corruption cannot be recovered with C++ exceptions.
 * This coordinator instead validates external files before entry, keeps UObject work on the
 * game thread, delays cache destruction until native callbacks have finished, and quarantines
 * repeatedly failing files. Authoring retries can explicitly clear stale failure history, and a
 * changed source file automatically invalidates its previous quarantine record.
 */
class GLTFSIMULATOR_API FglTFRuntimeSafety
{
public:
    using FQueuedStart = TFunction<void(uint64)>;
    using FRejected = TFunction<void(const FString&)>;

    /**
     * Creates an independent parser on a worker thread.
     *
     * Parser creation is globally serialized. Pure file validation may still run in parallel, but
     * only one third-party parser constructor is allowed to allocate native state at a time.
     */
    static TSharedPtr<FglTFRuntimeParser> CreateParserSafely(
        const FString& FilePath,
        const FglTFRuntimeConfig& Config,
        FString* OutError = nullptr);

    /**
     * Runs one short, synchronous glTFRuntime call on the game thread.
     *
     * This is the only entry point used by synchronous vehicle, Static, and weapon loading. It
     * shares the same process-wide gate as parser construction, queued mesh finalization, and
     * ClearCache. The call is rejected instead of blocking the game thread when asynchronous
     * native work already owns the gate.
     */
    static bool ExecuteSynchronousOperation(const FString& Label, TFunctionRef<void()> Operation);

    /**
     * Enqueues one game-thread glTFRuntime operation for Asset.
     *
     * Start receives a ticket that the caller must return through CompleteOperation from every
     * terminal success/failure/cancellation callback. Independent runtime assets may execute in
     * parallel up to the configured mesh-build concurrency limit.
     */
    static uint64 EnqueueOperation(
        UObject* Owner,
        UglTFRuntimeAsset* Asset,
        const FString& Label,
        FQueuedStart Start,
        FRejected Rejected = FRejected());

    /**
     * Enqueues a parser/source-capture operation that must not overlap any mesh finalizer.
     * Use this only for mutable source-parser work; baked RuntimeLOD finalizers should use the
     * bounded-parallel EnqueueOperation path above.
     */
    static uint64 EnqueueExclusiveOperation(
        UObject* Owner,
        UglTFRuntimeAsset* Asset,
        const FString& Label,
        FQueuedStart Start,
        FRejected Rejected = FRejected());

    /** Releases an active ticket, performs any now-safe cache teardown, and pumps queued work. */
    static void CompleteOperation(uint64 Ticket);

    /**
     * Defers ticket release to the next game-thread task.
     *
     * glTFRuntime invokes project delegates before unregistering its internal FGCObject. Terminal
     * plugin callbacks must use this method so ClearCache cannot run while that wrapper is unwinding.
     */
    static void CompleteOperationAfterCallback(uint64 Ticket);

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

    /** Clears accumulated recoverable failures for one source/reference after a successful use. */
    static void ClearRecoverableFailure(const FString& FilePath);

    /** Clears process-local recoverable failure history before an explicit authoring retry. */
    static void ResetRecoverableFailures();

    /** Returns true when a path is quarantined and its source fingerprint has not changed. */
    static bool IsPathQuarantined(const FString& FilePath, FString* OutReason = nullptr);

    /** Returns true after a native-operation timeout opens the session circuit breaker. */
    static bool IsCircuitOpen(FString* OutReason = nullptr);

    /** Stops new work, rejects queued operations, and begins draining active callbacks. */
    static void BeginShutdown();

    /** Waits for active native operations and deferred cache releases before module unload. */
    static bool FlushPendingOperations(double TimeoutSeconds);

    /** Returns queued + active operations + deferred asset releases. Game-thread only. */
    static int32 GetPendingOperationCount();

    /** Effective bounded-parallel RuntimeLOD finalizer count. Game-thread only. */
    static int32 GetMeshBuildConcurrencyLimit();

private:
    static uint64 EnqueueOperationInternal(
        UObject* Owner,
        UglTFRuntimeAsset* Asset,
        const FString& Label,
        FQueuedStart Start,
        FRejected Rejected,
        bool bExclusive);
    static void NotifyGateAvailable_GameThread();
    static void PumpQueue_GameThread();
    static void ProcessPendingAssetReleases_GameThread();
    static bool TickWatchdog(float DeltaSeconds);
};
