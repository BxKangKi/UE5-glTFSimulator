// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * File role: glTFRuntimeSafety.h
 * 역할: glTFRuntime의 native 작업과 해제를 직렬 조정합니다.
 * 핵심 기능: 작업 티켓·대기 큐, GC 참조 보호, 지연 cache 해제, 종료 drain.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

/**
 * @file glTFRuntimeSafety.h
 * @brief Serializes native glTFRuntime work and coordinates safe runtime-asset cache teardown.
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
 * glTFRuntime keeps mutable caches and runtime mesh-build state inside its parsers. Native
 * operations are globally serialized: this is deliberately conservative, but it prevents the
 * plugin's multi-file startup and LOD builders from overlapping unsafe native allocations.
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
     * terminal success/failure/cancellation callback. Only one native operation is active process-
     * wide, including when the operations belong to different runtime assets.
     */
    static uint64 EnqueueOperation(
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

private:
    static void NotifyGateAvailable_GameThread();
    static void PumpQueue_GameThread();
    static void ProcessPendingAssetReleases_GameThread();
    static bool TickWatchdog(float DeltaSeconds);
};
