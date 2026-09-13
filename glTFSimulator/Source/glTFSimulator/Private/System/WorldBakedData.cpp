// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldBakedData.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "System/WorldBakedData.h"

#include "Character/CharacterBoneSchema.h"

#include "Animation/Skeleton.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "PixelFormat.h"
#include "Serialization/BulkData.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeParser.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include <type_traits>

namespace WorldBakedDataPrivate
{
    constexpr int32 MaxMeshes = 1000000;
    constexpr int32 MaxPrimitivesPerMesh = 65536;
    constexpr int32 MaxVerticesPerPrimitive = 100000000;
    constexpr int32 MaxIndicesPerPrimitive = 300000000;
    constexpr int32 MaxUvChannels = 16;
    constexpr int32 MaxJointSets = 8;
    constexpr int32 MaxMorphTargets = 4096;
    constexpr int32 MaxBones = 65536;
    constexpr int32 MaxMaterials = 1000000;
    constexpr int32 MaxTextures = 1000000;
    constexpr int32 MaxTextureMips = 32;
    constexpr int32 MaxMaterialParameters = 65536;
    constexpr int32 MaxAdditionalTransforms = 65536;
    constexpr int64 MaxTextureMipBytes = 1024ll * 1024ll * 1024ll;

    bool IsFiniteVector2f(const FVector2f& Value)
    {
        return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y);
    }

    bool IsFiniteVector3f(const FVector3f& Value)
    {
        return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y)
            && FMath::IsFinite(Value.Z);
    }

    bool IsFiniteVector4f(const FVector4f& Value)
    {
        return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y)
            && FMath::IsFinite(Value.Z) && FMath::IsFinite(Value.W);
    }

    bool IsFiniteColor(const FLinearColor& Value)
    {
        return FMath::IsFinite(Value.R) && FMath::IsFinite(Value.G)
            && FMath::IsFinite(Value.B) && FMath::IsFinite(Value.A);
    }

    bool IsPersistentCookedAsset(const UObject* Object)
    {
        const UPackage* Package = IsValid(Object) ? Object->GetOutermost() : nullptr;
        return Package && Object->IsAsset() && Object != GetTransientPackage()
            && Package != GetTransientPackage()
            // UE 5.8 no longer exposes a PKG_Transient package flag. Transient objects are
            // identified by RF_Transient and/or by living in the transient package itself.
            && !Object->HasAnyFlags(RF_Transient);
    }

    bool IsFiniteTransform(const FTransform& Value)
    {
        const FQuat Rotation = Value.GetRotation();
        return !Value.ContainsNaN() && Rotation.IsNormalized()
            && FMath::IsFinite(Value.GetLocation().X)
            && FMath::IsFinite(Value.GetLocation().Y)
            && FMath::IsFinite(Value.GetLocation().Z)
            && FMath::IsFinite(Value.GetScale3D().X)
            && FMath::IsFinite(Value.GetScale3D().Y)
            && FMath::IsFinite(Value.GetScale3D().Z);
    }

    /** Removes invalid and zero-area triangles before they reach Chaos collision cooking. */
    void SanitizeTriangleIndices(
        const TArray<FVector>& Positions,
        const int32 PrimitiveMode,
        const TArray<uint32>& SourceIndices,
        TArray<uint32>& OutIndices)
    {
        if (PrimitiveMode != 4 || SourceIndices.Num() < 3 || (SourceIndices.Num() % 3) != 0)
        {
            OutIndices = SourceIndices;
            return;
        }

        OutIndices.Reset(SourceIndices.Num());
        const uint32 VertexCount = static_cast<uint32>(Positions.Num());
        for (int32 Index = 0; Index < SourceIndices.Num(); Index += 3)
        {
            const uint32 A = SourceIndices[Index];
            const uint32 B = SourceIndices[Index + 1];
            const uint32 C = SourceIndices[Index + 2];
            if (A >= VertexCount || B >= VertexCount || C >= VertexCount
                || A == B || B == C || C == A)
            {
                continue;
            }

            const FVector EdgeAB = Positions[B] - Positions[A];
            const FVector EdgeAC = Positions[C] - Positions[A];
            if (FVector::CrossProduct(EdgeAB, EdgeAC).SizeSquared() <= UE_SMALL_NUMBER)
            {
                continue;
            }

            OutIndices.Add(A);
            OutIndices.Add(B);
            OutIndices.Add(C);
        }
    }

    template <typename ValueType>
    bool AllFinite(const TArray<ValueType>& Values)
    {
        for (const ValueType& Value : Values)
        {
            // Use the standard trait explicitly. TIsSame is no longer pulled in transitively by
            // CoreMinimal in UE 5.8's explicit/shared-PCH build configuration.
            if constexpr (std::is_same_v<ValueType, FVector3f>)
            {
                if (!IsFiniteVector3f(Value)) return false;
            }
            else if constexpr (std::is_same_v<ValueType, FVector4f>)
            {
                if (!IsFiniteVector4f(Value)) return false;
            }
        }
        return true;
    }

    /**
     * Computes the exact byte count Unreal expects for one platform mip. The multiplication is
     * deliberately checked before it reaches texture bulk-data allocation: archive fields are
     * untrusted even after their container CRC has passed.
     */
    bool GetExpectedMipBytes(
        const int32 PixelFormat,
        const FGWorldBakedTextureMip& Mip,
        int64& OutBytes)
    {
        OutBytes = 0;
        if (PixelFormat <= PF_Unknown || PixelFormat >= PF_MAX
            || Mip.SizeX <= 0 || Mip.SizeY <= 0 || Mip.SizeZ != 1)
        {
            return false;
        }

        const FPixelFormatInfo& Format = GPixelFormats[PixelFormat];
        if (!Format.Supported || Format.BlockSizeX <= 0 || Format.BlockSizeY <= 0
            || Format.BlockSizeZ <= 0 || Format.BlockBytes <= 0)
        {
            return false;
        }

        const int64 BlocksX = (static_cast<int64>(Mip.SizeX) + Format.BlockSizeX - 1)
            / Format.BlockSizeX;
        const int64 BlocksY = (static_cast<int64>(Mip.SizeY) + Format.BlockSizeY - 1)
            / Format.BlockSizeY;
        const int64 BlocksZ = (static_cast<int64>(Mip.SizeZ) + Format.BlockSizeZ - 1)
            / Format.BlockSizeZ;
        if (BlocksX <= 0 || BlocksY <= 0 || BlocksZ <= 0
            || BlocksX > MAX_int64 / BlocksY
            || BlocksX * BlocksY > MAX_int64 / BlocksZ
            || BlocksX * BlocksY * BlocksZ > MAX_int64 / Format.BlockBytes)
        {
            return false;
        }

        OutBytes = BlocksX * BlocksY * BlocksZ * Format.BlockBytes;
        return OutBytes > 0 && OutBytes <= MaxTextureMipBytes;
    }

    bool CaptureTexture(
        UTexture2D* Texture,
        const int32 TextureId,
        FGWorldBakedTexture& OutTexture,
        FString& OutError)
    {
        check(IsInGameThread());
        OutTexture = FGWorldBakedTexture();
        if (!IsValid(Texture))
        {
            OutError = TEXT("A runtime material referenced an invalid texture");
            return false;
        }

        FTexturePlatformData* PlatformData = Texture->GetPlatformData();
        if (!PlatformData || PlatformData->SizeX <= 0 || PlatformData->SizeY <= 0
            || PlatformData->PixelFormat <= PF_Unknown || PlatformData->PixelFormat >= PF_MAX
            || PlatformData->Mips.IsEmpty() || PlatformData->Mips.Num() > MaxTextureMips)
        {
            OutError = FString::Printf(
                TEXT("Texture platform data is unavailable or invalid: %s"),
                *GetNameSafe(Texture));
            return false;
        }

        OutTexture.TextureId = TextureId;
        OutTexture.Name = Texture->GetName();
        OutTexture.SizeX = PlatformData->SizeX;
        OutTexture.SizeY = PlatformData->SizeY;
        OutTexture.PixelFormat = static_cast<int32>(PlatformData->PixelFormat);
        OutTexture.AddressX = static_cast<uint8>(Texture->AddressX.GetValue());
        OutTexture.AddressY = static_cast<uint8>(Texture->AddressY.GetValue());
        OutTexture.Filter = static_cast<uint8>(Texture->Filter.GetValue());
        OutTexture.LODGroup = static_cast<uint8>(Texture->LODGroup.GetValue());
        OutTexture.bSRGB = Texture->SRGB;
        OutTexture.Mips.Reserve(PlatformData->Mips.Num());

        for (int32 MipIndex = 0; MipIndex < PlatformData->Mips.Num(); ++MipIndex)
        {
            FTexture2DMipMap& SourceMip = PlatformData->Mips[MipIndex];
            const int64 BulkSize = SourceMip.BulkData.GetBulkDataSize();
            const bool bBulkResident = SourceMip.BulkData.IsBulkDataLoaded();
            FGWorldBakedTextureMip MipLayout;
            MipLayout.SizeX = SourceMip.SizeX;
            MipLayout.SizeY = SourceMip.SizeY;
            // glTFRuntime's regular UTexture2D builder sets SizeX/SizeY but intentionally leaves
            // FTexture2DMipMap::SizeZ at its default value (0). SizeZ is populated only for
            // volume textures. The .gworld texture format is strictly 2D, so normalize the
            // source representation to a single slice instead of rejecting a valid runtime mip.
            MipLayout.SizeZ = 1;
            int64 ExpectedBytes = 0;

            if (SourceMip.SizeX <= 0 || SourceMip.SizeY <= 0 || SourceMip.SizeZ > 1)
            {
                OutError = FString::Printf(
                    TEXT("Texture mip %d has invalid 2D dimensions: %s (%dx%dx%d)"),
                    MipIndex, *GetNameSafe(Texture),
                    SourceMip.SizeX, SourceMip.SizeY, SourceMip.SizeZ);
                return false;
            }
            if (!GetExpectedMipBytes(OutTexture.PixelFormat, MipLayout, ExpectedBytes))
            {
                OutError = FString::Printf(
                    TEXT("Texture mip %d uses an unsupported/unsafe layout: %s Format=%d Size=%dx%dx%d"),
                    MipIndex, *GetNameSafe(Texture), OutTexture.PixelFormat,
                    SourceMip.SizeX, SourceMip.SizeY, SourceMip.SizeZ);
                return false;
            }
            if (BulkSize <= 0)
            {
                OutError = FString::Printf(
                    TEXT("Texture mip %d CPU payload is unavailable after glTFRuntime UpdateResource: %s ")
                    TEXT("Format=%d Size=%dx%dx%d Resident=%s. The build path captures transient ")
                    TEXT("BulkData directly with glTFRuntime texture streaming disabled."),
                    MipIndex, *GetNameSafe(Texture), OutTexture.PixelFormat,
                    SourceMip.SizeX, SourceMip.SizeY, SourceMip.SizeZ,
                    bBulkResident ? TEXT("true") : TEXT("false"));
                return false;
            }
            if (BulkSize > MaxTextureMipBytes || BulkSize > MAX_int32)
            {
                OutError = FString::Printf(
                    TEXT("Texture mip %d exceeds the archive safety limit: %s Bytes=%lld"),
                    MipIndex, *GetNameSafe(Texture), static_cast<long long>(BulkSize));
                return false;
            }
            if (BulkSize != ExpectedBytes)
            {
                OutError = FString::Printf(
                    TEXT("Texture mip %d byte count does not match its pixel format: %s ")
                    TEXT("Format=%d Size=%dx%dx%d Bytes=%lld Expected=%lld Resident=%s"),
                    MipIndex, *GetNameSafe(Texture), OutTexture.PixelFormat,
                    SourceMip.SizeX, SourceMip.SizeY, SourceMip.SizeZ,
                    static_cast<long long>(BulkSize), static_cast<long long>(ExpectedBytes),
                    bBulkResident ? TEXT("true") : TEXT("false"));
                return false;
            }

            // LockReadOnly may synchronously make a nonresident bulk payload available. Do not
            // reject solely on IsBulkDataLoaded(); the authoritative checks are payload size and
            // whether the lock returns a valid pointer.
            const void* SourceBytes = SourceMip.BulkData.LockReadOnly();
            if (!SourceBytes)
            {
                OutError = FString::Printf(
                    TEXT("Texture mip %d could not be locked for CPU capture: %s Bytes=%lld Resident=%s"),
                    MipIndex, *GetNameSafe(Texture), static_cast<long long>(BulkSize),
                    bBulkResident ? TEXT("true") : TEXT("false"));
                return false;
            }

            FGWorldBakedTextureMip& Mip = OutTexture.Mips.AddDefaulted_GetRef();
            Mip.SizeX = SourceMip.SizeX;
            Mip.SizeY = SourceMip.SizeY;
            Mip.SizeZ = 1;
            Mip.Bytes.SetNumUninitialized(static_cast<int32>(BulkSize));
            FMemory::Memcpy(Mip.Bytes.GetData(), SourceBytes, static_cast<SIZE_T>(BulkSize));
            SourceMip.BulkData.Unlock();
        }
        return true;
    }

    int32 FindOrCaptureTexture(
        UTexture2D* Texture,
        FGWorldBakedModel& InOutModel,
        TMap<TWeakObjectPtr<UTexture2D>, int32>& InOutTextureIds,
        FString& OutError)
    {
        if (const int32* Existing = InOutTextureIds.Find(Texture))
        {
            return *Existing;
        }
        if (InOutModel.Textures.Num() >= MaxTextures)
        {
            OutError = TEXT("The model exceeds the baked texture-count safety limit");
            return INDEX_NONE;
        }

        const int32 TextureId = InOutModel.Textures.Num();
        FGWorldBakedTexture Captured;
        if (!CaptureTexture(Texture, TextureId, Captured, OutError))
        {
            return INDEX_NONE;
        }
        InOutTextureIds.Add(Texture, TextureId);
        InOutModel.Textures.Add(MoveTemp(Captured));
        return TextureId;
    }

    int32 FindOrCaptureMaterial(
        UMaterialInterface* Material,
        const FString& SourceMaterialName,
        FGWorldBakedModel& InOutModel,
        TMap<TWeakObjectPtr<UMaterialInterface>, int32>& InOutMaterialIds,
        TMap<TWeakObjectPtr<UTexture2D>, int32>& InOutTextureIds,
        FString& OutError)
    {
        if (!IsValid(Material)) return INDEX_NONE;
        if (const int32* Existing = InOutMaterialIds.Find(Material))
        {
            return *Existing;
        }
        if (InOutModel.Materials.Num() >= MaxMaterials)
        {
            OutError = TEXT("The model exceeds the baked material-count safety limit");
            return INDEX_NONE;
        }

        UMaterial* BaseMaterial = Material->GetMaterial();
        if (!IsPersistentCookedAsset(BaseMaterial))
        {
            OutError = FString::Printf(
                TEXT("Runtime material has no persistent shader parent: %s"),
                *GetNameSafe(Material));
            return INDEX_NONE;
        }

        FGWorldBakedMaterial Captured;
        Captured.MaterialId = InOutModel.Materials.Num();
        // Preserve the authored glTF material name. A transient MID name is allocation-order
        // dependent and would make MaterialsOverrideByNameMap stop matching after baking.
        Captured.Name = SourceMaterialName.IsEmpty()
            ? Material->GetName()
            : SourceMaterialName;
        Captured.BaseMaterialPath = BaseMaterial->GetPathName();
        Captured.bUnlit = BaseMaterial->GetShadingModels().HasShadingModel(MSM_Unlit);
        const bool bTwoSided = BaseMaterial->IsTwoSided();
        const EBlendMode BlendMode = BaseMaterial->GetBlendMode();
        // Unlit versus uber and the blend family are independent. Keeping both prevents an unlit
        // masked/translucent material from being rebuilt with an opaque override shader.
        if (BlendMode == BLEND_Masked)
        {
            Captured.MaterialType = static_cast<uint8>(bTwoSided
                ? EglTFRuntimeMaterialType::TwoSidedMasked
                : EglTFRuntimeMaterialType::Masked);
        }
        else if (IsTranslucentBlendMode(BlendMode))
        {
            Captured.MaterialType = static_cast<uint8>(bTwoSided
                ? EglTFRuntimeMaterialType::TwoSidedTranslucent
                : EglTFRuntimeMaterialType::Translucent);
        }
        else
        {
            Captured.MaterialType = static_cast<uint8>(bTwoSided
                ? EglTFRuntimeMaterialType::TwoSided
                : EglTFRuntimeMaterialType::Opaque);
        }

        TArray<FMaterialParameterInfo> ParameterInfos;
        TArray<FGuid> ParameterIds;
        Material->GetAllScalarParameterInfo(ParameterInfos, ParameterIds);
        ParameterInfos.Sort([](const FMaterialParameterInfo& A, const FMaterialParameterInfo& B)
        {
            return A.Name.LexicalLess(B.Name);
        });
        for (const FMaterialParameterInfo& Info : ParameterInfos)
        {
            float Value = 0.0f;
            if (Material->GetScalarParameterValue(Info, Value) && FMath::IsFinite(Value))
            {
                FGWorldBakedScalarParameter& Parameter = Captured.Scalars.AddDefaulted_GetRef();
                Parameter.Name = Info.Name.ToString();
                Parameter.Value = Value;
            }
        }

        ParameterInfos.Reset();
        ParameterIds.Reset();
        Material->GetAllVectorParameterInfo(ParameterInfos, ParameterIds);
        ParameterInfos.Sort([](const FMaterialParameterInfo& A, const FMaterialParameterInfo& B)
        {
            return A.Name.LexicalLess(B.Name);
        });
        for (const FMaterialParameterInfo& Info : ParameterInfos)
        {
            FLinearColor Value;
            if (Material->GetVectorParameterValue(Info, Value)
                && FMath::IsFinite(Value.R) && FMath::IsFinite(Value.G)
                && FMath::IsFinite(Value.B) && FMath::IsFinite(Value.A))
            {
                FGWorldBakedVectorParameter& Parameter = Captured.Vectors.AddDefaulted_GetRef();
                Parameter.Name = Info.Name.ToString();
                Parameter.Value = Value;
            }
        }

        ParameterInfos.Reset();
        ParameterIds.Reset();
        Material->GetAllTextureParameterInfo(ParameterInfos, ParameterIds);
        ParameterInfos.Sort([](const FMaterialParameterInfo& A, const FMaterialParameterInfo& B)
        {
            return A.Name.LexicalLess(B.Name);
        });
        for (const FMaterialParameterInfo& Info : ParameterInfos)
        {
            UTexture* Texture = nullptr;
            if (!Material->GetTextureParameterValue(Info, Texture) || !IsValid(Texture))
            {
                continue;
            }

            FGWorldBakedTextureParameter& Parameter = Captured.Textures.AddDefaulted_GetRef();
            Parameter.Name = Info.Name.ToString();
            if (IsPersistentCookedAsset(Texture))
            {
                // Engine/project defaults already ship as cooked assets. Referencing their stable
                // object path avoids duplicating them into every model archive.
                Parameter.AssetPath = Texture->GetPathName();
                continue;
            }

            UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
            if (!Texture2D)
            {
                OutError = FString::Printf(
                    TEXT("Unsupported transient texture type on material %s parameter %s"),
                    *GetNameSafe(Material), *Parameter.Name);
                return INDEX_NONE;
            }
            Parameter.TextureId = FindOrCaptureTexture(
                Texture2D, InOutModel, InOutTextureIds, OutError);
            if (Parameter.TextureId == INDEX_NONE)
            {
                return INDEX_NONE;
            }
        }

        const int32 MaterialId = Captured.MaterialId;
        InOutMaterialIds.Add(Material, MaterialId);
        InOutModel.Materials.Add(MoveTemp(Captured));
        return MaterialId;
    }
}

void FGWorldBakedModel::Reset()
{
    Meshes.Reset();
    Skins.Reset();
    Materials.Reset();
    Textures.Reset();
}

void FGWorldBakedAssetBundle::Reset()
{
    Meshes.Reset();
    Skins.Reset();
    Materials.Reset();
    Textures.Reset();
}

bool FGWorldBakedModel::IsSane(FString* OutError) const
{
    using namespace WorldBakedDataPrivate;
    auto Fail = [OutError](const TCHAR* Message)
    {
        if (OutError) *OutError = Message;
        return false;
    };
    if (Meshes.Num() > MaxMeshes || Materials.Num() > MaxMaterials
        || Textures.Num() > MaxTextures || Skins.Num() > MaxBones)
    {
        return Fail(TEXT("Baked model collection count exceeds a safety limit"));
    }

    // mesh.dat is also validated independently before its dependency members are read. In that
    // narrow case material ids cannot yet be range-checked; the bundle reader validates them while
    // following the manifest. Complete build inputs always carry their material/texture arrays.
    const bool bStandaloneMeshMember = Meshes.Num() == 1 && Skins.IsEmpty()
        && Materials.IsEmpty() && Textures.IsEmpty();

    TSet<int32> MeshIds;
    for (const FGWorldBakedMesh& Mesh : Meshes)
    {
        if (Mesh.MeshIndex < 0 || MeshIds.Contains(Mesh.MeshIndex)
            || Mesh.Primitives.IsEmpty() || Mesh.Primitives.Num() > MaxPrimitivesPerMesh
            || Mesh.AdditionalTransforms.Num() > MaxAdditionalTransforms)
        {
            return Fail(TEXT("Baked mesh index or primitive count is invalid"));
        }
        MeshIds.Add(Mesh.MeshIndex);
        for (const FTransform& Transform : Mesh.AdditionalTransforms)
        {
            if (!IsFiniteTransform(Transform)) return Fail(TEXT("Baked mesh transform is invalid"));
        }
        for (const FGWorldBakedPrimitive& Primitive : Mesh.Primitives)
        {
            const int32 VertexCount = Primitive.Positions.Num();
            if (VertexCount <= 0 || VertexCount > MaxVerticesPerPrimitive
                || Primitive.Indices.Num() > MaxIndicesPerPrimitive
                || (Primitive.bHasIndices && Primitive.Indices.IsEmpty())
                || Primitive.UVs.Num() > MaxUvChannels || Primitive.Joints.Num() > MaxJointSets
                || Primitive.Weights.Num() > MaxJointSets
                || Primitive.Joints.Num() != Primitive.Weights.Num()
                || Primitive.MorphTargets.Num() > MaxMorphTargets
                || Primitive.Mode < 0 || Primitive.Mode > 6
                || !AllFinite(Primitive.Positions) || !AllFinite(Primitive.Normals)
                || !AllFinite(Primitive.Tangents) || !AllFinite(Primitive.Colors))
            {
                return Fail(TEXT("Baked primitive array is invalid or exceeds a safety limit"));
            }
            // Non-indexed glTF primitives are valid. glTFRuntime records that distinction in
            // bHasIndices and its finalizer generates the native draw order from the vertex stream.
            // If a decoded index stream is present, validate it regardless of the source flag.
            for (const uint32 Index : Primitive.Indices)
            {
                if (Index >= static_cast<uint32>(VertexCount))
                    return Fail(TEXT("Baked primitive contains an out-of-range index"));
            }
            if ((!Primitive.Normals.IsEmpty() && Primitive.Normals.Num() != VertexCount)
                || (!Primitive.Tangents.IsEmpty() && Primitive.Tangents.Num() != VertexCount)
                || (!Primitive.Colors.IsEmpty() && Primitive.Colors.Num() != VertexCount))
            {
                return Fail(TEXT("Baked primitive vertex streams have different lengths"));
            }
            for (const TArray<FVector2f>& Channel : Primitive.UVs)
            {
                if (Channel.Num() != VertexCount)
                    return Fail(TEXT("Baked primitive UV stream length is invalid"));
                for (const FVector2f& UV : Channel)
                    if (!IsFiniteVector2f(UV)) return Fail(TEXT("Baked primitive UV contains NaN/Inf"));
            }
            for (int32 SetIndex = 0; SetIndex < Primitive.Joints.Num(); ++SetIndex)
            {
                if (Primitive.Joints[SetIndex].Num() != VertexCount
                    || Primitive.Weights[SetIndex].Num() != VertexCount
                    || !AllFinite(Primitive.Weights[SetIndex]))
                {
                    return Fail(TEXT("Baked primitive skin streams are invalid"));
                }
            }
            for (const FGWorldBakedMorphTarget& Morph : Primitive.MorphTargets)
            {
                if ((!Morph.Positions.IsEmpty() && Morph.Positions.Num() != VertexCount)
                    || (!Morph.Normals.IsEmpty() && Morph.Normals.Num() != VertexCount)
                    || !AllFinite(Morph.Positions) || !AllFinite(Morph.Normals))
                {
                    return Fail(TEXT("Baked morph-target streams are invalid"));
                }
            }
            for (const TPair<int32, FName>& Bone : Primitive.BoneMap)
            {
                if (Bone.Key < 0 || Bone.Value.IsNone())
                    return Fail(TEXT("Baked primitive bone map is invalid"));
            }
            for (const TPair<FString, TArray<float>>& WeightMap : Primitive.WeightMaps)
            {
                if (WeightMap.Key.IsEmpty() || WeightMap.Value.Num() != VertexCount)
                    return Fail(TEXT("Baked primitive weight map length is invalid"));
                for (const float Value : WeightMap.Value)
                    if (!FMath::IsFinite(Value)) return Fail(TEXT("Baked weight map contains NaN/Inf"));
            }
            if (Primitive.MaterialId < INDEX_NONE
                || (!bStandaloneMeshMember && Primitive.MaterialId != INDEX_NONE
                    && !Materials.IsValidIndex(Primitive.MaterialId))
                || (Primitive.bHasMaterial && Primitive.MaterialId == INDEX_NONE))
            {
                return Fail(TEXT("Baked primitive material reference is invalid"));
            }
        }
    }

    for (int32 Index = 0; Index < Meshes.Num(); ++Index)
    {
        if (!MeshIds.Contains(Index))
            return Fail(TEXT("Baked mesh indices are not contiguous"));
    }

    for (int32 Index = 0; Index < Materials.Num(); ++Index)
    {
        const FGWorldBakedMaterial& Material = Materials[Index];
        if (Material.MaterialId != Index || Material.BaseMaterialPath.IsEmpty()
            || Material.MaterialType > static_cast<uint8>(EglTFRuntimeMaterialType::TwoSidedMasked)
            || Material.Scalars.Num() > MaxMaterialParameters
            || Material.Vectors.Num() > MaxMaterialParameters
            || Material.Textures.Num() > MaxMaterialParameters)
            return Fail(TEXT("Baked material index or shader path is invalid"));
        for (const FGWorldBakedScalarParameter& Parameter : Material.Scalars)
            if (Parameter.Name.IsEmpty() || !FMath::IsFinite(Parameter.Value))
                return Fail(TEXT("Baked scalar material parameter is invalid"));
        for (const FGWorldBakedVectorParameter& Parameter : Material.Vectors)
            if (Parameter.Name.IsEmpty() || !IsFiniteColor(Parameter.Value))
                return Fail(TEXT("Baked vector material parameter is invalid"));
        for (const FGWorldBakedTextureParameter& Parameter : Material.Textures)
        {
            const bool bHasBakedTexture = Parameter.TextureId != INDEX_NONE;
            const bool bHasAssetTexture = !Parameter.AssetPath.IsEmpty();
            if (Parameter.Name.IsEmpty() || Parameter.TextureId < INDEX_NONE
                || bHasBakedTexture == bHasAssetTexture
                || (bHasBakedTexture && !Textures.IsValidIndex(Parameter.TextureId)))
            {
                return Fail(TEXT("Baked texture material parameter is invalid"));
            }
        }
    }
    for (int32 Index = 0; Index < Textures.Num(); ++Index)
    {
        const FGWorldBakedTexture& Texture = Textures[Index];
        if (Texture.TextureId != Index || Texture.SizeX <= 0 || Texture.SizeY <= 0
            || Texture.PixelFormat <= PF_Unknown || Texture.PixelFormat >= PF_MAX
            || Texture.AddressX >= static_cast<uint8>(TA_MAX)
            || Texture.AddressY >= static_cast<uint8>(TA_MAX)
            || Texture.Filter >= static_cast<uint8>(TF_MAX)
            || Texture.LODGroup >= static_cast<uint8>(TEXTUREGROUP_MAX)
            || Texture.Mips.IsEmpty() || Texture.Mips.Num() > MaxTextureMips)
        {
            return Fail(TEXT("Baked texture metadata is invalid"));
        }
        int32 ExpectedSizeX = Texture.SizeX;
        int32 ExpectedSizeY = Texture.SizeY;
        for (const FGWorldBakedTextureMip& Mip : Texture.Mips)
        {
            int64 ExpectedBytes = 0;
            if (Mip.SizeX != ExpectedSizeX || Mip.SizeY != ExpectedSizeY
                || !GetExpectedMipBytes(Texture.PixelFormat, Mip, ExpectedBytes)
                || Mip.Bytes.Num() != ExpectedBytes)
            {
                return Fail(TEXT("Baked texture mip layout or byte count is invalid"));
            }
            ExpectedSizeX = FMath::Max(1, ExpectedSizeX >> 1);
            ExpectedSizeY = FMath::Max(1, ExpectedSizeY >> 1);
        }
    }
    TSet<int32> SkinIds;
    for (const FGWorldBakedSkin& Skin : Skins)
    {
        if (Skin.SkinIndex < 0 || SkinIds.Contains(Skin.SkinIndex)
            || Skin.Bones.IsEmpty() || Skin.Bones.Num() > MaxBones)
            return Fail(TEXT("Baked skin is invalid"));
        SkinIds.Add(Skin.SkinIndex);
        TSet<FName> BoneNames;
        for (int32 BoneIndex = 0; BoneIndex < Skin.Bones.Num(); ++BoneIndex)
        {
            const FGWorldBakedBone& Bone = Skin.Bones[BoneIndex];
            if (Bone.Name.IsEmpty() || Bone.ParentIndex >= BoneIndex
                || Bone.ParentIndex < INDEX_NONE || !IsFiniteTransform(Bone.Transform)
                || BoneNames.Contains(FName(*Bone.Name)))
            {
                return Fail(TEXT("Baked skeleton hierarchy is invalid"));
            }
            BoneNames.Add(FName(*Bone.Name));
        }
        for (const TPair<int32, FName>& Joint : Skin.JointBoneMap)
        {
            if (Joint.Key < 0 || Joint.Value.IsNone() || !BoneNames.Contains(Joint.Value))
                return Fail(TEXT("Baked skin joint-to-bone map is invalid"));
        }
    }
    if (OutError) OutError->Reset();
    return true;
}

bool FGWorldBakedDataCapture::CaptureMesh(
    const int32 MeshIndex,
    const FString& MeshName,
    const FglTFRuntimeMeshLOD& RuntimeLOD,
    FGWorldBakedModel& InOutModel,
    TMap<TWeakObjectPtr<UMaterialInterface>, int32>& InOutMaterialIds,
    TMap<TWeakObjectPtr<UTexture2D>, int32>& InOutTextureIds,
    FString& OutError)
{
    using namespace WorldBakedDataPrivate;
    check(IsInGameThread());
    OutError.Reset();
    if (MeshIndex < 0 || RuntimeLOD.Primitives.IsEmpty()
        || RuntimeLOD.Primitives.Num() > MaxPrimitivesPerMesh)
    {
        OutError = TEXT("glTFRuntime returned an empty or invalid decoded mesh");
        return false;
    }
    for (const FGWorldBakedMesh& Existing : InOutModel.Meshes)
    {
        if (Existing.MeshIndex == MeshIndex)
        {
            OutError = TEXT("The same source mesh was captured twice");
            return false;
        }
    }

    FGWorldBakedMesh Mesh;
    Mesh.MeshIndex = MeshIndex;
    Mesh.Name = MeshName;
    Mesh.bHasNormals = RuntimeLOD.bHasNormals;
    Mesh.bHasTangents = RuntimeLOD.bHasTangents;
    Mesh.bHasUV = RuntimeLOD.bHasUV;
    Mesh.bHasVertexColors = RuntimeLOD.bHasVertexColors;
    Mesh.AdditionalTransforms = RuntimeLOD.AdditionalTransforms;
    Mesh.Primitives.Reserve(RuntimeLOD.Primitives.Num());

    for (const FglTFRuntimePrimitive& Source : RuntimeLOD.Primitives)
    {
        FGWorldBakedPrimitive& Target = Mesh.Primitives.AddDefaulted_GetRef();
        Target.Positions.Reserve(Source.Positions.Num());
        for (const FVector& Value : Source.Positions) Target.Positions.Add(FVector3f(Value));
        Target.Normals.Reserve(Source.Normals.Num());
        for (const FVector& Value : Source.Normals) Target.Normals.Add(FVector3f(Value));
        Target.Tangents.Reserve(Source.Tangents.Num());
        for (const FVector4& Value : Source.Tangents) Target.Tangents.Add(FVector4f(Value));
        Target.UVs.SetNum(Source.UVs.Num());
        for (int32 Channel = 0; Channel < Source.UVs.Num(); ++Channel)
        {
            Target.UVs[Channel].Reserve(Source.UVs[Channel].Num());
            for (const FVector2D& Value : Source.UVs[Channel])
                Target.UVs[Channel].Add(FVector2f(Value));
        }
        SanitizeTriangleIndices(Source.Positions, Source.Mode, Source.Indices, Target.Indices);
        if (Target.Indices.Num() != Source.Indices.Num())
        {
            UE_LOG(LogTemp, VeryVerbose,
                TEXT("Removed %d degenerate/invalid triangle indices while baking mesh %d (%s)."),
                Source.Indices.Num() - Target.Indices.Num(), MeshIndex, *MeshName);
        }
        Target.Joints.SetNum(Source.Joints.Num());
        for (int32 SetIndex = 0; SetIndex < Source.Joints.Num(); ++SetIndex)
        {
            Target.Joints[SetIndex].Reserve(Source.Joints[SetIndex].Num());
            for (const FglTFRuntimeUInt16Vector4& Value : Source.Joints[SetIndex])
            {
                FGWorldBakedJoint4& Joint = Target.Joints[SetIndex].AddDefaulted_GetRef();
                Joint.X = Value.X; Joint.Y = Value.Y; Joint.Z = Value.Z; Joint.W = Value.W;
            }
        }
        Target.Weights.SetNum(Source.Weights.Num());
        for (int32 SetIndex = 0; SetIndex < Source.Weights.Num(); ++SetIndex)
        {
            Target.Weights[SetIndex].Reserve(Source.Weights[SetIndex].Num());
            for (const FVector4& Value : Source.Weights[SetIndex])
                Target.Weights[SetIndex].Add(FVector4f(Value));
        }
        Target.Colors.Reserve(Source.Colors.Num());
        for (const FVector4& Value : Source.Colors) Target.Colors.Add(FVector4f(Value));
        Target.MorphTargets.Reserve(Source.MorphTargets.Num());
        for (const FglTFRuntimeMorphTarget& SourceMorph : Source.MorphTargets)
        {
            FGWorldBakedMorphTarget& TargetMorph = Target.MorphTargets.AddDefaulted_GetRef();
            TargetMorph.Name = SourceMorph.Name;
            TargetMorph.Positions.Reserve(SourceMorph.Positions.Num());
            for (const FVector& Value : SourceMorph.Positions)
                TargetMorph.Positions.Add(FVector3f(Value));
            TargetMorph.Normals.Reserve(SourceMorph.Normals.Num());
            for (const FVector& Value : SourceMorph.Normals)
                TargetMorph.Normals.Add(FVector3f(Value));
        }
        Target.BoneMap = Source.OverrideBoneMap;
        Target.WeightMaps = Source.WeightMaps;
        Target.MaterialName = Source.MaterialName;
        Target.Mode = Source.Mode;
        Target.bHasMaterial = Source.bHasMaterial;
        Target.bHighPrecisionUVs = Source.bHighPrecisionUVs;
        Target.bHighPrecisionWeights = Source.bHighPrecisionWeights;
        Target.bDisableShadows = Source.bDisableShadows;
        Target.bHasIndices = Source.bHasIndices;
        // A material pointer on a material-less primitive may be glTFRuntime's temporary default.
        // Do not serialize it: dependency streaming must follow only an authored material edge.
        if (Source.bHasMaterial)
        {
            Target.MaterialId = FindOrCaptureMaterial(
                Source.Material,
                Source.MaterialName,
                InOutModel,
                InOutMaterialIds,
                InOutTextureIds,
                OutError);
            if (Target.MaterialId == INDEX_NONE)
            {
                if (OutError.IsEmpty())
                {
                    OutError = TEXT("glTFRuntime returned an invalid material for a material-bearing primitive");
                }
                return false;
            }
        }
    }

    InOutModel.Meshes.Add(MoveTemp(Mesh));
    return true;
}

bool FGWorldBakedDataCapture::CaptureSkin(
    UglTFRuntimeAsset* SourceAsset,
    const int32 SkinIndex,
    const TSet<int32>& MeshIndicesUsingSkin,
    const TMap<FString, FString>& BoneAliases,
    FGWorldBakedModel& InOutModel,
    FString& OutError)
{
    using namespace WorldBakedDataPrivate;
    check(IsInGameThread());
    OutError.Reset();
    if (!IsValid(SourceAsset) || SkinIndex < 0)
    {
        OutError = TEXT("Cannot capture an invalid source skin");
        return false;
    }
    for (const FGWorldBakedSkin& Existing : InOutModel.Skins)
    {
        if (Existing.SkinIndex == SkinIndex) return true;
    }

    if (!BoneAliases.IsEmpty())
    {
        FString AliasError;
        if (!CharacterBoneSchema::ValidateSourceToCanonicalMap(BoneAliases, AliasError))
        {
            OutError = FString::Printf(TEXT("Character bone map rejected before skin capture: %s"), *AliasError);
            return false;
        }
    }

    FglTFRuntimeSkeletonConfig Config;
    Config.CacheMode = EglTFRuntimeCacheMode::None;
    Config.BonesNameMap = BoneAliases;
    Config.RootBoneName = TEXT("Root");
    Config.bAddRootNodeIfMissing = true;
    USkeleton* Skeleton = SourceAsset->LoadSkeleton(SkinIndex, Config);
    if (!IsValid(Skeleton))
    {
        OutError = FString::Printf(TEXT("glTFRuntime could not decode skin %d"), SkinIndex);
        return false;
    }

    const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();
    if (ReferenceSkeleton.GetNum() <= 0 || ReferenceSkeleton.GetNum() > MaxBones)
    {
        OutError = TEXT("Decoded skeleton is empty or exceeds the bone safety limit");
        return false;
    }
    if (!BoneAliases.IsEmpty()
        && !CharacterBoneSchema::ValidateCanonicalReferenceSkeleton(ReferenceSkeleton, OutError))
    {
        OutError = FString::Printf(TEXT("Canonical character skeleton validation failed: %s"), *OutError);
        return false;
    }

    FGWorldBakedSkin Skin;
    Skin.SkinIndex = SkinIndex;
    Skin.Bones.Reserve(ReferenceSkeleton.GetNum());
    for (int32 BoneIndex = 0; BoneIndex < ReferenceSkeleton.GetNum(); ++BoneIndex)
    {
        FGWorldBakedBone& Bone = Skin.Bones.AddDefaulted_GetRef();
        Bone.Name = ReferenceSkeleton.GetBoneName(BoneIndex).ToString();
        Bone.ParentIndex = ReferenceSkeleton.GetParentIndex(BoneIndex);
        Bone.Transform = ReferenceSkeleton.GetRefBonePose()[BoneIndex];
    }

    // Only joint indices actually referenced by decoded vertices are materialized. This avoids
    // guessing a source skin's joint count and rejects a missing name before publishing the build.
    TSet<int32> ReferencedJoints;
    for (const FGWorldBakedMesh& Mesh : InOutModel.Meshes)
    {
        if (!MeshIndicesUsingSkin.Contains(Mesh.MeshIndex))
        {
            continue;
        }
        for (const FGWorldBakedPrimitive& Primitive : Mesh.Primitives)
        {
            for (const TArray<FGWorldBakedJoint4>& JointSet : Primitive.Joints)
            {
                for (const FGWorldBakedJoint4& Joint : JointSet)
                {
                    ReferencedJoints.Add(Joint.X); ReferencedJoints.Add(Joint.Y);
                    ReferencedJoints.Add(Joint.Z); ReferencedJoints.Add(Joint.W);
                }
            }
        }
    }
    for (const int32 JointIndex : ReferencedJoints)
    {
        FString BoneName = SourceAsset->GetSkinJointNameFromJointIndex(SkinIndex, JointIndex);
        if (const FString* Alias = BoneAliases.Find(BoneName)) BoneName = *Alias;
        if (BoneName.IsEmpty())
        {
            OutError = FString::Printf(
                TEXT("Skin %d has no name for referenced joint %d"), SkinIndex, JointIndex);
            return false;
        }
        Skin.JointBoneMap.Add(JointIndex, FName(*BoneName));
    }

    InOutModel.Skins.Add(MoveTemp(Skin));
    return true;
}
