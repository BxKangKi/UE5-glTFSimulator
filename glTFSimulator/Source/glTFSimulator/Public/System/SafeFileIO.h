// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file SafeFileIO.h
 * @brief Bounded, asynchronous, crash-resilient file and JSON I/O used by runtime data systems.
 *
 * This utility deliberately keeps raw disk access and JSON parsing away from the game thread.
 * JSON saves use a same-directory temporary file plus a recoverable backup so a process exit
 * cannot leave the only valid copy half-written.
 */
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Templates/Function.h"

/** Result code shared by safe file and JSON operations. */
enum class ESafeFileIOStatus : uint8
{
    Success,
    RecoveredFromBackup,
    Cancelled,
    Superseded,
    ShuttingDown,
    InvalidPath,
    Missing,
    TooLarge,
    ReadFailed,
    DecodeFailed,
    InvalidJson,
    ValidationFailed,
    SerializeFailed,
    WriteFailed,
    CommitFailed
};

/** Bounds applied before and after Unreal's JSON deserializer is invoked. */
struct GLTFSIMULATOR_API FSafeJsonLimits
{
    /** Maximum encoded JSON file size. */
    int64 MaxFileBytes = 64ll * 1024ll * 1024ll;

    /** Maximum object/array nesting depth accepted by the pre-scan and DOM validation. */
    int32 MaxDepth = 64;

    /** Maximum total JSON values visited after deserialization. */
    int32 MaxValues = 1000000;

    /** Maximum entries accepted in any single object or array. */
    int32 MaxContainerEntries = 250000;

    /** Maximum number of TCHAR units accepted in one key or string value. */
    int32 MaxStringCharacters = 4 * 1024 * 1024;

    /** Maximum characters accepted in one unquoted primitive token such as a number. */
    int32 MaxPrimitiveCharacters = 1024;

    /** When true, a valid .bak copy is used if the primary file is missing or corrupt. */
    bool bAllowBackupRecovery = true;
};

/** Immutable result returned by a bounded JSON load. */
struct GLTFSIMULATOR_API FSafeJsonLoadResult
{
    ESafeFileIOStatus Status = ESafeFileIOStatus::ReadFailed;
    FString Path;
    FString Error;
    TSharedPtr<FJsonObject> JsonObject;
    bool bRecoveredFromBackup = false;

    bool IsSuccess() const
    {
        return JsonObject.IsValid() &&
            (Status == ESafeFileIOStatus::Success || Status == ESafeFileIOStatus::RecoveredFromBackup);
    }
};

/** Immutable result returned by an atomic write. */
struct GLTFSIMULATOR_API FSafeFileWriteResult
{
    ESafeFileIOStatus Status = ESafeFileIOStatus::WriteFailed;
    FString Path;
    FString Error;
    int64 BytesWritten = 0;

    bool IsSuccess() const
    {
        return Status == ESafeFileIOStatus::Success;
    }
};

/** Immutable result returned by a bounded binary read. */
struct GLTFSIMULATOR_API FSafeBinaryLoadResult
{
    ESafeFileIOStatus Status = ESafeFileIOStatus::ReadFailed;
    FString Path;
    FString Error;
    TArray<uint8> Data;

    bool IsSuccess() const
    {
        return Status == ESafeFileIOStatus::Success ||
            Status == ESafeFileIOStatus::RecoveredFromBackup;
    }
};

/**
 * Non-UObject service for safe file operations.
 *
 * Blocking functions are intended for worker threads. Async functions perform disk/JSON work on
 * the task graph and marshal their completion callback to the game thread.
 */
class GLTFSIMULATOR_API FSafeFileIO
{
public:
    using FJsonLoadCallback = TFunction<void(FSafeJsonLoadResult)>;
    using FWriteCallback = TFunction<void(FSafeFileWriteResult)>;
    using FBinaryLoadCallback = TFunction<void(FSafeBinaryLoadResult)>;
    using FTrackedTask = TFunction<void()>;

    /** Normalizes a path to a full standard filename without resolving asset package names. */
    static FString NormalizeFilePath(const FString& Path);

    /**
     * Runs a pure-data task on the shared worker pool while keeping the game module alive.
     * Worker code must not access UObjects and should marshal results with DispatchTrackedGameThread.
     */
    static bool RunTrackedWorker(FTrackedTask Task);

    /**
     * Queues a game-thread continuation whose lifetime is also included in shutdown draining.
     * Returns false when shutdown has already started and the continuation was suppressed.
     */
    static bool DispatchTrackedGameThread(FTrackedTask Task);

    /** Returns true after BeginShutdown has stopped new asynchronous work. */
    static bool IsShuttingDown();

    /** Parses and validates an in-memory JSON string without touching the filesystem. */
    static FSafeJsonLoadResult ParseJsonText(
        const FString& JsonText,
        const FString& SourceLabel,
        const FSafeJsonLimits& Limits = FSafeJsonLimits());

    /** Strictly decodes UTF-8 bytes, then parses them under the same JSON limits. */
    static FSafeJsonLoadResult ParseJsonUtf8Bytes(
        const TArray<uint8>& Utf8Bytes,
        const FString& SourceLabel,
        const FSafeJsonLimits& Limits = FSafeJsonLimits());

    /** Loads a JSON object with size/depth/value bounds and optional .bak recovery. */
    static FSafeJsonLoadResult LoadJsonBlocking(
        const FString& Path,
        const FSafeJsonLimits& Limits = FSafeJsonLimits());

    /** Runs LoadJsonBlocking on a worker and invokes Callback on the game thread. */
    static void LoadJsonAsync(
        const FString& Path,
        FJsonLoadCallback Callback,
        const FSafeJsonLimits& Limits = FSafeJsonLimits());

    /** Serializes and atomically commits JSON through temp + backup files. */
    static FSafeFileWriteResult SaveJsonBlocking(
        const TSharedRef<FJsonObject>& JsonObject,
        const FString& Path,
        int64 MaxOutputBytes = 64ll * 1024ll * 1024ll);

    /** Keeps an immutable JSON snapshot alive, then serializes and atomically commits it on a worker. */
    static void SaveJsonAsync(
        const TSharedRef<FJsonObject>& JsonObject,
        const FString& Path,
        FWriteCallback Callback = FWriteCallback(),
        int64 MaxOutputBytes = 64ll * 1024ll * 1024ll);

    /** Reads a bounded binary file. This blocking form is intended for worker threads. */
    static FSafeBinaryLoadResult LoadBinaryBlocking(const FString& Path, int64 MaxBytes);

    /** Reads a bounded binary file on a worker and returns on the game thread. */
    static void LoadBinaryAsync(const FString& Path, int64 MaxBytes, FBinaryLoadCallback Callback);

    /** Atomically commits arbitrary bytes using the same temp + backup transaction as JSON. */
    static FSafeFileWriteResult SaveBinaryBlocking(
        const TArray<uint8>& Data,
        const FString& Path,
        int64 MaxOutputBytes);

    /** Copies bytes and atomically commits them on a worker. */
    static void SaveBinaryAsync(
        const TArray<uint8>& Data,
        const FString& Path,
        int64 MaxOutputBytes,
        FWriteCallback Callback = FWriteCallback());

    /** Appends text on a worker using a per-file lock; intended for diagnostic logs only. */
    static void AppendTextAsync(
        const FString& Text,
        const FString& Path,
        FWriteCallback Callback = FWriteCallback());

    /** Deletes filesystem paths on a tracked worker; missing files are treated as success. */
    static void DeleteFilesAsync(
        const TArray<FString>& Paths,
        FWriteCallback Callback = FWriteCallback());

    /** Stops accepting new jobs and suppresses callbacks queued after module shutdown begins. */
    static void BeginShutdown();

    /** Waits for active disk jobs to drain. Returns false if TimeoutSeconds elapsed. */
    static bool FlushPendingOperations(double TimeoutSeconds);

    /** Returns the number of worker operations currently tracked by this service. */
    static int32 GetPendingOperationCount();

    /** Removes stale temporary transaction files left by a prior hard termination. */
    static void CleanupStaleTemporaryFiles(const FString& RootDirectory, double OlderThanHours = 24.0);
};
