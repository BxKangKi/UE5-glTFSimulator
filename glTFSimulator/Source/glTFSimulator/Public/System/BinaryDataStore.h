// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "System/SafeFileIO.h"
#include "World/PlacementTypes.h"
#include "World/PlayerData.h"

/** Zlib-compressed program-owned size cache written beside one glTF/GLB source as <model>.scz. */
struct GLTFSIMULATOR_API FModelCacheData
{
    /** Upper-case SHA-1 of the source model bytes. */
    FString ModelHash;

    /** Model-space center of the union of all renderable nodes. */
    FVector Center = FVector::ZeroVector;

    /** Model-space half size of the union of all renderable nodes. */
    FVector Extent = FVector::ZeroVector;

    /** Unscaled local-space half size for each base mesh key. */
    TMap<FName, FVector> MeshExtents;

    bool IsSane() const;
};

/** Mutable world runtime state. User-authored level settings remain in level.json. */
struct GLTFSIMULATOR_API FWorldRuntimeData
{
    float WorldTime = 0.0f;
    FString SelectedPlayer;
};

/**
 * Versioned binary persistence for program-owned .dat state files and .scz model-size caches.
 *
 * Every payload has a magic, file-kind, schema version, exact byte count, and CRC32. Model .scz
 * payloads additionally carry a bounded zlib-compressed block with its own raw-size and CRC checks. Disk commits
 * use FSafeFileIO's verified temp/primary/.bak transaction. Deserializers validate every count,
 * string length, enum, number, and transform before publishing data to gameplay code.
 */
class GLTFSIMULATOR_API FBinaryDataStore
{
public:
    static constexpr int64 MaxModelSczBytes = 128ll * 1024ll * 1024ll;
    static constexpr int64 MaxModelCacheRawBytes = 256ll * 1024ll * 1024ll;
    /** Compatibility alias for external code compiled against the former model-DAT API. */
    static constexpr int64 MaxModelDatBytes = MaxModelSczBytes;
    static constexpr int64 MaxEntitiesDatBytes = 256ll * 1024ll * 1024ll;
    static constexpr int64 MaxPlayersDatBytes = 64ll * 1024ll * 1024ll;
    static constexpr int64 MaxWorldDatBytes = 1024ll * 1024ll;

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

    /** Removes the primary, backup, and abandoned transaction files for a stale .scz cache. */
    static bool InvalidateCacheFile(const FString& CachePath, FString& OutError);

    /** Backward-compatible name; model caches now use .scz and call InvalidateCacheFile(). */
    static bool InvalidateDatFile(const FString& CachePath, FString& OutError)
    {
        return InvalidateCacheFile(CachePath, OutError);
    }

    static bool LoadEntities(
        const FString& DatPath,
        TArray<FPlacedObjectRecord>& OutRecords,
        FString& OutError);

    static FSafeFileWriteResult SaveEntitiesBlocking(
        const FString& DatPath,
        const TArray<FPlacedObjectRecord>& Records);

    static void SaveEntitiesAsync(
        const FString& DatPath,
        const TArray<FPlacedObjectRecord>& Records,
        FSafeFileIO::FWriteCallback Callback = FSafeFileIO::FWriteCallback());

    static bool LoadPlayers(
        const FString& DatPath,
        UPlayerData* OutData,
        FString& OutError);

    static FSafeFileWriteResult SavePlayersBlocking(
        const FString& DatPath,
        const UPlayerData* Data);

    static void SavePlayersAsync(
        const FString& DatPath,
        const UPlayerData* Data,
        FSafeFileIO::FWriteCallback Callback = FSafeFileIO::FWriteCallback());

    static bool LoadWorldRuntime(
        const FString& DatPath,
        FWorldRuntimeData& OutData,
        FString& OutError);

    static FSafeFileWriteResult SaveWorldRuntimeBlocking(
        const FString& DatPath,
        const FWorldRuntimeData& Data);

    static void SaveWorldRuntimeAsync(
        const FString& DatPath,
        const FWorldRuntimeData& Data,
        FSafeFileIO::FWriteCallback Callback = FSafeFileIO::FWriteCallback());

private:
    static bool SerializeModelCache(const FModelCacheData& Cache, TArray<uint8>& OutBytes, FString& OutError);
    static bool DeserializeModelCache(const TArray<uint8>& Bytes, FModelCacheData& OutCache, FString& OutError);
    static bool SerializeEntities(const TArray<FPlacedObjectRecord>& Records, TArray<uint8>& OutBytes, FString& OutError);
    static bool DeserializeEntities(const TArray<uint8>& Bytes, TArray<FPlacedObjectRecord>& OutRecords, FString& OutError);
    static bool SerializePlayers(const UPlayerData* Data, TArray<uint8>& OutBytes, FString& OutError);
    static bool DeserializePlayers(const TArray<uint8>& Bytes, UPlayerData* OutData, FString& OutError);
    static bool SerializeWorldRuntime(const FWorldRuntimeData& Data, TArray<uint8>& OutBytes, FString& OutError);
    static bool DeserializeWorldRuntime(const TArray<uint8>& Bytes, FWorldRuntimeData& OutData, FString& OutError);
};
