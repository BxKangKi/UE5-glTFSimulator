// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldBakedModelAsset.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "System/WorldBakedModelAsset.h"

#include "Async/ParallelFor.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "PixelFormat.h"
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

private:
    TArray<TObjectPtr<UObject>> References;
};

namespace WorldBakedModelAssetPrivate
{
    constexpr int32 MaxTextureMips = 32;
    constexpr int64 MaxTextureMipBytes = 1024ll * 1024ll * 1024ll;

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

    for (int32 MipIndex = FirstRuntimeMip; MipIndex < Baked.Mips.Num(); ++MipIndex)
    {
        const FGWorldBakedTextureMip& Source = Baked.Mips[MipIndex];
        FTexture2DMipMap* Mip = new FTexture2DMipMap();
        Mip->SizeX = Source.SizeX;
        Mip->SizeY = Source.SizeY;
        Mip->SizeZ = Source.SizeZ;

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
    return Material;
}

bool UWorldBakedModelAsset::BuildRuntimeLODs(
    const FGWorldBakedAssetBundle& Bundle,
    const FglTFRuntimeMaterialsConfig& MaterialsConfig,
    const int32 SkinIndex,
    FWorldBakedBuildReferenceGuard& ReferenceGuard,
    TArray<FglTFRuntimeMeshLOD>& OutLODs,
    FString& OutError)
{
    check(IsInGameThread());
    OutLODs.Reset();
    OutError.Reset();

    // A native caller can invoke this facade through a transient raw pointer. Keep the facade and
    // its reflected node/cache state alive across nested UpdateResource/finalizer collection points.
    ReferenceGuard.Add(this);

    TMap<int32, UTexture2D*> Textures;
    TMap<int32, UMaterialInterface*> Materials;
    if (!MaterialsConfig.bSkipLoad)
    {
        // Each material retains its base plus one MID; texture parameters can append a few more.
        // Reserve the deterministic lower bound to avoid reallocating the GC-visible pointer table.
        ReferenceGuard.Reserve(Bundle.Textures.Num() + Bundle.Materials.Num() * 2);
        Textures.Reserve(Bundle.Textures.Num());
        for (const FGWorldBakedTexture& Baked : Bundle.Textures)
        {
            UTexture2D* Texture = CreateTexture(Baked, ReferenceGuard, OutError);
            if (!Texture)
            {
                return false;
            }
            Textures.Add(Baked.TextureId, Texture);
        }

        Materials.Reserve(Bundle.Materials.Num());
        for (const FGWorldBakedMaterial& Baked : Bundle.Materials)
        {
            UMaterialInterface* Material =
                CreateMaterial(
                    Baked, Textures, MaterialsConfig, ReferenceGuard, OutError);
            if (!Material)
            {
                return false;
            }
            Materials.Add(Baked.MaterialId, Material);
        }
    }

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

    // Validate UObject material references serially before entering the worker-only geometry copy.
    // This keeps every error path and UObject validity test on the game thread while allowing the
    // large vertex/normal/UV/morph array conversions to use all available CPU cores.
    if (!MaterialsConfig.bSkipLoad)
    {
        for (const FGWorldBakedMesh& BakedMesh : Bundle.Meshes)
        {
            for (const FGWorldBakedPrimitive& Baked : BakedMesh.Primitives)
            {
                if (Baked.MaterialId == INDEX_NONE)
                {
                    continue;
                }
                UMaterialInterface* const* Found = Materials.Find(Baked.MaterialId);
                if (!Found || !IsValid(*Found))
                {
                    OutError = TEXT("A mesh references a missing baked material");
                    return false;
                }
            }
        }
    }

    OutLODs.SetNum(Bundle.Meshes.Num());
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
        PrimitiveTasks.Reserve(PrimitiveTasks.Num() + BakedMesh.Primitives.Num());
        for (int32 PrimitiveIndex = 0; PrimitiveIndex < BakedMesh.Primitives.Num(); ++PrimitiveIndex)
        {
            PrimitiveTasks.Emplace(MeshArrayIndex, PrimitiveIndex);
        }
    }

    // Flatten the work list instead of nesting ParallelFor calls. This avoids oversubscribing the
    // global UE task pool when several mesh groups are already streaming concurrently.
    ParallelFor(PrimitiveTasks.Num(), [&Bundle, Skin, &MaterialsConfig, &Materials, &OutLODs, &PrimitiveTasks](const int32 TaskIndex)
    {
        const int32 MeshArrayIndex = PrimitiveTasks[TaskIndex].Key;
        const int32 PrimitiveIndex = PrimitiveTasks[TaskIndex].Value;
        const FGWorldBakedMesh& BakedMesh = Bundle.Meshes[MeshArrayIndex];
        const FGWorldBakedPrimitive& Baked = BakedMesh.Primitives[PrimitiveIndex];
        FglTFRuntimePrimitive& Primitive = OutLODs[MeshArrayIndex].Primitives[PrimitiveIndex];

        Primitive.Positions.Reserve(Baked.Positions.Num());
        for (const FVector3f& Value : Baked.Positions)
        {
            Primitive.Positions.Add(FVector(Value));
        }
        Primitive.Normals.Reserve(Baked.Normals.Num());
        for (const FVector3f& Value : Baked.Normals)
        {
            Primitive.Normals.Add(FVector(Value));
        }
        Primitive.Tangents.Reserve(Baked.Tangents.Num());
        for (const FVector4f& Value : Baked.Tangents)
        {
            Primitive.Tangents.Add(FVector4(Value));
        }

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
        for (const FVector4f& Value : Baked.Colors)
        {
            Primitive.Colors.Add(FVector4(Value));
        }

        Primitive.MorphTargets.Reserve(Baked.MorphTargets.Num());
        for (const FGWorldBakedMorphTarget& BakedMorph : Baked.MorphTargets)
        {
            FglTFRuntimeMorphTarget& Morph = Primitive.MorphTargets.AddDefaulted_GetRef();
            Morph.Name = BakedMorph.Name;
            Morph.Positions.Reserve(BakedMorph.Positions.Num());
            for (const FVector3f& Value : BakedMorph.Positions)
            {
                Morph.Positions.Add(FVector(Value));
            }
            Morph.Normals.Reserve(BakedMorph.Normals.Num());
            for (const FVector3f& Value : BakedMorph.Normals)
            {
                Morph.Normals.Add(FVector(Value));
            }
        }

        Primitive.OverrideBoneMap = Skin ? Skin->JointBoneMap : Baked.BoneMap;
        Primitive.WeightMaps = Baked.WeightMaps;
        Primitive.MaterialName = Baked.MaterialName;
        Primitive.Mode = Baked.Mode;
        Primitive.bHasMaterial = !MaterialsConfig.bSkipLoad && Baked.bHasMaterial;
        Primitive.bHighPrecisionUVs = Baked.bHighPrecisionUVs;
        Primitive.bHighPrecisionWeights = Baked.bHighPrecisionWeights;
        Primitive.bDisableShadows = Baked.bDisableShadows;
        Primitive.bHasIndices = Baked.bHasIndices;

        if (!MaterialsConfig.bSkipLoad && Baked.MaterialId != INDEX_NONE)
        {
            // Materials is immutable during this phase and every id was validated above.
            Primitive.Material = Materials.FindRef(Baked.MaterialId);
        }
    });

    if (OutLODs.IsEmpty())
    {
        OutError = TEXT("The requested baked mesh set is empty");
        return false;
    }
    return true;
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

    // Parser attachment is intentionally deferred into the caller's serialized native operation,
    // keeping creation and mesh finalization under one uninterrupted glTFRuntime gate ownership.
    return Builder;
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
    if (!Reader.IsValid() || !Manifest.IsValid()
        || !Reader->ReadMeshBundle(
            UUID, *Manifest, MeshIndices, INDEX_NONE,
            !Config.MaterialsConfig.bSkipLoad, Bundle, Error))
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
    TArray<int32> MeshIndices;
    MeshIndices.Add(MeshIndex);
    LoadStaticMeshLODsAsync(MeshIndices, Callback, Config);
}

void UWorldBakedModelAsset::LoadStaticMeshLODsAsync(
    const TArray<int32>& MeshIndices,
    const FglTFRuntimeStaticMeshAsync& Callback,
    const FglTFRuntimeStaticMeshConfig& Config)
{
    check(IsInGameThread());
    const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> LocalReader = Reader;
    const TSharedPtr<FGWorldModelManifest, ESPMode::ThreadSafe> LocalManifest = Manifest;
    const FGuid LocalUUID = UUID;
    const FString LocalReference = Reference;
    TWeakObjectPtr<UWorldBakedModelAsset> WeakThis(this);

    const bool bQueued = LocalReader.IsValid() && LocalManifest.IsValid()
        && FSafeFileIO::RunTrackedWorker(
            [WeakThis, LocalReader, LocalManifest, LocalUUID, LocalReference,
                MeshIndices, Callback, Config]() mutable
            {
                FGWorldBakedAssetBundle Bundle;
                FString Error;
                const bool bRead = LocalReader->ReadMeshBundle(
                    LocalUUID, *LocalManifest, MeshIndices, INDEX_NONE,
                    !Config.MaterialsConfig.bSkipLoad, Bundle, Error);

                FSafeFileIO::DispatchTrackedGameThread(
                    [WeakThis, LocalReference, Callback, Config, bRead,
                        Bundle = MoveTemp(Bundle), Error = MoveTemp(Error)]() mutable
                    {
                        UStaticMesh* Result = nullptr;
                        UWorldBakedModelAsset* Self = WeakThis.Get();
                        if (IsValid(Self) && bRead)
                        {
                            FWorldBakedBuildReferenceGuard ReferenceGuard;
                            TArray<FglTFRuntimeMeshLOD> LODs;
                            if (Self->BuildRuntimeLODs(
                                    Bundle, Config.MaterialsConfig, INDEX_NONE,
                                    ReferenceGuard, LODs, Error))
                            {
                                Bundle.Reset();
                                // The worker result is now on the game thread, but mesh finalization
                                // may enter nested engine code; explicitly guard the transient UObject.
                                TStrongObjectPtr<UglTFRuntimeAsset> BuilderGuard(
                                    Self->CreateMeshBuilder(Error));
                                UglTFRuntimeAsset* const Builder = BuilderGuard.Get();
                                bool bBuilderInitialized = false;
                                const bool bExecuted = Builder
                                    && FglTFRuntimeSafety::ExecuteSynchronousOperation(
                                        TEXT("Build streamed baked static mesh"),
                                        [&Result, &bBuilderInitialized, Builder, &LODs, &Config,
                                            &ReferenceGuard]()
                                        {
                                            bBuilderInitialized =
                                                WorldBakedModelAssetPrivate::InitializeEmptyMeshBuilder(
                                                    Builder);
                                            if (bBuilderInitialized)
                                            {
                                                Result = Builder->LoadStaticMeshFromRuntimeLODs(
                                                    LODs, Config);
                                                // Retain the result until the delegate has had a
                                                // chance to install it in reflected owner state.
                                                ReferenceGuard.Add(Result);
                                            }
                                        });
                                if (!bExecuted && Error.IsEmpty())
                                {
                                    Error = TEXT(
                                        "The glTFRuntime mesh finalizer is busy or shutting down");
                                }
                                if (bExecuted && !bBuilderInitialized && Error.IsEmpty())
                                {
                                    Error = TEXT(
                                        "Could not initialize the decoded-data mesh finalizer");
                                }
                            }
                        }
                        else if (!IsValid(Self) && Error.IsEmpty())
                        {
                            Error = TEXT("The baked model facade was destroyed during streaming");
                        }

                        if (!Result)
                        {
                            if (Error.IsEmpty())
                            {
                                Error = TEXT("Streamed decoded-data static mesh build failed");
                            }
                            FglTFRuntimeSafety::ReportRecoverableFailure(
                                LocalReference, Error);
                        }
                        Callback.ExecuteIfBound(Result);
                    });
            });

    if (!bQueued)
    {
        FglTFRuntimeSafety::ReportRecoverableFailure(
            LocalReference, TEXT("The baked mesh I/O queue is shutting down"));
        Callback.ExecuteIfBound(nullptr);
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
    if (!Reader.IsValid() || !Manifest.IsValid()
        || !Reader->ReadMeshBundle(
            UUID, *Manifest, MeshIndices, SkinIndex,
            !Config.MaterialsConfig.bSkipLoad, Bundle, Error))
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
    check(IsInGameThread());
    const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> LocalReader = Reader;
    const TSharedPtr<FGWorldModelManifest, ESPMode::ThreadSafe> LocalManifest = Manifest;
    const FGuid LocalUUID = UUID;
    const FString LocalReference = Reference;
    TWeakObjectPtr<UWorldBakedModelAsset> WeakThis(this);
    TArray<int32> MeshIndices;
    MeshIndices.Add(MeshIndex);

    const bool bQueued = LocalReader.IsValid() && LocalManifest.IsValid()
        && FSafeFileIO::RunTrackedWorker(
            [WeakThis, LocalReader, LocalManifest, LocalUUID, LocalReference,
                MeshIndices, SkinIndex, Callback, Config]() mutable
            {
                FGWorldBakedAssetBundle Bundle;
                FString Error;
                const bool bRead = LocalReader->ReadMeshBundle(
                    LocalUUID, *LocalManifest, MeshIndices, SkinIndex,
                    !Config.MaterialsConfig.bSkipLoad, Bundle, Error);

                FSafeFileIO::DispatchTrackedGameThread(
                    [WeakThis, LocalReference, SkinIndex, Callback, Config, bRead,
                        Bundle = MoveTemp(Bundle), Error = MoveTemp(Error)]() mutable
                    {
                        USkeletalMesh* Result = nullptr;
                        UWorldBakedModelAsset* Self = WeakThis.Get();
                        if (IsValid(Self) && bRead)
                        {
                            FWorldBakedBuildReferenceGuard ReferenceGuard;
                            TArray<FglTFRuntimeMeshLOD> LODs;
                            if (Self->BuildRuntimeLODs(
                                    Bundle, Config.MaterialsConfig, SkinIndex,
                                    ReferenceGuard, LODs, Error))
                            {
                                Bundle.Reset();
                                FglTFRuntimeSkeletalMeshConfig LocalConfig = Config;
                                LocalConfig.bIgnoreSkin = true;
                                LocalConfig.OverrideSkinIndex = INDEX_NONE;
                                // Keep the finalizer alive only for this serialized native call;
                                // the resulting mesh owns every resource it needs afterward.
                                TStrongObjectPtr<UglTFRuntimeAsset> BuilderGuard(
                                    Self->CreateMeshBuilder(Error));
                                UglTFRuntimeAsset* const Builder = BuilderGuard.Get();
                                bool bBuilderInitialized = false;
                                const bool bExecuted = Builder
                                    && FglTFRuntimeSafety::ExecuteSynchronousOperation(
                                        TEXT("Build streamed baked skeletal mesh"),
                                        [&Result, &bBuilderInitialized, Builder, &LODs, &LocalConfig,
                                            &ReferenceGuard]()
                                        {
                                            bBuilderInitialized =
                                                WorldBakedModelAssetPrivate::InitializeEmptyMeshBuilder(
                                                    Builder);
                                            if (bBuilderInitialized)
                                            {
                                                Result = Builder->LoadSkeletalMeshFromRuntimeLODs(
                                                    LODs, INDEX_NONE, LocalConfig);
                                                // The dynamic delegate becomes the long-lived owner;
                                                // retain the mesh until that callback finishes.
                                                ReferenceGuard.Add(Result);
                                            }
                                        });
                                if (!bExecuted && Error.IsEmpty())
                                {
                                    Error = TEXT(
                                        "The glTFRuntime skeletal finalizer is busy or shutting down");
                                }
                                if (bExecuted && !bBuilderInitialized && Error.IsEmpty())
                                {
                                    Error = TEXT(
                                        "Could not initialize the decoded-data skeletal mesh finalizer");
                                }
                            }
                        }
                        else if (!IsValid(Self) && Error.IsEmpty())
                        {
                            Error = TEXT("The baked model facade was destroyed during streaming");
                        }

                        if (!Result)
                        {
                            if (Error.IsEmpty())
                            {
                                Error = TEXT("Streamed decoded-data skeletal mesh build failed");
                            }
                            FglTFRuntimeSafety::ReportRecoverableFailure(
                                LocalReference, Error);
                        }
                        Callback.ExecuteIfBound(Result);
                    });
            });

    if (!bQueued)
    {
        FglTFRuntimeSafety::ReportRecoverableFailure(
            LocalReference, TEXT("The baked skeletal-mesh I/O queue is shutting down"));
        Callback.ExecuteIfBound(nullptr);
    }
}
