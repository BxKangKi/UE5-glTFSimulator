// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldBakedModelAsset.h
 * 역할: baked 모델을 Unreal 런타임 에셋으로 제공합니다.
 * 핵심 기능: dat 범위 로드, 메시·텍스처·머티리얼 재구성, 요청별 GC 보호.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "glTFRuntimeAsset.h"
#include "System/WorldArchive.h"
#include "WorldBakedModelAsset.generated.h"

struct FResolvedRuntimeModel;
struct FWorldBakedBuildReferenceGuard;

/**
 * Runtime facade over one baked model.
 *
 * It deliberately owns no GLB bytes and never calls LoadFromData/LoadFromFilename. The small node
 * and range tables stay resident; decoded mesh/material/texture .dat members are loaded only for a
 * requested mesh and discarded after Unreal's render objects have copied/retained their data.
 */
UCLASS(Transient)
class GLTFSIMULATOR_API UWorldBakedModelAsset final : public UObject
{
    GENERATED_BODY()

public:
    bool Initialize(const FResolvedRuntimeModel& Model, FString& OutError);

    const TArray<FglTFRuntimeNode>& GetNodes() const { return Nodes; }
    int32 GetNumMeshes() const { return Manifest.IsValid() ? Manifest->MeshRanges.Num() : 0; }
    FString GetMeshName(int32 MeshIndex) const;
    bool HasSkin(int32 SkinIndex) const
    {
        return Manifest.IsValid() && Manifest->SkinRanges.Contains(SkinIndex);
    }

    UStaticMesh* LoadStaticMesh(
        int32 MeshIndex,
        const FglTFRuntimeStaticMeshConfig& Config,
        FString* OutError = nullptr);
    UStaticMesh* LoadStaticMeshLODs(
        const TArray<int32>& MeshIndices,
        const FglTFRuntimeStaticMeshConfig& Config,
        FString* OutError = nullptr);
    void LoadStaticMeshAsync(
        int32 MeshIndex,
        const FglTFRuntimeStaticMeshAsync& Callback,
        const FglTFRuntimeStaticMeshConfig& Config);
    void LoadStaticMeshLODsAsync(
        const TArray<int32>& MeshIndices,
        const FglTFRuntimeStaticMeshAsync& Callback,
        const FglTFRuntimeStaticMeshConfig& Config);

    USkeletalMesh* LoadSkeletalMesh(
        int32 MeshIndex,
        int32 SkinIndex,
        const FglTFRuntimeSkeletalMeshConfig& Config,
        FString* OutError = nullptr);
    void LoadSkeletalMeshAsync(
        int32 MeshIndex,
        int32 SkinIndex,
        const FglTFRuntimeSkeletalMeshAsync& Callback,
        const FglTFRuntimeSkeletalMeshConfig& Config);

private:
    FGuid UUID;
    FString Reference;
    TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Reader;
    /** Shared because every async range request must retain the table without copying all rows. */
    TSharedPtr<FGWorldModelManifest, ESPMode::ThreadSafe> Manifest;

    /** Reflected storage keeps the node view attached to this facade for Blueprint/native users. */
    UPROPERTY(Transient)
    TArray<FglTFRuntimeNode> Nodes;

    /**
     * Does not keep textures alive. Loaded meshes/materials retain the textures they still use;
     * a later mesh request reuses such a texture, while GC remains free to reclaim unloaded data.
     */
    // Key packs baked texture id + active max-resolution setting so changing the texture cap
    // cannot accidentally reuse a larger runtime texture from an earlier streaming request.
    TMap<uint64, TWeakObjectPtr<UTexture2D>> WeakTextureCache;

    bool BuildRuntimeLODs(
        const FGWorldBakedAssetBundle& Bundle,
        const FglTFRuntimeMaterialsConfig& MaterialsConfig,
        int32 SkinIndex,
        FWorldBakedBuildReferenceGuard& ReferenceGuard,
        TArray<FglTFRuntimeMeshLOD>& OutLODs,
        FString& OutError);
    UglTFRuntimeAsset* CreateMeshBuilder(FString& OutError);
    UTexture2D* CreateTexture(
        const FGWorldBakedTexture& Baked,
        FWorldBakedBuildReferenceGuard& ReferenceGuard,
        FString& OutError);
    UMaterialInterface* CreateMaterial(
        const FGWorldBakedMaterial& Baked,
        const TMap<int32, UTexture2D*>& Textures,
        const FglTFRuntimeMaterialsConfig& Config,
        FWorldBakedBuildReferenceGuard& ReferenceGuard,
        FString& OutError);
};
