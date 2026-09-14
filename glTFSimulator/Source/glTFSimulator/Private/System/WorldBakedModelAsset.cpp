// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldBakedModelAsset.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "System/WorldBakedModelAsset.h"

#include "Async/ParallelFor.h"
#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "ReferenceSkeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "PixelFormat.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Setting/GameSettings.h"
#include "Simulator/RuntimeModelResolver.h"
#include "System/SafeFileIO.h"
#include "System/glTFRuntimeSafety.h"
#include "UObject/GCObject.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

/**
 * Request-local GC bridge for transient textures/materials stored in glTFRuntime POD structs.
 * Native stack pointers and non-reflected TMaps are not GC references; this guard keeps every
 * object alive from decoded bundle conversion through the synchronous mesh finalizer call.
 */
struct FWorldBakedBuildReferenceGuard final : public FGCObject
{
    void Add(UObject* Object)
    {
        if (IsValid(Object))
        {
            // Duplicate references are harmless to GC and cheaper than an O(N) AddUnique scan for
            // every material in a large model. The guard exists for one short build request only.
            References.Add(Object);
        }
    }

    void Reserve(const int32 ExpectedReferences)
    {
        References.Reserve(References.Num() + FMath::Max(0, ExpectedReferences));
    }

    virtual void AddReferencedObjects(FReferenceCollector& Collector) override
    {
        for (TObjectPtr<UObject>& Reference : References)
        {
            Collector.AddReferencedObject(Reference);
        }
    }

    virtual FString GetReferencerName() const override
    {
        return TEXT("FWorldBakedBuildReferenceGuard");
    }

    void AppendTo(TArray<TObjectPtr<UObject>>& OutReferences) const
    {
        OutReferences.Append(References);
    }

private:
    TArray<TObjectPtr<UObject>> References;
};

namespace WorldBakedModelAssetPrivate
{
    constexpr int32 MaxTextureMips = 32;
    constexpr int64 MaxTextureMipBytes = 1024ll * 1024ll * 1024ll;
    // A bounded strong cache makes archive I/O reuse GC-safe without turning a streaming facade
    // into permanent storage for every texture ever visited. The weak map may still reuse a texture
    // that remains referenced by a live mesh/material after it falls out of this recent set.
    // Speed-first streaming cache: enough residency to stop adjacent world chunks from repeatedly
    // reopening/decoding/uploading the same shared textures. The cache remains bounded so a long
    // traversal cannot retain the entire world forever.
    constexpr int32 MaxStrongTextureCacheEntries = 256;
    constexpr int64 MaxStrongTextureCacheBytes = 512ll * 1024ll * 1024ll;
    constexpr int32 MaxStrongMaterialCacheEntries = 256;

    uint32 BuildMaterialConfigSignature(const FglTFRuntimeMaterialsConfig& Config)
    {
        uint32 Hash = 0x9e3779b9u;
        Hash = HashCombineFast(Hash, PointerHash(Config.ForceMaterial));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.ImagesConfig.MaxWidth));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.ImagesConfig.MaxHeight));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.ImagesConfig.bCompressMips));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.ImagesConfig.bStreaming));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.bMaterialsOverrideMapInjectParams));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.bSkipLoad));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.bGeneratesMipMaps));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.bLoadMipMaps));
        Hash = HashCombineFast(Hash, GetTypeHash(Config.SpecularFactor));

        TArray<FString> NamedKeys;
        Config.MaterialsOverrideByNameMap.GenerateKeyArray(NamedKeys);
        NamedKeys.Sort();
        for (const FString& Key : NamedKeys)
        {
            UMaterialInterface* const* Material = Config.MaterialsOverrideByNameMap.Find(Key);
            Hash = HashCombineFast(Hash, GetTypeHash(Key));
            Hash = HashCombineFast(Hash, PointerHash(Material ? *Material : nullptr));
        }

        const auto HashTypedMap = [&Hash](const TMap<EglTFRuntimeMaterialType, UMaterialInterface*>& Map)
        {
            TArray<EglTFRuntimeMaterialType> Keys;
            Map.GenerateKeyArray(Keys);
            Keys.Sort([](const EglTFRuntimeMaterialType A, const EglTFRuntimeMaterialType B)
            {
                return static_cast<uint8>(A) < static_cast<uint8>(B);
            });
            for (const EglTFRuntimeMaterialType Key : Keys)
            {
                UMaterialInterface* const* Material = Map.Find(Key);
                Hash = HashCombineFast(Hash, GetTypeHash(static_cast<uint8>(Key)));
                Hash = HashCombineFast(Hash, PointerHash(Material ? *Material : nullptr));
            }
        };
        HashTypedMap(Config.UberMaterialsOverrideMap);
        HashTypedMap(Config.UnlitOverrideMap);

        TArray<FString> ScalarKeys;
        Config.ScalarParamsOverrides.GenerateKeyArray(ScalarKeys);
        ScalarKeys.Sort();
        for (const FString& Key : ScalarKeys)
        {
            const float* Value = Config.ScalarParamsOverrides.Find(Key);
            Hash = HashCombineFast(Hash, GetTypeHash(Key));
            Hash = HashCombineFast(Hash, Value ? GetTypeHash(*Value) : 0u);
        }
        return Hash;
    }

    uint64 BuildMaterialCacheKey(
        const int32 MaterialId,
        const FglTFRuntimeMaterialsConfig& Config)
    {
        return (static_cast<uint64>(static_cast<uint32>(MaterialId)) << 32)
            | static_cast<uint64>(BuildMaterialConfigSignature(Config));
    }

    FString BuildSharedRenderMeshCacheKey(
        const TArray<int32>& MeshIndices,
        const FglTFRuntimeStaticMeshConfig& Config)
    {
        uint32 MeshConfigHash = 0x85ebca6bu;
        MeshConfigHash = HashCombineFast(MeshConfigHash, GetTypeHash(Config.bReverseWinding));
        MeshConfigHash = HashCombineFast(MeshConfigHash,
            GetTypeHash(static_cast<uint8>(Config.NormalsGenerationStrategy)));
        MeshConfigHash = HashCombineFast(MeshConfigHash,
            GetTypeHash(static_cast<uint8>(Config.TangentsGenerationStrategy)));
        MeshConfigHash = HashCombineFast(MeshConfigHash, GetTypeHash(Config.bReverseTangents));
        MeshConfigHash = HashCombineFast(MeshConfigHash, GetTypeHash(Config.bUseHighPrecisionUVs));
        MeshConfigHash = HashCombineFast(MeshConfigHash, GetTypeHash(Config.bUseHighPrecisionTangentBasis));
        MeshConfigHash = HashCombineFast(MeshConfigHash, GetTypeHash(Config.bGenerateStaticMeshDescription));
        MeshConfigHash = HashCombineFast(MeshConfigHash, GetTypeHash(Config.bBuildLumenCards));

        FString Key = FString::Printf(
            TEXT("mat=%08x;mesh=%08x;mul=%08x;lod="),
            BuildMaterialConfigSignature(Config.MaterialsConfig),
            MeshConfigHash,
            GetTypeHash(Config.LODScreenSizeMultiplier));
        Key.Reserve(Key.Len() + MeshIndices.Num() * 16 + Config.LODScreenSize.Num() * 24);
        for (const int32 MeshIndex : MeshIndices)
        {
            Key += FString::Printf(TEXT("%d,"), MeshIndex);
        }
        Key += TEXT(";screen=");
        TArray<int32> ScreenKeys;
        Config.LODScreenSize.GenerateKeyArray(ScreenKeys);
        ScreenKeys.Sort();
        for (const int32 ScreenKey : ScreenKeys)
        {
            const float* ScreenSize = Config.LODScreenSize.Find(ScreenKey);
            Key += FString::Printf(
                TEXT("%d:%08x,"), ScreenKey, ScreenSize ? GetTypeHash(*ScreenSize) : 0u);
        }
        return Key;
    }

    UMaterialInterface* SelectBaseMaterial(
        const FGWorldBakedMaterial& Baked,
        const FglTFRuntimeMaterialsConfig& Config)
    {
        if (IsValid(Config.ForceMaterial))
        {
            return Config.ForceMaterial;
        }
        if (UMaterialInterface* const* Named =
                Config.MaterialsOverrideByNameMap.Find(Baked.Name))
        {
            if (IsValid(*Named))
            {
                return *Named;
            }
        }

        const EglTFRuntimeMaterialType Type =
            static_cast<EglTFRuntimeMaterialType>(Baked.MaterialType);
        const TMap<EglTFRuntimeMaterialType, UMaterialInterface*>& Overrides =
            Baked.bUnlit ? Config.UnlitOverrideMap : Config.UberMaterialsOverrideMap;
        if (UMaterialInterface* const* Typed = Overrides.Find(Type))
        {
            if (IsValid(*Typed))
            {
                return *Typed;
            }
        }

        // The archive stores the shader asset identity, not a transient MID pointer. Project and
        // plugin master materials must consequently remain cooked just as they did for glTFRuntime.
        return LoadObject<UMaterialInterface>(nullptr, *Baked.BaseMaterialPath);
    }

    bool CheckedMultiply(const int64 A, const int64 B, int64& Out)
    {
        if (A <= 0 || B <= 0 || A > MAX_int64 / B)
        {
            Out = 0;
            return false;
        }
        Out = A * B;
        return true;
    }

    /**
     * Validates every field that is consumed by FTexturePlatformData/RHI. A CRC only proves that a
     * member matches its directory entry; it does not make malicious dimensions or enums safe.
     */
    bool ValidateTextureLayout(const FGWorldBakedTexture& Baked, FString& OutError)
    {
        if (Baked.SizeX <= 0 || Baked.SizeY <= 0
            || Baked.PixelFormat <= PF_Unknown || Baked.PixelFormat >= PF_MAX
            || Baked.AddressX >= static_cast<uint8>(TA_MAX)
            || Baked.AddressY >= static_cast<uint8>(TA_MAX)
            || Baked.Filter >= static_cast<uint8>(TF_MAX)
            || Baked.LODGroup >= static_cast<uint8>(TEXTUREGROUP_MAX)
            || Baked.Mips.IsEmpty() || Baked.Mips.Num() > MaxTextureMips)
        {
            OutError = FString::Printf(
                TEXT("Unsupported baked texture metadata: %s"), *Baked.Name);
            return false;
        }

        const FPixelFormatInfo& Format = GPixelFormats[Baked.PixelFormat];
        if (!Format.Supported || Format.BlockSizeX <= 0 || Format.BlockSizeY <= 0
            || Format.BlockSizeZ <= 0 || Format.BlockBytes <= 0)
        {
            OutError = FString::Printf(
                TEXT("Baked texture pixel format is unsupported on this platform: %s"),
                *Baked.Name);
            return false;
        }

        int32 ExpectedSizeX = Baked.SizeX;
        int32 ExpectedSizeY = Baked.SizeY;
        for (const FGWorldBakedTextureMip& Mip : Baked.Mips)
        {
            if (Mip.SizeX != ExpectedSizeX || Mip.SizeY != ExpectedSizeY
                || Mip.SizeZ != 1 || Mip.Bytes.IsEmpty())
            {
                OutError = FString::Printf(
                    TEXT("Baked texture has an invalid mip chain: %s"), *Baked.Name);
                return false;
            }

            const int64 BlocksX =
                (static_cast<int64>(Mip.SizeX) + Format.BlockSizeX - 1) / Format.BlockSizeX;
            const int64 BlocksY =
                (static_cast<int64>(Mip.SizeY) + Format.BlockSizeY - 1) / Format.BlockSizeY;
            const int64 BlocksZ =
                (static_cast<int64>(Mip.SizeZ) + Format.BlockSizeZ - 1) / Format.BlockSizeZ;
            int64 BlockCountXY = 0;
            int64 BlockCount = 0;
            int64 ExpectedBytes = 0;
            if (!CheckedMultiply(BlocksX, BlocksY, BlockCountXY)
                || !CheckedMultiply(BlockCountXY, BlocksZ, BlockCount)
                || !CheckedMultiply(BlockCount, Format.BlockBytes, ExpectedBytes)
                || ExpectedBytes > MaxTextureMipBytes
                || ExpectedBytes != static_cast<int64>(Mip.Bytes.Num()))
            {
                OutError = FString::Printf(
                    TEXT("Baked texture mip byte count is invalid: %s"), *Baked.Name);
                return false;
            }

            ExpectedSizeX = FMath::Max(1, ExpectedSizeX >> 1);
            ExpectedSizeY = FMath::Max(1, ExpectedSizeY >> 1);
        }
        return true;
    }

    /**
     * Attaches the data-free parser required by glTFRuntime's RuntimeLOD finalizers. The caller
     * must already own FglTFRuntimeSafety's synchronous native gate because parser construction and
     * SetParser touch the same third-party mutable state as mesh creation.
     */
    bool InitializeEmptyMeshBuilder(UglTFRuntimeAsset* Builder)
    {
        if (!IsValid(Builder))
        {
            return false;
        }
        const TSharedRef<FJsonObject> EmptyRoot = MakeShared<FJsonObject>();
        const TSharedRef<FglTFRuntimeParser> EmptyParser =
            MakeShared<FglTFRuntimeParser>(EmptyRoot, FMatrix::Identity, 1.0f);
        return Builder->SetParser(EmptyParser);
    }

    /**
     * Runtime world meshes are short-lived streamed geometry. Keeping every one in the hardware
     * ray-tracing scene makes UE build BLAS + persistent SBT records for each streamed mesh and,
     * on large worlds, quickly exhausts the Always Resident RT geometry budget. Disable RT before
     * glTFRuntime initializes render resources; raster/Lumen screen traces continue to work.
     */
    struct FScopedDisableStreamedStaticMeshRayTracing final
    {
        FDelegateHandle Handle;

        FScopedDisableStreamedStaticMeshRayTracing()
        {
#if (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5) || ENGINE_MAJOR_VERSION > 5
            Handle = FglTFRuntimeParser::OnPreCreatedStaticMesh.AddLambda(
                [](FglTFRuntimeStaticMeshContextRef Context)
                {
                    if (Context->StaticMesh)
                    {
                        Context->StaticMesh->bSupportRayTracing = false;
                    }
                });
#endif
        }

        ~FScopedDisableStreamedStaticMeshRayTracing()
        {
#if (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5) || ENGINE_MAJOR_VERSION > 5
            if (Handle.IsValid())
            {
                FglTFRuntimeParser::OnPreCreatedStaticMesh.Remove(Handle);
            }
#endif
        }
    };

    /**
     * Converts archive POD arrays into glTFRuntime RuntimeLODs without touching any UObject.
     * This function is intentionally worker-safe and is the expensive half of reconstruction.
     */
    bool BuildRuntimeLODsDetached(
        const FGWorldBakedAssetBundle& Bundle,
        const int32 SkinIndex,
        const bool bLoadMaterials,
        TArray<FglTFRuntimeMeshLOD>& OutLODs,
        TArray<TArray<int32>>& OutMaterialIds,
        FString& OutError)
    {
        OutLODs.Reset();
        OutMaterialIds.Reset();
        OutError.Reset();

        const FGWorldBakedSkin* Skin = nullptr;
        if (SkinIndex != INDEX_NONE)
        {
            for (const FGWorldBakedSkin& Candidate : Bundle.Skins)
            {
                if (Candidate.SkinIndex == SkinIndex)
                {
                    Skin = &Candidate;
                    break;
                }
            }
            if (!Skin)
            {
                OutError = TEXT("The requested baked skin is absent");
                return false;
            }
        }

        OutLODs.SetNum(Bundle.Meshes.Num());
        OutMaterialIds.SetNum(Bundle.Meshes.Num());
        TArray<TPair<int32, int32>> PrimitiveTasks;
        for (int32 MeshArrayIndex = 0; MeshArrayIndex < Bundle.Meshes.Num(); ++MeshArrayIndex)
        {
            const FGWorldBakedMesh& BakedMesh = Bundle.Meshes[MeshArrayIndex];
            FglTFRuntimeMeshLOD& LOD = OutLODs[MeshArrayIndex];
            LOD.bHasNormals = BakedMesh.bHasNormals;
            LOD.bHasTangents = BakedMesh.bHasTangents;
            LOD.bHasUV = BakedMesh.bHasUV;
            LOD.bHasVertexColors = BakedMesh.bHasVertexColors;
            LOD.AdditionalTransforms = BakedMesh.AdditionalTransforms;

            if (Skin)
            {
                LOD.Skeleton.Reserve(Skin->Bones.Num());
                for (const FGWorldBakedBone& BakedBone : Skin->Bones)
                {
                    FglTFRuntimeBone& Bone = LOD.Skeleton.AddDefaulted_GetRef();
                    Bone.BoneName = BakedBone.Name;
                    Bone.ParentIndex = BakedBone.ParentIndex;
                    Bone.Transform = BakedBone.Transform;
                }
            }

            LOD.Primitives.SetNum(BakedMesh.Primitives.Num());
            OutMaterialIds[MeshArrayIndex].SetNum(BakedMesh.Primitives.Num());
            PrimitiveTasks.Reserve(PrimitiveTasks.Num() + BakedMesh.Primitives.Num());
            for (int32 PrimitiveIndex = 0; PrimitiveIndex < BakedMesh.Primitives.Num(); ++PrimitiveIndex)
            {
                PrimitiveTasks.Emplace(MeshArrayIndex, PrimitiveIndex);
            }
        }

        ParallelFor(PrimitiveTasks.Num(),
            [&Bundle, Skin, bLoadMaterials, &OutLODs, &OutMaterialIds, &PrimitiveTasks](const int32 TaskIndex)
            {
                const int32 MeshArrayIndex = PrimitiveTasks[TaskIndex].Key;
                const int32 PrimitiveIndex = PrimitiveTasks[TaskIndex].Value;
                const FGWorldBakedPrimitive& Baked =
                    Bundle.Meshes[MeshArrayIndex].Primitives[PrimitiveIndex];
                FglTFRuntimePrimitive& Primitive =
                    OutLODs[MeshArrayIndex].Primitives[PrimitiveIndex];

                Primitive.Positions.Reserve(Baked.Positions.Num());
                for (const FVector3f& Value : Baked.Positions) Primitive.Positions.Add(FVector(Value));
                Primitive.Normals.Reserve(Baked.Normals.Num());
                for (const FVector3f& Value : Baked.Normals) Primitive.Normals.Add(FVector(Value));
                Primitive.Tangents.Reserve(Baked.Tangents.Num());
                for (const FVector4f& Value : Baked.Tangents) Primitive.Tangents.Add(FVector4(Value));

                Primitive.UVs.SetNum(Baked.UVs.Num());
                for (int32 Channel = 0; Channel < Baked.UVs.Num(); ++Channel)
                {
                    Primitive.UVs[Channel].Reserve(Baked.UVs[Channel].Num());
                    for (const FVector2f& Value : Baked.UVs[Channel])
                    {
                        Primitive.UVs[Channel].Add(FVector2D(Value));
                    }
                }

                Primitive.Indices = Baked.Indices;
                Primitive.Joints.SetNum(Baked.Joints.Num());
                for (int32 SetIndex = 0; SetIndex < Baked.Joints.Num(); ++SetIndex)
                {
                    Primitive.Joints[SetIndex].Reserve(Baked.Joints[SetIndex].Num());
                    for (const FGWorldBakedJoint4& Value : Baked.Joints[SetIndex])
                    {
                        FglTFRuntimeUInt16Vector4& Joint =
                            Primitive.Joints[SetIndex].AddDefaulted_GetRef();
                        Joint.X = Value.X;
                        Joint.Y = Value.Y;
                        Joint.Z = Value.Z;
                        Joint.W = Value.W;
                    }
                }

                Primitive.Weights.SetNum(Baked.Weights.Num());
                for (int32 SetIndex = 0; SetIndex < Baked.Weights.Num(); ++SetIndex)
                {
                    Primitive.Weights[SetIndex].Reserve(Baked.Weights[SetIndex].Num());
                    for (const FVector4f& Value : Baked.Weights[SetIndex])
                    {
                        Primitive.Weights[SetIndex].Add(FVector4(Value));
                    }
                }

                Primitive.Colors.Reserve(Baked.Colors.Num());
                for (const FVector4f& Value : Baked.Colors) Primitive.Colors.Add(FVector4(Value));

                Primitive.MorphTargets.Reserve(Baked.MorphTargets.Num());
                for (const FGWorldBakedMorphTarget& BakedMorph : Baked.MorphTargets)
                {
                    FglTFRuntimeMorphTarget& Morph = Primitive.MorphTargets.AddDefaulted_GetRef();
                    Morph.Name = BakedMorph.Name;
                    Morph.Positions.Reserve(BakedMorph.Positions.Num());
                    for (const FVector3f& Value : BakedMorph.Positions) Morph.Positions.Add(FVector(Value));
                    Morph.Normals.Reserve(BakedMorph.Normals.Num());
                    for (const FVector3f& Value : BakedMorph.Normals) Morph.Normals.Add(FVector(Value));
                }

                Primitive.OverrideBoneMap = !Baked.BoneMap.IsEmpty()
                    ? Baked.BoneMap
                    : (Skin ? Skin->JointBoneMap : TMap<int32, FName>());
                Primitive.WeightMaps = Baked.WeightMaps;
                Primitive.MaterialName = Baked.MaterialName;
                Primitive.Mode = Baked.Mode;
                Primitive.bHasMaterial = bLoadMaterials && Baked.bHasMaterial;
                Primitive.bHighPrecisionUVs = Baked.bHighPrecisionUVs;
                Primitive.bHighPrecisionWeights = Baked.bHighPrecisionWeights;
                Primitive.bDisableShadows = Baked.bDisableShadows;
                Primitive.bHasIndices = Baked.bHasIndices;
                OutMaterialIds[MeshArrayIndex][PrimitiveIndex] = Baked.MaterialId;
            });

        if (OutLODs.IsEmpty())
        {
            OutError = TEXT("The requested baked mesh set is empty");
            return false;
        }
        return true;
    }
}

bool UWorldBakedModelAsset::Initialize(const FResolvedRuntimeModel& Model, FString& OutError)
{
    check(IsInGameThread());
    UUID.Invalidate();
    Reference.Reset();
    Reader.Reset();
    Manifest.Reset();
    Nodes.Reset();
    WeakTextureCache.Reset();
    TextureCacheKeepAlive.Reset();
    TextureCacheKeepAliveKeys.Reset();
    TextureCacheKeepAliveBytes.Reset();
    TextureCacheKeepAliveTotalBytes = 0;
    WeakMaterialCache.Reset();
    MaterialCacheKeepAlive.Reset();
    MaterialCacheKeepAliveKeys.Reset();
    AsyncBuildReferences.Reset();
    ActiveFinalizeRequests.Reset();

    if (!Model.IsValid())
    {
        OutError = TEXT("Cannot initialize a baked asset from an invalid model record");
        return false;
    }

    FGWorldModelMetadata Metadata;
    FGWorldModelManifest LoadedManifest;
    if (!Model.ArchiveReader->ReadModelMetadata(Model.UUID, Metadata, OutError)
        || !Model.ArchiveReader->ReadModelManifest(Model.UUID, LoadedManifest, OutError))
    {
        return false;
    }

    UUID = Model.UUID;
    Reference = Model.Reference;
    Reader = Model.ArchiveReader;
    Manifest = MakeShared<FGWorldModelManifest, ESPMode::ThreadSafe>(
        MoveTemp(LoadedManifest));

    // Build the glTFRuntime-compatible node view from baked metadata only. No parser or GLB bytes
    // participate in this path.
    Nodes.Reserve(Metadata.NodeTransforms.Num());
    TMap<int32, int32> NodeToArray;
    NodeToArray.Reserve(Metadata.NodeTransforms.Num());
    for (const FGWorldNodeTransform& Source : Metadata.NodeTransforms)
    {
        FglTFRuntimeNode& Target = Nodes.AddDefaulted_GetRef();
        Target.Index = Source.NodeIndex;
        Target.ParentIndex = Source.ParentIndex;
        Target.MeshIndex = Source.MeshIndex;
        Target.SkinIndex = Source.SkinIndex;
        Target.Name = Source.Name;
        Target.Transform = Source.LocalTransform;
        NodeToArray.Add(Target.Index, Nodes.Num() - 1);
    }
    for (FglTFRuntimeNode& Node : Nodes)
    {
        if (const int32* ParentArrayIndex = NodeToArray.Find(Node.ParentIndex))
        {
            Nodes[*ParentArrayIndex].ChildrenIndices.Add(Node.Index);
        }
    }

    OutError.Reset();
    return true;
}

FString UWorldBakedModelAsset::GetMeshName(const int32 MeshIndex) const
{
    if (!Manifest.IsValid())
    {
        return FString();
    }
    const FString* Name = Manifest->MeshNames.Find(MeshIndex);
    return Name ? *Name : FString();
}

void UWorldBakedModelAsset::CollectCachedTextureIds(TSet<int32>& OutTextureIds)
{
    check(IsInGameThread());
    OutTextureIds.Reset();
    const uint32 TextureLimit = static_cast<uint32>(
        UGameSettings::ResolveMaxTextureResolution(this));

    // Skip worker I/O only for the bounded strong-cache subset. A weak entry can disappear during
    // an async read if GC runs, while these reflected references are guaranteed to remain resident.
    const int32 EntryCount = FMath::Min(TextureCacheKeepAlive.Num(), TextureCacheKeepAliveKeys.Num());
    for (int32 Index = 0; Index < EntryCount; ++Index)
    {
        const uint64 CacheKey = TextureCacheKeepAliveKeys[Index];
        const uint32 CachedLimit = static_cast<uint32>(CacheKey & 0xffffffffull);
        if (CachedLimit != TextureLimit || !IsValid(TextureCacheKeepAlive[Index]))
        {
            continue;
        }
        OutTextureIds.Add(static_cast<int32>(static_cast<uint32>(CacheKey >> 32)));
    }
}

void UWorldBakedModelAsset::CollectCachedMaterialIds(
    const FglTFRuntimeMaterialsConfig& MaterialsConfig,
    TSet<int32>& OutMaterialIds)
{
    check(IsInGameThread());
    OutMaterialIds.Reset();
    const uint32 Signature =
        WorldBakedModelAssetPrivate::BuildMaterialConfigSignature(MaterialsConfig);
    const int32 EntryCount = FMath::Min(
        MaterialCacheKeepAlive.Num(), MaterialCacheKeepAliveKeys.Num());
    for (int32 Index = 0; Index < EntryCount; ++Index)
    {
        const uint64 CacheKey = MaterialCacheKeepAliveKeys[Index];
        if (static_cast<uint32>(CacheKey) != Signature
            || !IsValid(MaterialCacheKeepAlive[Index]))
        {
            continue;
        }
        OutMaterialIds.Add(
            static_cast<int32>(static_cast<uint32>(CacheKey >> 32)));
    }
}

UMaterialInterface* UWorldBakedModelAsset::FindCachedMaterial(
    const int32 MaterialId,
    const FglTFRuntimeMaterialsConfig& MaterialsConfig,
    FWorldBakedBuildReferenceGuard& ReferenceGuard)
{
    check(IsInGameThread());
    const uint64 CacheKey =
        WorldBakedModelAssetPrivate::BuildMaterialCacheKey(MaterialId, MaterialsConfig);
    if (const TWeakObjectPtr<UMaterialInterface>* Cached = WeakMaterialCache.Find(CacheKey))
    {
        if (UMaterialInterface* Existing = Cached->Get())
        {
            ReferenceGuard.Add(Existing);
            return Existing;
        }
        WeakMaterialCache.Remove(CacheKey);
    }
    return nullptr;
}

UTexture2D* UWorldBakedModelAsset::FindCachedTexture(
    const int32 TextureId,
    FWorldBakedBuildReferenceGuard& ReferenceGuard)
{
    check(IsInGameThread());
    const uint32 TextureLimit = static_cast<uint32>(
        UGameSettings::ResolveMaxTextureResolution(this));
    const uint64 TextureCacheKey =
        (static_cast<uint64>(static_cast<uint32>(TextureId)) << 32) | TextureLimit;
    if (const TWeakObjectPtr<UTexture2D>* Cached = WeakTextureCache.Find(TextureCacheKey))
    {
        if (UTexture2D* Existing = Cached->Get())
        {
            ReferenceGuard.Add(Existing);
            return Existing;
        }
        WeakTextureCache.Remove(TextureCacheKey);
    }
    return nullptr;
}

UTexture2D* UWorldBakedModelAsset::CreateTexture(
    const FGWorldBakedTexture& Baked,
    FWorldBakedBuildReferenceGuard& ReferenceGuard,
    FString& OutError)
{
    check(IsInGameThread());
    if (!WorldBakedModelAssetPrivate::ValidateTextureLayout(Baked, OutError))
    {
        return nullptr;
    }
    const int32 TextureLimit = UGameSettings::ResolveMaxTextureResolution(this);
    const uint64 TextureCacheKey =
        (static_cast<uint64>(static_cast<uint32>(Baked.TextureId)) << 32)
        | static_cast<uint32>(TextureLimit);
    if (const TWeakObjectPtr<UTexture2D>* Cached = WeakTextureCache.Find(TextureCacheKey))
    {
        if (UTexture2D* Existing = Cached->Get())
        {
            ReferenceGuard.Add(Existing);
            return Existing;
        }
        WeakTextureCache.Remove(TextureCacheKey);
    }

    // Existing .gworld archives can contain larger source mips. Select the first stored mip that
    // satisfies the current user cap instead of rebuilding/resampling pixels at load time. This
    // makes the setting effective immediately and avoids allocating/uploading discarded top mips.
    int32 FirstRuntimeMip = 0;
    while (FirstRuntimeMip + 1 < Baked.Mips.Num()
        && (Baked.Mips[FirstRuntimeMip].SizeX > TextureLimit
            || Baked.Mips[FirstRuntimeMip].SizeY > TextureLimit))
    {
        ++FirstRuntimeMip;
    }
    const FGWorldBakedTextureMip& RuntimeTopMip = Baked.Mips[FirstRuntimeMip];

    UTexture2D* Texture = NewObject<UTexture2D>(this, NAME_None, RF_Transient);
    if (!IsValid(Texture))
    {
        OutError = TEXT("Could not allocate a runtime texture");
        return nullptr;
    }
    // Register immediately: UpdateResource and later material creation may enter engine code that
    // can collect an otherwise unreflected runtime texture.
    ReferenceGuard.Add(Texture);

    // FTexturePlatformData owns every mip appended below. Do not attach it to the UObject until all
    // allocations and copies have succeeded, so a partial texture can be destroyed locally.
    FTexturePlatformData* PlatformData = new FTexturePlatformData();
    PlatformData->SizeX = RuntimeTopMip.SizeX;
    PlatformData->SizeY = RuntimeTopMip.SizeY;
    PlatformData->PixelFormat = static_cast<EPixelFormat>(Baked.PixelFormat);
    // Match glTFRuntime::BuildTexture exactly for an ordinary UTexture2D. Its 2D path leaves
    // PackedData slice metadata at the default value; SetNumSlices() is reserved for volume/array
    // layouts and can make a reconstructed character texture disagree with the original resource.

    for (int32 MipIndex = FirstRuntimeMip; MipIndex < Baked.Mips.Num(); ++MipIndex)
    {
        const FGWorldBakedTextureMip& Source = Baked.Mips[MipIndex];
        FTexture2DMipMap* Mip = new FTexture2DMipMap();
        Mip->SizeX = Source.SizeX;
        Mip->SizeY = Source.SizeY;
        // glTFRuntime's UTexture2D builder leaves SizeZ at its 2D default (0). The archive uses
        // normalized depth=1 only for byte-count validation; restore the native 2D layout here.
        Mip->SizeZ = 0;

        Mip->BulkData.Lock(LOCK_READ_WRITE);
        void* Destination = Mip->BulkData.Realloc(Source.Bytes.Num());
        if (!Destination)
        {
            Mip->BulkData.Unlock();
            delete Mip;
            delete PlatformData;
            OutError = FString::Printf(
                TEXT("Could not allocate mip bytes for texture: %s"), *Baked.Name);
            return nullptr;
        }
        FMemory::Memcpy(Destination, Source.Bytes.GetData(), Source.Bytes.Num());
        Mip->BulkData.Unlock();
        PlatformData->Mips.Add(Mip);
    }

    Texture->SetPlatformData(PlatformData);
    Texture->SRGB = Baked.bSRGB;
    Texture->AddressX = static_cast<TextureAddress>(Baked.AddressX);
    Texture->AddressY = static_cast<TextureAddress>(Baked.AddressY);
    Texture->Filter = static_cast<TextureFilter>(Baked.Filter);
    Texture->LODGroup = static_cast<TextureGroup>(Baked.LODGroup);

    // Streaming happens at the archive-member/mesh level. Each material dependency is loaded only
    // when requested, so Unreal's separate texture streamer must not seek nonexistent cooked bulk.
    Texture->NeverStream = true;
    Texture->UpdateResource();
    WeakTextureCache.Add(TextureCacheKey, Texture);

    int64 ApproximateResidentBytes = 0;
    for (const FGWorldBakedTextureMip& Mip : Baked.Mips)
    {
        ApproximateResidentBytes += static_cast<int64>(Mip.Bytes.Num());
    }

    if (ApproximateResidentBytes > 0
        && ApproximateResidentBytes <= WorldBakedModelAssetPrivate::MaxStrongTextureCacheBytes
        && TextureCacheKeepAlive.Num() < WorldBakedModelAssetPrivate::MaxStrongTextureCacheEntries
        && TextureCacheKeepAliveTotalBytes + ApproximateResidentBytes
            <= WorldBakedModelAssetPrivate::MaxStrongTextureCacheBytes)
    {
        // Admission-only strong cache: never evict a resident entry while this facade is alive.
        // Async bundle reads are allowed to omit texture.dat only when CollectCachedTextureIds()
        // sees an entry in this reflected array. FIFO eviction during another concurrent request
        // could otherwise invalidate that guarantee before the first worker returns to GT.
        TextureCacheKeepAlive.Add(Texture);
        TextureCacheKeepAliveKeys.Add(TextureCacheKey);
        TextureCacheKeepAliveBytes.Add(ApproximateResidentBytes);
        TextureCacheKeepAliveTotalBytes += ApproximateResidentBytes;
    }
    return Texture;
}

UMaterialInterface* UWorldBakedModelAsset::CreateMaterial(
    const FGWorldBakedMaterial& Baked,
    const TMap<int32, UTexture2D*>& Textures,
    const FglTFRuntimeMaterialsConfig& Config,
    FWorldBakedBuildReferenceGuard& ReferenceGuard,
    FString& OutError)
{
    check(IsInGameThread());
    if (UMaterialInterface* Cached =
            FindCachedMaterial(Baked.MaterialId, Config, ReferenceGuard))
    {
        return Cached;
    }

    UMaterialInterface* Base =
        WorldBakedModelAssetPrivate::SelectBaseMaterial(Baked, Config);
    if (!IsValid(Base))
    {
        OutError = FString::Printf(
            TEXT("Baked material shader is unavailable: %s"), *Baked.BaseMaterialPath);
        return nullptr;
    }
    ReferenceGuard.Add(Base);

    UMaterialInstanceDynamic* Material =
        UMaterialInstanceDynamic::Create(Base, this);
    if (!IsValid(Material))
    {
        OutError = FString::Printf(
            TEXT("Could not create material instance: %s"), *Baked.Name);
        return nullptr;
    }
    ReferenceGuard.Add(Material);

    for (const FGWorldBakedScalarParameter& Parameter : Baked.Scalars)
    {
        Material->SetScalarParameterValue(FName(*Parameter.Name), Parameter.Value);
    }
    for (const TPair<FString, float>& Override : Config.ScalarParamsOverrides)
    {
        Material->SetScalarParameterValue(FName(*Override.Key), Override.Value);
    }
    for (const FGWorldBakedVectorParameter& Parameter : Baked.Vectors)
    {
        Material->SetVectorParameterValue(FName(*Parameter.Name), Parameter.Value);
    }
    for (const FGWorldBakedTextureParameter& Parameter : Baked.Textures)
    {
        UTexture* Texture = nullptr;
        if (Parameter.TextureId != INDEX_NONE)
        {
            UTexture2D* const* Found = Textures.Find(Parameter.TextureId);
            Texture = Found ? *Found : nullptr;
        }
        else if (!Parameter.AssetPath.IsEmpty())
        {
            Texture = LoadObject<UTexture>(nullptr, *Parameter.AssetPath);
        }

        if (!IsValid(Texture))
        {
            OutError = FString::Printf(
                TEXT("Material %s has an unavailable texture parameter %s"),
                *Baked.Name, *Parameter.Name);
            return nullptr;
        }
        // Asset-path parameters do not pass through the baked-texture map, so retain them here
        // before material mutation can enter engine code and expose a collection point.
        ReferenceGuard.Add(Texture);
        Material->SetTextureParameterValue(FName(*Parameter.Name), Texture);
    }

    const uint64 MaterialCacheKey =
        WorldBakedModelAssetPrivate::BuildMaterialCacheKey(Baked.MaterialId, Config);
    WeakMaterialCache.Add(MaterialCacheKey, Material);
    // Same admission-only rule as textures. Entries advertised to worker threads as skippable
    // material.dat dependencies must remain strongly resident until the facade itself is released.
    if (MaterialCacheKeepAlive.Num() < WorldBakedModelAssetPrivate::MaxStrongMaterialCacheEntries)
    {
        MaterialCacheKeepAlive.Add(Material);
        MaterialCacheKeepAliveKeys.Add(MaterialCacheKey);
    }
    return Material;
}

bool UWorldBakedModelAsset::AttachRuntimeMaterials(
    const FGWorldBakedAssetBundle& Bundle,
    const FglTFRuntimeMaterialsConfig& MaterialsConfig,
    const TArray<TArray<int32>>& MaterialIds,
    FWorldBakedBuildReferenceGuard& ReferenceGuard,
    TArray<FglTFRuntimeMeshLOD>& InOutLODs,
    FString& OutError)
{
    check(IsInGameThread());
    OutError.Reset();
    ReferenceGuard.Add(this);

    if (MaterialIds.Num() != InOutLODs.Num())
    {
        OutError = TEXT("Prepared RuntimeLOD material layout is inconsistent");
        return false;
    }
    if (MaterialsConfig.bSkipLoad)
    {
        return true;
    }

    TMap<int32, UTexture2D*> Textures;
    TMap<int32, UMaterialInterface*> Materials;
    ReferenceGuard.Reserve(Bundle.Textures.Num() + Bundle.Materials.Num() * 2 + 16);

    // Material.dat can be omitted entirely by the worker when a matching immutable MID is in the
    // reflected keep-alive cache. Recover those dependencies before creating any missing entries.
    for (const TArray<int32>& LODMaterialIds : MaterialIds)
    {
        for (const int32 MaterialId : LODMaterialIds)
        {
            if (MaterialId == INDEX_NONE || Materials.Contains(MaterialId)) continue;
            if (UMaterialInterface* Cached =
                    FindCachedMaterial(MaterialId, MaterialsConfig, ReferenceGuard))
            {
                Materials.Add(MaterialId, Cached);
            }
        }
    }
    Textures.Reserve(Bundle.Textures.Num());
    for (const FGWorldBakedTexture& Baked : Bundle.Textures)
    {
        UTexture2D* Texture = CreateTexture(Baked, ReferenceGuard, OutError);
        if (!Texture) return false;
        Textures.Add(Baked.TextureId, Texture);
    }

    for (const FGWorldBakedMaterial& BakedMaterial : Bundle.Materials)
    {
        for (const FGWorldBakedTextureParameter& Parameter : BakedMaterial.Textures)
        {
            if (Parameter.TextureId == INDEX_NONE || Textures.Contains(Parameter.TextureId)) continue;
            UTexture2D* CachedTexture = FindCachedTexture(Parameter.TextureId, ReferenceGuard);
            if (!IsValid(CachedTexture))
            {
                OutError = FString::Printf(
                    TEXT("Texture %d was omitted from archive I/O but is no longer resident"),
                    Parameter.TextureId);
                return false;
            }
            Textures.Add(Parameter.TextureId, CachedTexture);
        }
    }

    Materials.Reserve(Bundle.Materials.Num());
    for (const FGWorldBakedMaterial& Baked : Bundle.Materials)
    {
        UMaterialInterface* Material =
            CreateMaterial(Baked, Textures, MaterialsConfig, ReferenceGuard, OutError);
        if (!Material) return false;
        Materials.Add(Baked.MaterialId, Material);
    }

    for (int32 MeshArrayIndex = 0; MeshArrayIndex < InOutLODs.Num(); ++MeshArrayIndex)
    {
        FglTFRuntimeMeshLOD& LOD = InOutLODs[MeshArrayIndex];
        if (!MaterialIds.IsValidIndex(MeshArrayIndex)
            || MaterialIds[MeshArrayIndex].Num() != LOD.Primitives.Num())
        {
            OutError = TEXT("Prepared RuntimeLOD primitive/material layout is inconsistent");
            return false;
        }
        for (int32 PrimitiveIndex = 0; PrimitiveIndex < LOD.Primitives.Num(); ++PrimitiveIndex)
        {
            const int32 MaterialId = MaterialIds[MeshArrayIndex][PrimitiveIndex];
            if (MaterialId == INDEX_NONE) continue;
            UMaterialInterface* const* Found = Materials.Find(MaterialId);
            if (!Found || !IsValid(*Found))
            {
                OutError = TEXT("A mesh references a missing baked material");
                return false;
            }
            LOD.Primitives[PrimitiveIndex].Material = *Found;
        }
    }
    return true;
}

bool UWorldBakedModelAsset::BuildRuntimeLODs(
    const FGWorldBakedAssetBundle& Bundle,
    const FglTFRuntimeMaterialsConfig& MaterialsConfig,
    const int32 SkinIndex,
    FWorldBakedBuildReferenceGuard& ReferenceGuard,
    TArray<FglTFRuntimeMeshLOD>& OutLODs,
    FString& OutError)
{
    TArray<TArray<int32>> MaterialIds;
    if (!WorldBakedModelAssetPrivate::BuildRuntimeLODsDetached(
            Bundle, SkinIndex, !MaterialsConfig.bSkipLoad, OutLODs, MaterialIds, OutError))
    {
        return false;
    }
    return AttachRuntimeMaterials(
        Bundle, MaterialsConfig, MaterialIds, ReferenceGuard, OutLODs, OutError);
}

UglTFRuntimeAsset* UWorldBakedModelAsset::CreateMeshBuilder(FString& OutError)
{
    check(IsInGameThread());
    UglTFRuntimeAsset* Builder =
        NewObject<UglTFRuntimeAsset>(this, NAME_None, RF_Transient);
    if (!IsValid(Builder))
    {
        OutError = TEXT("Could not allocate the decoded-data mesh finalizer");
        return nullptr;
    }

    // Parser attachment is deferred into the request's native operation. Each request owns a distinct
    // builder/parser, so baked RuntimeLOD finalizers can use the bounded-parallel gate safely.
    return Builder;
}

void UWorldBakedModelAsset::RetainAsyncBuildReferences(
    UglTFRuntimeAsset* Builder,
    const FWorldBakedBuildReferenceGuard& ReferenceGuard)
{
    check(IsInGameThread());
    if (!IsValid(Builder)) return;
    FGWorldBakedAsyncBuildReferences& Entry = AsyncBuildReferences.AddDefaulted_GetRef();
    Entry.Builder = Builder;
    ReferenceGuard.AppendTo(Entry.References);
}

void UWorldBakedModelAsset::ReleaseAsyncBuildReferences(UglTFRuntimeAsset* Builder)
{
    check(IsInGameThread());
    for (int32 Index = AsyncBuildReferences.Num() - 1; Index >= 0; --Index)
    {
        if (AsyncBuildReferences[Index].Builder.Get() == Builder)
        {
            AsyncBuildReferences.RemoveAtSwap(Index, 1, EAllowShrinking::No);
            return;
        }
    }
}

void UWorldBakedMeshFinalizeRequest::InitializeStatic(
    UWorldBakedModelAsset* InOwner,
    UglTFRuntimeAsset* InBuilder,
    FString InSourceReference,
    FGWorldBakedStaticMeshNativeCallback InCallback)
{
    check(IsInGameThread());
    Owner = InOwner;
    Builder = InBuilder;
    SourceReference = MoveTemp(InSourceReference);
    StaticCallback = MoveTemp(InCallback);
    SkeletalCallback = FGWorldBakedSkeletalMeshNativeCallback();
}

void UWorldBakedMeshFinalizeRequest::InitializeSkeletal(
    UWorldBakedModelAsset* InOwner,
    UglTFRuntimeAsset* InBuilder,
    FString InSourceReference,
    FGWorldBakedSkeletalMeshNativeCallback InCallback)
{
    check(IsInGameThread());
    Owner = InOwner;
    Builder = InBuilder;
    SourceReference = MoveTemp(InSourceReference);
    SkeletalCallback = MoveTemp(InCallback);
    StaticCallback = FGWorldBakedStaticMeshNativeCallback();
}

void UWorldBakedMeshFinalizeRequest::Cleanup()
{
    check(IsInGameThread());
#if (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5) || ENGINE_MAJOR_VERSION > 5
    if (StaticRayTracingHandle.IsValid())
    {
        FglTFRuntimeParser::OnPreCreatedStaticMesh.Remove(StaticRayTracingHandle);
        StaticRayTracingHandle.Reset();
    }
#endif
    if (UWorldBakedModelAsset* StrongOwner = Owner.Get())
    {
        StrongOwner->ReleaseAsyncBuildReferences(Builder.Get());
        StrongOwner->RemoveFinalizeRequest(this);
    }
    Builder = nullptr;
    Owner = nullptr;
}

void UWorldBakedMeshFinalizeRequest::FailStarted(const FString& Reason)
{
    check(IsInGameThread());
    const uint64 LocalTicket = Ticket;
    FGWorldBakedStaticMeshNativeCallback LocalStatic = MoveTemp(StaticCallback);
    FGWorldBakedSkeletalMeshNativeCallback LocalSkeletal = MoveTemp(SkeletalCallback);
    Ticket = 0;
    FglTFRuntimeSafety::ReportRecoverableFailure(SourceReference, Reason);
    Cleanup();
    if (LocalStatic) LocalStatic(nullptr);
    if (LocalSkeletal) LocalSkeletal(nullptr);
    FglTFRuntimeSafety::CompleteOperation(LocalTicket);
}

void UWorldBakedMeshFinalizeRequest::RejectBeforeStart(const FString& Reason)
{
    check(IsInGameThread());
    FGWorldBakedStaticMeshNativeCallback LocalStatic = MoveTemp(StaticCallback);
    FGWorldBakedSkeletalMeshNativeCallback LocalSkeletal = MoveTemp(SkeletalCallback);
    FglTFRuntimeSafety::ReportRecoverableFailure(SourceReference, Reason);
    Cleanup();
    if (LocalStatic) LocalStatic(nullptr);
    if (LocalSkeletal) LocalSkeletal(nullptr);
}

void UWorldBakedMeshFinalizeRequest::BeginStatic(
    const uint64 InTicket,
    TArray<FglTFRuntimeMeshLOD>&& LODs,
    const FglTFRuntimeStaticMeshConfig& Config)
{
    check(IsInGameThread());
    Ticket = InTicket;
    if (!IsValid(Owner.Get()) || !IsValid(Builder.Get()))
    {
        FailStarted(TEXT("The baked model facade or mesh builder expired before static finalization"));
        return;
    }
    if (!WorldBakedModelAssetPrivate::InitializeEmptyMeshBuilder(Builder.Get()))
    {
        FailStarted(TEXT("Could not initialize the decoded-data static mesh finalizer"));
        return;
    }
#if (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5) || ENGINE_MAJOR_VERSION > 5
    StaticRayTracingHandle = FglTFRuntimeParser::OnPreCreatedStaticMesh.AddLambda(
        [](FglTFRuntimeStaticMeshContextRef Context)
        {
            if (Context->StaticMesh) Context->StaticMesh->bSupportRayTracing = false;
        });
#endif
    FglTFRuntimeStaticMeshAsync NativeCallback;
    NativeCallback.BindDynamic(this, &UWorldBakedMeshFinalizeRequest::HandleStaticMeshFinalized);
    Builder->LoadStaticMeshFromRuntimeLODsAsync(LODs, NativeCallback, Config);
}

void UWorldBakedMeshFinalizeRequest::BeginSkeletal(
    const uint64 InTicket,
    TArray<FglTFRuntimeMeshLOD>&& LODs,
    const FglTFRuntimeSkeletalMeshConfig& Config)
{
    check(IsInGameThread());
    Ticket = InTicket;
    if (!IsValid(Owner.Get()) || !IsValid(Builder.Get()))
    {
        FailStarted(TEXT("The baked model facade or mesh builder expired before skeletal finalization"));
        return;
    }
    if (!WorldBakedModelAssetPrivate::InitializeEmptyMeshBuilder(Builder.Get()))
    {
        FailStarted(TEXT("Could not initialize the decoded-data skeletal mesh finalizer"));
        return;
    }
    FglTFRuntimeSkeletalMeshAsync NativeCallback;
    NativeCallback.BindDynamic(this, &UWorldBakedMeshFinalizeRequest::HandleSkeletalMeshFinalized);
    Builder->LoadSkeletalMeshFromRuntimeLODsAsync(LODs, INDEX_NONE, NativeCallback, Config);
}

void UWorldBakedMeshFinalizeRequest::HandleStaticMeshFinalized(UStaticMesh* StaticMesh)
{
    check(IsInGameThread());
    const uint64 LocalTicket = Ticket;
    const FString LocalReference = SourceReference;
    FGWorldBakedStaticMeshNativeCallback Callback = MoveTemp(StaticCallback);
    Ticket = 0;
    Cleanup();
    if (!IsValid(StaticMesh))
    {
        FglTFRuntimeSafety::ReportRecoverableFailure(
            LocalReference, TEXT("Streamed decoded-data static mesh build failed"));
    }
    if (Callback) Callback(StaticMesh);
    FglTFRuntimeSafety::CompleteOperationAfterCallback(LocalTicket);
}

void UWorldBakedMeshFinalizeRequest::HandleSkeletalMeshFinalized(USkeletalMesh* SkeletalMesh)
{
    check(IsInGameThread());
    const uint64 LocalTicket = Ticket;
    const FString LocalReference = SourceReference;
    FGWorldBakedSkeletalMeshNativeCallback Callback = MoveTemp(SkeletalCallback);
    Ticket = 0;
    Cleanup();
    if (!IsValid(SkeletalMesh))
    {
        FglTFRuntimeSafety::ReportRecoverableFailure(
            LocalReference, TEXT("Streamed decoded-data skeletal mesh build failed"));
    }
    if (Callback) Callback(SkeletalMesh);
    FglTFRuntimeSafety::CompleteOperationAfterCallback(LocalTicket);
}

void UWorldBakedModelAsset::RemoveFinalizeRequest(UWorldBakedMeshFinalizeRequest* Request)
{
    check(IsInGameThread());
    ActiveFinalizeRequests.RemoveSingleSwap(Request, EAllowShrinking::No);
}

void UWorldBakedModelAsset::QueueStaticRuntimeLODFinalizer(
    TArray<FglTFRuntimeMeshLOD>&& LODs,
    const FglTFRuntimeStaticMeshConfig& Config,
    FGWorldBakedStaticMeshNativeCallback Callback,
    const FString& SourceReference,
    const FWorldBakedBuildReferenceGuard& ReferenceGuard)
{
    check(IsInGameThread());
    FString Error;
    UglTFRuntimeAsset* Builder = CreateMeshBuilder(Error);
    if (!IsValid(Builder))
    {
        FglTFRuntimeSafety::ReportRecoverableFailure(SourceReference, Error);
        if (Callback) Callback(nullptr);
        return;
    }
    RetainAsyncBuildReferences(Builder, ReferenceGuard);

    UWorldBakedMeshFinalizeRequest* Request = NewObject<UWorldBakedMeshFinalizeRequest>(this);
    if (!IsValid(Request))
    {
        ReleaseAsyncBuildReferences(Builder);
        FglTFRuntimeSafety::ReportRecoverableFailure(SourceReference, TEXT("Could not allocate static finalizer request"));
        if (Callback) Callback(nullptr);
        return;
    }
    Request->InitializeStatic(this, Builder, SourceReference, MoveTemp(Callback));
    ActiveFinalizeRequests.Add(Request);

    TWeakObjectPtr<UWorldBakedMeshFinalizeRequest> WeakRequest(Request);
    FglTFRuntimeSafety::EnqueueOperation(
        this,
        Builder,
        TEXT("Finalize streamed baked static mesh asynchronously"),
        [WeakRequest, LODs = MoveTemp(LODs), Config](const uint64 Ticket) mutable
        {
            if (UWorldBakedMeshFinalizeRequest* StrongRequest = WeakRequest.Get())
            {
                StrongRequest->BeginStatic(Ticket, MoveTemp(LODs), Config);
                return;
            }
            FglTFRuntimeSafety::CompleteOperation(Ticket);
        },
        [WeakRequest](const FString& Reason)
        {
            if (UWorldBakedMeshFinalizeRequest* StrongRequest = WeakRequest.Get())
            {
                StrongRequest->RejectBeforeStart(Reason);
            }
        });
}

void UWorldBakedModelAsset::QueueSkeletalRuntimeLODFinalizer(
    TArray<FglTFRuntimeMeshLOD>&& LODs,
    const FglTFRuntimeSkeletalMeshConfig& Config,
    FGWorldBakedSkeletalMeshNativeCallback Callback,
    const FString& SourceReference,
    const FWorldBakedBuildReferenceGuard& ReferenceGuard)
{
    check(IsInGameThread());
    FString Error;
    UglTFRuntimeAsset* Builder = CreateMeshBuilder(Error);
    if (!IsValid(Builder))
    {
        FglTFRuntimeSafety::ReportRecoverableFailure(SourceReference, Error);
        if (Callback) Callback(nullptr);
        return;
    }
    RetainAsyncBuildReferences(Builder, ReferenceGuard);

    UWorldBakedMeshFinalizeRequest* Request = NewObject<UWorldBakedMeshFinalizeRequest>(this);
    if (!IsValid(Request))
    {
        ReleaseAsyncBuildReferences(Builder);
        FglTFRuntimeSafety::ReportRecoverableFailure(SourceReference, TEXT("Could not allocate skeletal finalizer request"));
        if (Callback) Callback(nullptr);
        return;
    }
    Request->InitializeSkeletal(this, Builder, SourceReference, MoveTemp(Callback));
    ActiveFinalizeRequests.Add(Request);

    TWeakObjectPtr<UWorldBakedMeshFinalizeRequest> WeakRequest(Request);
    FglTFRuntimeSafety::EnqueueOperation(
        this,
        Builder,
        TEXT("Finalize streamed baked skeletal mesh asynchronously"),
        [WeakRequest, LODs = MoveTemp(LODs), Config](const uint64 Ticket) mutable
        {
            if (UWorldBakedMeshFinalizeRequest* StrongRequest = WeakRequest.Get())
            {
                StrongRequest->BeginSkeletal(Ticket, MoveTemp(LODs), Config);
                return;
            }
            FglTFRuntimeSafety::CompleteOperation(Ticket);
        },
        [WeakRequest](const FString& Reason)
        {
            if (UWorldBakedMeshFinalizeRequest* StrongRequest = WeakRequest.Get())
            {
                StrongRequest->RejectBeforeStart(Reason);
            }
        });
}

bool UWorldBakedModelAsset::TryBeginSharedRenderMeshRequest(
    const TArray<int32>& MeshIndices,
    const FglTFRuntimeStaticMeshConfig& Config,
    FGWorldBakedStaticMeshNativeCallback& InOutCallback,
    FString& OutCacheKey)
{
    check(IsInGameThread());
    OutCacheKey.Reset();

    // Explicit opt-in contract: WorldSceneStreamAction sets this facade as Outer only for visual
    // static geometry. Physics/nav/CPU-access meshes keep their world context and never enter this
    // cache, avoiding accidental cross-world sharing of runtime collision state.
    if (Config.Outer != this
        || Config.bBuildComplexCollision
        || Config.bBuildSimpleCollision
        || Config.bBuildNavCollision
        || Config.bAllowCPUAccess
        || MeshIndices.IsEmpty())
    {
        return false;
    }

    OutCacheKey = WorldBakedModelAssetPrivate::BuildSharedRenderMeshCacheKey(MeshIndices, Config);
    if (TWeakObjectPtr<UStaticMesh>* Cached = SharedRenderMeshCache.Find(OutCacheKey))
    {
        if (UStaticMesh* Existing = Cached->Get(); IsValid(Existing))
        {
            FGWorldBakedStaticMeshNativeCallback Callback = MoveTemp(InOutCallback);
            if (Callback) Callback(Existing);
            return true;
        }
        SharedRenderMeshCache.Remove(OutCacheKey);
    }

    if (TArray<FGWorldBakedStaticMeshNativeCallback>* Waiters =
            PendingSharedRenderMeshBuilds.Find(OutCacheKey))
    {
        Waiters->Add(MoveTemp(InOutCallback));
        return true;
    }

    TArray<FGWorldBakedStaticMeshNativeCallback>& Waiters =
        PendingSharedRenderMeshBuilds.Add(OutCacheKey);
    Waiters.Add(MoveTemp(InOutCallback));

    // The first caller is the build leader. Replace its callback with one fan-out callback that
    // publishes the result to every request that arrived while I/O/native finalization was active.
    TWeakObjectPtr<UWorldBakedModelAsset> WeakThis(this);
    const FString CacheKeyCopy = OutCacheKey;
    InOutCallback = [WeakThis, CacheKeyCopy](UStaticMesh* Mesh)
    {
        if (UWorldBakedModelAsset* Self = WeakThis.Get())
        {
            Self->CompleteSharedRenderMeshRequest(CacheKeyCopy, Mesh);
        }
    };
    return false;
}

void UWorldBakedModelAsset::CompleteSharedRenderMeshRequest(
    const FString& CacheKey,
    UStaticMesh* Mesh)
{
    check(IsInGameThread());
    TArray<FGWorldBakedStaticMeshNativeCallback> Waiters;
    if (TArray<FGWorldBakedStaticMeshNativeCallback>* Pending =
            PendingSharedRenderMeshBuilds.Find(CacheKey))
    {
        Waiters = MoveTemp(*Pending);
        PendingSharedRenderMeshBuilds.Remove(CacheKey);
    }

    if (IsValid(Mesh))
    {
        SharedRenderMeshCache.Add(CacheKey, Mesh);
        // Weak entries cost little but can accumulate during long editor rebuild sessions.
        if (SharedRenderMeshCache.Num() > 2048)
        {
            for (auto It = SharedRenderMeshCache.CreateIterator(); It; ++It)
            {
                if (!It.Value().IsValid()) It.RemoveCurrent();
            }
        }
    }

    for (FGWorldBakedStaticMeshNativeCallback& Callback : Waiters)
    {
        if (Callback) Callback(Mesh);
    }
}

UStaticMesh* UWorldBakedModelAsset::LoadStaticMesh(
    const int32 MeshIndex,
    const FglTFRuntimeStaticMeshConfig& Config,
    FString* OutError)
{
    TArray<int32> MeshIndices;
    MeshIndices.Add(MeshIndex);
    return LoadStaticMeshLODs(MeshIndices, Config, OutError);
}

UStaticMesh* UWorldBakedModelAsset::LoadStaticMeshLODs(
    const TArray<int32>& MeshIndices,
    const FglTFRuntimeStaticMeshConfig& Config,
    FString* OutError)
{
    check(IsInGameThread());
    FString Error;
    FGWorldBakedAssetBundle Bundle;
    TSet<int32> CachedTextureIds;
    TSet<int32> CachedMaterialIds;
    if (!Config.MaterialsConfig.bSkipLoad)
    {
        CollectCachedTextureIds(CachedTextureIds);
        CollectCachedMaterialIds(Config.MaterialsConfig, CachedMaterialIds);
    }
    if (!Reader.IsValid() || !Manifest.IsValid()
        || !Reader->ReadMeshBundle(
            UUID, *Manifest, MeshIndices, INDEX_NONE,
            !Config.MaterialsConfig.bSkipLoad, Bundle, Error,
            &CachedTextureIds, &CachedMaterialIds))
    {
        if (Error.IsEmpty())
        {
            Error = TEXT("The baked model reader is not initialized");
        }
        if (OutError)
        {
            *OutError = Error;
        }
        return nullptr;
    }

    FWorldBakedBuildReferenceGuard ReferenceGuard;
    TArray<FglTFRuntimeMeshLOD> LODs;
    if (!BuildRuntimeLODs(
            Bundle, Config.MaterialsConfig, INDEX_NONE,
            ReferenceGuard, LODs, Error))
    {
        if (OutError)
        {
            *OutError = Error;
        }
        return nullptr;
    }
    Bundle.Reset(); // Release serialized float/mip arrays before native mesh finalization peaks.

    // NewObject alone is not a stack GC reference. Keep the transient finalizer strongly reachable
    // across any nested engine work performed by glTFRuntime, then release it with this call frame.
    TStrongObjectPtr<UglTFRuntimeAsset> BuilderGuard(CreateMeshBuilder(Error));
    UglTFRuntimeAsset* const Builder = BuilderGuard.Get();
    UStaticMesh* Result = nullptr;
    bool bBuilderInitialized = false;
    const bool bExecuted = Builder
        && FglTFRuntimeSafety::ExecuteSynchronousOperation(
            TEXT("Build baked static mesh"),
            [&Result, &bBuilderInitialized, Builder, &LODs, &Config, &ReferenceGuard]()
            {
                bBuilderInitialized =
                    WorldBakedModelAssetPrivate::InitializeEmptyMeshBuilder(Builder);
                if (bBuilderInitialized)
                {
                    WorldBakedModelAssetPrivate::FScopedDisableStreamedStaticMeshRayTracing
                        DisableRayTracing;
                    Result = Builder->LoadStaticMeshFromRuntimeLODs(LODs, Config);
                    // ExecuteSynchronousOperation may pump another queued native job before it
                    // returns, so protect the newly created mesh during that re-entrant window.
                    ReferenceGuard.Add(Result);
                }
            });
    if (!bExecuted && Error.IsEmpty())
    {
        Error = TEXT("The glTFRuntime mesh finalizer is busy or shutting down");
    }
    if (bExecuted && !bBuilderInitialized && Error.IsEmpty())
    {
        Error = TEXT("Could not initialize the decoded-data mesh finalizer");
    }
    if (!Result && Error.IsEmpty())
    {
        Error = TEXT("Decoded-data static mesh finalization failed");
    }
    if (OutError)
    {
        *OutError = Error;
    }
    return Result;
}

void UWorldBakedModelAsset::LoadStaticMeshAsync(
    const int32 MeshIndex,
    const FglTFRuntimeStaticMeshAsync& Callback,
    const FglTFRuntimeStaticMeshConfig& Config)
{
    FglTFRuntimeStaticMeshAsync DynamicCallback = Callback;
    LoadStaticMeshAsyncNative(
        MeshIndex,
        [DynamicCallback](UStaticMesh* Mesh) mutable
        {
            DynamicCallback.ExecuteIfBound(Mesh);
        },
        Config);
}

void UWorldBakedModelAsset::LoadStaticMeshLODsAsync(
    const TArray<int32>& MeshIndices,
    const FglTFRuntimeStaticMeshAsync& Callback,
    const FglTFRuntimeStaticMeshConfig& Config)
{
    FglTFRuntimeStaticMeshAsync DynamicCallback = Callback;
    LoadStaticMeshLODsAsyncNative(
        MeshIndices,
        [DynamicCallback](UStaticMesh* Mesh) mutable
        {
            DynamicCallback.ExecuteIfBound(Mesh);
        },
        Config);
}

void UWorldBakedModelAsset::LoadStaticMeshAsyncNative(
    const int32 MeshIndex,
    FGWorldBakedStaticMeshNativeCallback Callback,
    const FglTFRuntimeStaticMeshConfig& Config)
{
    TArray<int32> MeshIndices;
    MeshIndices.Add(MeshIndex);
    LoadStaticMeshLODsAsyncNative(MeshIndices, MoveTemp(Callback), Config);
}

void UWorldBakedModelAsset::LoadStaticMeshLODsAsyncNative(
    const TArray<int32>& MeshIndices,
    FGWorldBakedStaticMeshNativeCallback Callback,
    const FglTFRuntimeStaticMeshConfig& Config)
{
    check(IsInGameThread());
    FString SharedRenderMeshCacheKey;
    if (TryBeginSharedRenderMeshRequest(
            MeshIndices, Config, Callback, SharedRenderMeshCacheKey))
    {
        return;
    }

    const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> LocalReader = Reader;
    const TSharedPtr<FGWorldModelManifest, ESPMode::ThreadSafe> LocalManifest = Manifest;
    const FGuid LocalUUID = UUID;
    const FString LocalReference = Reference;
    TWeakObjectPtr<UWorldBakedModelAsset> WeakThis(this);
    TSet<int32> CachedTextureIds;
    TSet<int32> CachedMaterialIds;
    if (!Config.MaterialsConfig.bSkipLoad)
    {
        CollectCachedTextureIds(CachedTextureIds);
        CollectCachedMaterialIds(Config.MaterialsConfig, CachedMaterialIds);
    }

    const bool bQueued = LocalReader.IsValid() && LocalManifest.IsValid()
        && FSafeFileIO::RunTrackedWorker(
            [WeakThis, LocalReader, LocalManifest, LocalUUID, LocalReference,
                MeshIndices, Callback, Config, CachedTextureIds, CachedMaterialIds]() mutable
            {
                FGWorldBakedAssetBundle Bundle;
                TArray<FglTFRuntimeMeshLOD> LODs;
                TArray<TArray<int32>> MaterialIds;
                FString Error;
                const bool bRead = LocalReader->ReadMeshBundle(
                    LocalUUID, *LocalManifest, MeshIndices, INDEX_NONE,
                    !Config.MaterialsConfig.bSkipLoad, Bundle, Error,
                    &CachedTextureIds, &CachedMaterialIds);
                const bool bPrepared = bRead
                    && WorldBakedModelAssetPrivate::BuildRuntimeLODsDetached(
                        Bundle, INDEX_NONE, !Config.MaterialsConfig.bSkipLoad,
                        LODs, MaterialIds, Error);

                // Geometry/joints/morphs are now fully detached in RuntimeLODs. Do not carry the
                // duplicate baked mesh arrays back to GT; only material/texture payloads remain.
                if (bPrepared) Bundle.Meshes.Reset();

                FSafeFileIO::DispatchTrackedGameThread(
                    [WeakThis, LocalReference, Callback, Config, bPrepared,
                        Bundle = MoveTemp(Bundle), LODs = MoveTemp(LODs),
                        MaterialIds = MoveTemp(MaterialIds), Error = MoveTemp(Error)]() mutable
                    {
                        UWorldBakedModelAsset* Self = WeakThis.Get();
                        if (IsValid(Self) && bPrepared)
                        {
                            FWorldBakedBuildReferenceGuard ReferenceGuard;
                            if (Self->AttachRuntimeMaterials(
                                    Bundle, Config.MaterialsConfig, MaterialIds,
                                    ReferenceGuard, LODs, Error))
                            {
                                Bundle.Reset();
                                Self->QueueStaticRuntimeLODFinalizer(
                                    MoveTemp(LODs), Config, Callback,
                                    LocalReference, ReferenceGuard);
                                return;
                            }
                        }
                        else if (!IsValid(Self) && Error.IsEmpty())
                        {
                            Error = TEXT("The baked model facade was destroyed during streaming");
                        }
                        if (Error.IsEmpty()) Error = TEXT("Streamed baked static mesh preparation failed");
                        FglTFRuntimeSafety::ReportRecoverableFailure(LocalReference, Error);
                        if (Callback) Callback(nullptr);
                    });
            });

    if (!bQueued)
    {
        FglTFRuntimeSafety::ReportRecoverableFailure(
            LocalReference, TEXT("The baked mesh I/O queue is shutting down"));
        if (Callback) Callback(nullptr);
    }
}

USkeletalMesh* UWorldBakedModelAsset::LoadSkeletalMesh(
    const int32 MeshIndex,
    const int32 SkinIndex,
    const FglTFRuntimeSkeletalMeshConfig& Config,
    FString* OutError)
{
    check(IsInGameThread());
    FString Error;
    FGWorldBakedAssetBundle Bundle;
    TArray<int32> MeshIndices;
    MeshIndices.Add(MeshIndex);
    TSet<int32> CachedTextureIds;
    TSet<int32> CachedMaterialIds;
    if (!Config.MaterialsConfig.bSkipLoad)
    {
        CollectCachedTextureIds(CachedTextureIds);
        CollectCachedMaterialIds(Config.MaterialsConfig, CachedMaterialIds);
    }
    if (!Reader.IsValid() || !Manifest.IsValid()
        || !Reader->ReadMeshBundle(
            UUID, *Manifest, MeshIndices, SkinIndex,
            !Config.MaterialsConfig.bSkipLoad, Bundle, Error,
            &CachedTextureIds, &CachedMaterialIds))
    {
        if (Error.IsEmpty())
        {
            Error = TEXT("The baked model reader is not initialized");
        }
        if (OutError)
        {
            *OutError = Error;
        }
        return nullptr;
    }

    FWorldBakedBuildReferenceGuard ReferenceGuard;
    TArray<FglTFRuntimeMeshLOD> LODs;
    if (!BuildRuntimeLODs(
            Bundle, Config.MaterialsConfig, SkinIndex,
            ReferenceGuard, LODs, Error))
    {
        if (OutError)
        {
            *OutError = Error;
        }
        return nullptr;
    }
    Bundle.Reset();

    // The reference skeleton and joint map are already present in RuntimeLODs. Telling the plugin
    // to ignore parser skins guarantees it never looks for a GLB skin object on the empty parser.
    FglTFRuntimeSkeletalMeshConfig LocalConfig = Config;
    LocalConfig.bIgnoreSkin = true;
    LocalConfig.OverrideSkinIndex = INDEX_NONE;

    // Protect the transient empty-parser finalizer from nested GC during skeletal construction.
    TStrongObjectPtr<UglTFRuntimeAsset> BuilderGuard(CreateMeshBuilder(Error));
    UglTFRuntimeAsset* const Builder = BuilderGuard.Get();
    USkeletalMesh* Result = nullptr;
    bool bBuilderInitialized = false;
    const bool bExecuted = Builder
        && FglTFRuntimeSafety::ExecuteSynchronousOperation(
            TEXT("Build baked skeletal mesh"),
            [&Result, &bBuilderInitialized, Builder, &LODs, &LocalConfig, &ReferenceGuard]()
            {
                bBuilderInitialized =
                    WorldBakedModelAssetPrivate::InitializeEmptyMeshBuilder(Builder);
                if (bBuilderInitialized)
                {
                    Result = Builder->LoadSkeletalMeshFromRuntimeLODs(
                        LODs, INDEX_NONE, LocalConfig);
                    // Hold the result across the coordinator's post-operation queue pump.
                    ReferenceGuard.Add(Result);
                }
            });
    if (!bExecuted && Error.IsEmpty())
    {
        Error = TEXT("The glTFRuntime skeletal finalizer is busy or shutting down");
    }
    if (bExecuted && !bBuilderInitialized && Error.IsEmpty())
    {
        Error = TEXT("Could not initialize the decoded-data skeletal mesh finalizer");
    }
    if (!Result && Error.IsEmpty())
    {
        Error = TEXT("Decoded-data skeletal mesh finalization failed");
    }
    if (OutError)
    {
        *OutError = Error;
    }
    return Result;
}

void UWorldBakedModelAsset::LoadSkeletalMeshAsync(
    const int32 MeshIndex,
    const int32 SkinIndex,
    const FglTFRuntimeSkeletalMeshAsync& Callback,
    const FglTFRuntimeSkeletalMeshConfig& Config)
{
    FglTFRuntimeSkeletalMeshAsync DynamicCallback = Callback;
    LoadSkeletalMeshAsyncNative(
        MeshIndex, SkinIndex,
        [DynamicCallback](USkeletalMesh* Mesh) mutable
        {
            DynamicCallback.ExecuteIfBound(Mesh);
        },
        Config);
}

void UWorldBakedModelAsset::LoadSkeletalMeshAsyncNative(
    const int32 MeshIndex,
    const int32 SkinIndex,
    FGWorldBakedSkeletalMeshNativeCallback Callback,
    const FglTFRuntimeSkeletalMeshConfig& Config)
{
    check(IsInGameThread());
    const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> LocalReader = Reader;
    const TSharedPtr<FGWorldModelManifest, ESPMode::ThreadSafe> LocalManifest = Manifest;
    const FGuid LocalUUID = UUID;
    const FString LocalReference = Reference;
    TWeakObjectPtr<UWorldBakedModelAsset> WeakThis(this);
    TArray<int32> MeshIndices;
    MeshIndices.Add(MeshIndex);
    TSet<int32> CachedTextureIds;
    TSet<int32> CachedMaterialIds;
    if (!Config.MaterialsConfig.bSkipLoad)
    {
        CollectCachedTextureIds(CachedTextureIds);
        CollectCachedMaterialIds(Config.MaterialsConfig, CachedMaterialIds);
    }

    const bool bQueued = LocalReader.IsValid() && LocalManifest.IsValid()
        && FSafeFileIO::RunTrackedWorker(
            [WeakThis, LocalReader, LocalManifest, LocalUUID, LocalReference,
                MeshIndices, SkinIndex, Callback, Config, CachedTextureIds, CachedMaterialIds]() mutable
            {
                FGWorldBakedAssetBundle Bundle;
                TArray<FglTFRuntimeMeshLOD> LODs;
                TArray<TArray<int32>> MaterialIds;
                FString Error;
                const bool bRead = LocalReader->ReadMeshBundle(
                    LocalUUID, *LocalManifest, MeshIndices, SkinIndex,
                    !Config.MaterialsConfig.bSkipLoad, Bundle, Error,
                    &CachedTextureIds, &CachedMaterialIds);
                const bool bPrepared = bRead
                    && WorldBakedModelAssetPrivate::BuildRuntimeLODsDetached(
                        Bundle, SkinIndex, !Config.MaterialsConfig.bSkipLoad,
                        LODs, MaterialIds, Error);
                if (bPrepared) Bundle.Meshes.Reset();

                FSafeFileIO::DispatchTrackedGameThread(
                    [WeakThis, LocalReference, Callback, Config, bPrepared,
                        Bundle = MoveTemp(Bundle), LODs = MoveTemp(LODs),
                        MaterialIds = MoveTemp(MaterialIds), Error = MoveTemp(Error)]() mutable
                    {
                        UWorldBakedModelAsset* Self = WeakThis.Get();
                        if (IsValid(Self) && bPrepared)
                        {
                            FWorldBakedBuildReferenceGuard ReferenceGuard;
                            if (Self->AttachRuntimeMaterials(
                                    Bundle, Config.MaterialsConfig, MaterialIds,
                                    ReferenceGuard, LODs, Error))
                            {
                                Bundle.Reset();
                                FglTFRuntimeSkeletalMeshConfig LocalConfig = Config;
                                // Skeleton + authoritative joint maps are embedded in RuntimeLODs.
                                // Never re-read a parser skin during final reconstruction.
                                LocalConfig.bIgnoreSkin = true;
                                LocalConfig.OverrideSkinIndex = INDEX_NONE;
                                Self->QueueSkeletalRuntimeLODFinalizer(
                                    MoveTemp(LODs), LocalConfig, Callback,
                                    LocalReference, ReferenceGuard);
                                return;
                            }
                        }
                        else if (!IsValid(Self) && Error.IsEmpty())
                        {
                            Error = TEXT("The baked model facade was destroyed during streaming");
                        }
                        if (Error.IsEmpty()) Error = TEXT("Streamed baked skeletal mesh preparation failed");
                        FglTFRuntimeSafety::ReportRecoverableFailure(LocalReference, Error);
                        if (Callback) Callback(nullptr);
                    });
            });

    if (!bQueued)
    {
        FglTFRuntimeSafety::ReportRecoverableFailure(
            LocalReference, TEXT("The baked skeletal-mesh I/O queue is shutting down"));
        if (Callback) Callback(nullptr);
    }
}
