// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldArchive.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Model/ModelData.h"
#include "Simulator/ModelDefinitionJson.h"
#include "System/WorldBakedData.h"

/** One complete glTF node row retained independently from the render-oriented scene maps. */
struct GLTFSIMULATOR_API FGWorldNodeTransform
{
    int32 NodeIndex = INDEX_NONE;
    int32 ParentIndex = INDEX_NONE;
    int32 MeshIndex = INDEX_NONE;
    int32 SkinIndex = INDEX_NONE;
    FString Name;
    FTransform LocalTransform = FTransform::Identity;
};

/**
 * Immutable metadata stored beside one model's decoded .dat members in a .gwd archive.
 *
 * Render buffers are not stored here. They live in independently compressed .dat members and are
 * range-read only after this metadata says that a particular mesh is required.
 */
struct GLTFSIMULATOR_API FGWorldModelMetadata
{
    /** Every source node, including empty hierarchy/bone nodes that do not own a render mesh. */
    TArray<FGWorldNodeTransform> NodeTransforms;
    FGWorldSceneRenderData SceneData;
    int32 SourceNodeCount = 0;
    int32 SourceMeshCount = 0;
    int32 SourceMaterialCount = 0;
    int32 SourceTextureCount = 0;

    bool IsSane(FString* OutError = nullptr) const;
};

/** Input captured by the source-model bake pass before the immutable archive is committed. */
struct GLTFSIMULATOR_API FGWorldBuildModel
{
    FModelDefinition Definition;
    FString DefinitionJson;
    FGWorldModelMetadata Metadata;
    /** Decoded Unreal-ready data captured from glTFRuntime; the source GLB is never copied. */
    FGWorldBakedModel BakedData;
    /** Source snapshot captured with the metadata pass; never serialized into the runtime file. */
    int64 SourceFileSize = -1;
    FDateTime SourceTimestamp;
};

/** Small, always-resident row used for model selection and distance culling. */
struct GLTFSIMULATOR_API FGWorldModelSummary
{
    int32 SourceNodeCount = 0;
    int32 SourceMeshCount = 0;
    int32 SourceMaterialCount = 0;
    int32 SourceTextureCount = 0;
    FVector Center = FVector::ZeroVector;
    FVector Size = FVector::ZeroVector;

    bool IsSane(FString* OutError = nullptr) const;
};

/** Checksummed random-access member described by the root .gwd directory. */
struct GLTFSIMULATOR_API FGWorldArchiveRange
{
    FString Name;
    uint64 Offset = 0;
    uint64 StoredSize = 0;
    uint64 UncompressedSize = 0;
    uint32 Crc = 0;
    /** 0 = stored, 1 = zlib. Unknown codecs are rejected rather than guessed. */
    uint8 Codec = 0;
};

/** On-demand table mapping decoded assets to their independently compressed .dat members. */
struct GLTFSIMULATOR_API FGWorldModelManifest
{
    TMap<int32, FGWorldArchiveRange> MeshRanges;
    TMap<int32, FGWorldArchiveRange> SkinRanges;
    TMap<int32, FGWorldArchiveRange> MaterialRanges;
    TMap<int32, FGWorldArchiveRange> TextureRanges;
    TMap<int32, FString> MeshNames;

    bool IsSane(const FGWorldModelSummary& Summary, FString* OutError = nullptr) const;
};

/**
 * Compact directory row retained after Open(). Large maps and JSON are represented by ranges and
 * therefore do not become resident merely because a world has been selected.
 */
struct GLTFSIMULATOR_API FGWorldModelRecord
{
    FModelDefinition Definition;
    FGWorldModelSummary Summary;
    FGWorldArchiveRange DefinitionRange;
    FGWorldArchiveRange MetadataRange;
    FGWorldArchiveRange ManifestRange;
};

/**
 * Thread-safe immutable .gwd reader.
 *
 * Open() reads the fixed header and bounded directory only. Every subsequent call opens a private
 * file handle and reads one checksummed compressed range, so callers never share a seek cursor and
 * neither the complete world nor a complete model is mirrored in RAM.
 */
class GLTFSIMULATOR_API FGWorldArchiveReader final
    : public TSharedFromThis<FGWorldArchiveReader, ESPMode::ThreadSafe>
{
public:
    static TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Open(
        const FString& ArchivePath,
        FString& OutError);

    const FString& GetArchivePath() const { return Path; }
    const FGuid& GetBuildId() const { return BuildId; }
    int32 Num() const { return Records.Num(); }

    void GetRecords(TArray<FGWorldModelRecord>& OutRecords) const;
    bool FindRecord(const FGuid& UUID, FGWorldModelRecord& OutRecord) const;
    bool FindRecordByReference(const FString& Reference, FGWorldModelRecord& OutRecord) const;
    bool GetSummary(const FGuid& UUID, FGWorldModelSummary& OutSummary) const;

    /** Reads the project config.json embedded in the immutable world archive. */
    bool ReadWorldConfig(FString& OutConfigJson, FString& OutError) const;

    /** Reads only the definition JSON and bone aliases for one model. */
    bool ReadModelDefinition(
        const FGuid& UUID,
        TMap<FString, FString>& OutBones,
        FString& OutDefinitionJson,
        FString& OutError) const;

    /** Reads only the baked node transforms, mesh sizes, LOD rows and scene settings. */
    bool ReadModelMetadata(
        const FGuid& UUID,
        FGWorldModelMetadata& OutMetadata,
        FString& OutError) const;

    /** Reads only the small table of mesh/material/texture/skin .dat ranges for one model. */
    bool ReadModelManifest(
        const FGuid& UUID,
        FGWorldModelManifest& OutManifest,
        FString& OutError) const;

    /** Reads requested LODs and, unless skipped, only their transitive material/texture dependencies. */
    bool ReadMeshBundle(
        const FGuid& UUID,
        const FGWorldModelManifest& Manifest,
        const TArray<int32>& MeshIndices,
        int32 SkinIndex,
        bool bLoadMaterialDependencies,
        FGWorldBakedAssetBundle& OutBundle,
        FString& OutError,
        const TSet<int32>* SkipTextureIds = nullptr,
        const TSet<int32>* SkipMaterialIds = nullptr) const;

private:
    FString Path;
    FGuid BuildId;
    int64 FileSize = 0;
    uint64 DataEndOffset = 0;
    FGWorldArchiveRange WorldConfigRange;
    TMap<FGuid, FGWorldModelRecord> Records;
    /** Sorted root-member intervals used for O(children + roots) manifest overlap checks. */
    TArray<TPair<uint64, uint64>> RootMemberRanges;

    /**
     * Tiny bounded resident cache for the hottest model tables. Repeated placements normally share
     * one UWorldBakedModelAsset, but this also avoids synchronous disk reads after GC/recreation.
     * The reader is shared by worker tasks, so all cache access is protected.
     */
    mutable FCriticalSection ResidentTableCacheLock;
    mutable TMap<FGuid, FGWorldModelMetadata> ResidentMetadataCache;
    mutable TMap<FGuid, FGWorldModelManifest> ResidentManifestCache;
    static constexpr int32 MaxResidentModelTableEntries = 64;
};

/** Static helpers for the immutable world-build file and its opaque runtime model references. */
class GLTFSIMULATOR_API FGWorldArchive
{
public:
    static constexpr int64 MaxDirectoryBytes = 256ll * 1024ll * 1024ll;
    // A single member is materialized into TArray<uint8>; enforce its signed 32-bit index limit.
    static constexpr int64 MaxDatMemberBytes = 2147483647ll;
    static constexpr int32 MaxModels = 100000;

    /** Returns Worlds/WorldName.gwd for a normalized virtual WorldName root path. */
    static FString MakeArchivePath(const FString& WorldRoot);

    /** Runtime keys never contain an authoring path and are safe to replicate between peers. */
    static FString MakeModelReference(const FGuid& UUID);
    static bool ParseModelReference(const FString& Reference, FGuid& OutUUID);

    /**
     * Serializes the already decoded build snapshots into a temporary archive and atomically
     * publishes it. Source GLBs are checked only for size/timestamp consistency and are never copied.
     * Existing valid builds remain untouched when any source, serialization or verification step
     * fails. This function performs file/native-data work only and is worker-thread safe.
     */
    static bool BuildBlocking(
        const FString& WorldRoot,
        const TArray<FGWorldBuildModel>& Models,
        FString& OutArchivePath,
        FString& OutError,
        TFunction<bool()> ShouldCancel = TFunction<bool()>(),
        const FString& WorldConfigJson = FString());

    /** Same immutable archive format, but published to an explicit path such as Resources/Pack.gasset. */
    static bool BuildBlockingToArchivePath(
        const FString& ArchivePath,
        const TArray<FGWorldBuildModel>& Models,
        FString& OutArchivePath,
        FString& OutError,
        TFunction<bool()> ShouldCancel = TFunction<bool()>(),
        const FString& ArchiveConfigJson = FString());
};
