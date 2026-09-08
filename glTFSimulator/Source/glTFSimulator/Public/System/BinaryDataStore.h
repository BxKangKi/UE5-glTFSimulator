// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "System/SafeFileIO.h"
#include "System/SceneDatabaseTypes.h"
#include "World/PlayerData.h"

/** One extensionless /cache/<JSON base filename> file. */
struct GLTFSIMULATOR_API FModelCacheData
{
    /** Upper-case SHA-1 of the source model bytes. */
    FString ModelHash;

    /** Exact author JSON used to build this cache; db.dat UUID validation selects the file. */
    FString DefinitionJson;

    /** Model-space center of the union of all renderable nodes. */
    FVector Center = FVector::ZeroVector;

    /** Model-space half size of the union of all renderable nodes. */
    FVector Extent = FVector::ZeroVector;

    /** Unscaled local-space full size for every base mesh key in the GLB. */
    TMap<FName, FVector> MeshSizes;

    bool IsSane() const;
};

/** 512 m voxel index. Coordinates are chunk numbers, never world-space metres. */
struct GLTFSIMULATOR_API FWorldChunkCoordinate
{
    int32 X = 0;
    int32 Y = 0;
    int32 Z = 0;

    bool operator==(const FWorldChunkCoordinate& Other) const
    {
        return X == Other.X && Y == Other.Y && Z == Other.Z;
    }

    FString ToPrefabFileName() const
    {
        return FString::Printf(TEXT("chunk.%d.%d.%d.dat"), X, Y, Z);
    }

    FString ToEntityFileName() const
    {
        return FString::Printf(TEXT("entity.%d.%d.%d.dat"), X, Y, Z);
    }
};

FORCEINLINE uint32 GetTypeHash(const FWorldChunkCoordinate& Value)
{
    return HashCombine(HashCombine(::GetTypeHash(Value.X), ::GetTypeHash(Value.Y)), ::GetTypeHash(Value.Z));
}

enum class EWorldObjectStorageKind : uint8
{
    Prefab,
    Entity
};

/** One placed object. StorageKind is assigned from its containing file and is not serialized. */
struct GLTFSIMULATOR_API FWorldChunkObject
{
    FGuid UUID;
    FVector Location = FVector::ZeroVector;
    FQuat Rotation = FQuat::Identity;
    FVector Scale = FVector::OneVector;
    FVector Velocity = FVector::ZeroVector;
    /** Required to restore frozen physics without losing rotational motion. */
    FVector AngularVelocity = FVector::ZeroVector;
    EWorldObjectStorageKind StorageKind = EWorldObjectStorageKind::Prefab;
};

/** db.dat row. Paths are world-root-relative and UUID maps one-to-one to JSON.UUID. */
struct GLTFSIMULATOR_API FModelDatabaseEntry
{
    FGuid UUID;
    FString Cache;
    FString Json;
};

/** Mutable state written atomically to data/level.dat. */
struct GLTFSIMULATOR_API FLevelRuntimeData
{
    float WorldTime = 0.0f;
    FString SelectedPlayer;
    TArray<FWorldPlayerRecord> Players;
};

/**
 * Versioned binary persistence for program-owned .dat state and extensionless model caches.
 *
 * Every payload has a magic, file-kind, schema version, exact byte count, and CRC32. Model caches
 * payloads additionally carry a bounded zlib-compressed block with its own raw-size and CRC checks. Disk commits
 * use FSafeFileIO's verified single-primary temporary-file transaction. Deserializers validate every count,
 * string length, enum, number, and transform before publishing data to gameplay code.
 */
class GLTFSIMULATOR_API FBinaryDataStore
{
public:
    static constexpr int64 MaxModelCacheBytes = 128ll * 1024ll * 1024ll;
    static constexpr int64 MaxModelCacheRawBytes = 256ll * 1024ll * 1024ll;
    static constexpr int64 MaxWorldChunkDatBytes = 256ll * 1024ll * 1024ll;
    static constexpr int64 MaxModelDatabaseDatBytes = 64ll * 1024ll * 1024ll;
    static constexpr int64 MaxSceneDatabaseDatBytes = 64ll * 1024ll * 1024ll;
    static constexpr int64 MaxLevelDatBytes = 64ll * 1024ll * 1024ll;

    /** Streams a file through SHA-1 without loading the whole model into memory. Worker-thread safe. */
    static bool ComputeFileSha1(const FString& FilePath, FString& OutHash, FString& OutError);

    /** Loads and validates a model cache. bOutHashMismatch distinguishes stale data from corruption. */
    static bool LoadModelCache(
        const FString& CachePath,
        const FString& ExpectedHash,
        FModelCacheData& OutCache,
        FString& OutError,
        bool& bOutHashMismatch);

    static FSafeFileWriteResult SaveModelCacheBlocking(
        const FString& CachePath,
        const FModelCacheData& Cache);

    /** Removes the primary and abandoned transaction files for a stale cache. */
    static bool InvalidateCacheFile(const FString& CachePath, FString& OutError);

    static bool LoadWorldChunk(const FString& DatPath, TArray<FWorldChunkObject>& OutObjects, FString& OutError);
    static void LoadWorldChunkAsync(const FString& DatPath, TFunction<void(bool, TArray<FWorldChunkObject>, FString)> Callback);
    static FSafeFileWriteResult SaveWorldChunkBlocking(const FString& DatPath, const TArray<FWorldChunkObject>& Objects);
    static void SaveWorldChunkAsync(const FString& DatPath, const TArray<FWorldChunkObject>& Objects,
        FSafeFileIO::FWriteCallback Callback = FSafeFileIO::FWriteCallback());

    static bool LoadModelDatabase(const FString& DatPath, TArray<FModelDatabaseEntry>& OutEntries, FString& OutError);
    static FSafeFileWriteResult SaveModelDatabaseBlocking(const FString& DatPath, const TArray<FModelDatabaseEntry>& Entries);
    static void SaveModelDatabaseAsync(const FString& DatPath, const TArray<FModelDatabaseEntry>& Entries,
        FSafeFileIO::FWriteCallback Callback = FSafeFileIO::FWriteCallback());

    static bool LoadSceneDatabase(const FString& DatPath, TArray<FSceneDatabaseEntry>& OutEntries, FString& OutError);
    static void LoadSceneDatabaseAsync(const FString& DatPath,
        TFunction<void(bool, bool, TArray<FSceneDatabaseEntry>, FString)> Callback);
    static FSafeFileWriteResult SaveSceneDatabaseBlocking(
        const FString& DatPath, const TArray<FSceneDatabaseEntry>& Entries);
    static void SaveSceneDatabaseAsync(const FString& DatPath, const TArray<FSceneDatabaseEntry>& Entries,
        FSafeFileIO::FWriteCallback Callback = FSafeFileIO::FWriteCallback());

    static bool LoadLevel(const FString& DatPath, FLevelRuntimeData& OutData, FString& OutError);
    static FSafeFileWriteResult SaveLevelBlocking(const FString& DatPath, const FLevelRuntimeData& Data);
    static void SaveLevelAsync(const FString& DatPath, const FLevelRuntimeData& Data,
        FSafeFileIO::FWriteCallback Callback = FSafeFileIO::FWriteCallback());

private:
    static bool SerializeModelCache(const FModelCacheData& Cache, TArray<uint8>& OutBytes, FString& OutError);
    static bool DeserializeModelCache(const TArray<uint8>& Bytes, FModelCacheData& OutCache, FString& OutError);
    static bool SerializeWorldChunk(const TArray<FWorldChunkObject>& Objects, TArray<uint8>& OutBytes, FString& OutError);
    static bool DeserializeWorldChunk(const TArray<uint8>& Bytes, TArray<FWorldChunkObject>& OutObjects, FString& OutError);
    static bool SerializeModelDatabase(const TArray<FModelDatabaseEntry>& Entries, TArray<uint8>& OutBytes, FString& OutError);
    static bool DeserializeModelDatabase(const TArray<uint8>& Bytes, TArray<FModelDatabaseEntry>& OutEntries, FString& OutError);
    static bool SerializeSceneDatabase(const TArray<FSceneDatabaseEntry>& Entries, TArray<uint8>& OutBytes, FString& OutError);
    static bool DeserializeSceneDatabase(const TArray<uint8>& Bytes, TArray<FSceneDatabaseEntry>& OutEntries, FString& OutError);
    static bool SerializeLevel(const FLevelRuntimeData& Data, TArray<uint8>& OutBytes, FString& OutError);
    static bool DeserializeLevel(const TArray<uint8>& Bytes, FLevelRuntimeData& OutData, FString& OutError);
};
