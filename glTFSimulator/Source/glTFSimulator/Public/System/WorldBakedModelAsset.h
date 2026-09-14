// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldBakedModelAsset.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "glTFRuntimeAsset.h"
#include "System/WorldArchive.h"
#include "WorldBakedModelAsset.generated.h"

struct FResolvedRuntimeModel;
struct FWorldBakedBuildReferenceGuard;
class UWorldBakedModelAsset;

using FGWorldBakedStaticMeshNativeCallback = TFunction<void(UStaticMesh*)>;
using FGWorldBakedSkeletalMeshNativeCallback = TFunction<void(USkeletalMesh*)>;

/**
 * One independent glTFRuntime finalization request. Keeping callback state per request allows a
 * single baked-model facade to finalize several already-decoded meshes concurrently instead of
 * serializing every mesh behind one ActiveAsyncTicket.
 */
UCLASS(Transient)
class GLTFSIMULATOR_API UWorldBakedMeshFinalizeRequest final : public UObject
{
    GENERATED_BODY()

public:
    void InitializeStatic(
        UWorldBakedModelAsset* InOwner,
        UglTFRuntimeAsset* InBuilder,
        FString InSourceReference,
        FGWorldBakedStaticMeshNativeCallback InCallback);
    void InitializeSkeletal(
        UWorldBakedModelAsset* InOwner,
        UglTFRuntimeAsset* InBuilder,
        FString InSourceReference,
        FGWorldBakedSkeletalMeshNativeCallback InCallback);
    void BeginStatic(
        uint64 InTicket,
        TArray<FglTFRuntimeMeshLOD>&& LODs,
        const FglTFRuntimeStaticMeshConfig& Config);
    void BeginSkeletal(
        uint64 InTicket,
        TArray<FglTFRuntimeMeshLOD>&& LODs,
        const FglTFRuntimeSkeletalMeshConfig& Config);
    void RejectBeforeStart(const FString& Reason);

private:
    UPROPERTY(Transient)
    TObjectPtr<UWorldBakedModelAsset> Owner = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UglTFRuntimeAsset> Builder = nullptr;

    uint64 Ticket = 0;
    FString SourceReference;
    FGWorldBakedStaticMeshNativeCallback StaticCallback;
    FGWorldBakedSkeletalMeshNativeCallback SkeletalCallback;
    FDelegateHandle StaticRayTracingHandle;

    void FailStarted(const FString& Reason);
    void Cleanup();

    UFUNCTION()
    void HandleStaticMeshFinalized(UStaticMesh* StaticMesh);

    UFUNCTION()
    void HandleSkeletalMeshFinalized(USkeletalMesh* SkeletalMesh);
};

USTRUCT()
struct FGWorldBakedAsyncBuildReferences
{
    GENERATED_BODY()

    UPROPERTY(Transient)
    TObjectPtr<UglTFRuntimeAsset> Builder = nullptr;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UObject>> References;
};

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
    /** Native async overloads allow callers to retain request context such as mesh index. */
    void LoadStaticMeshAsyncNative(
        int32 MeshIndex,
        FGWorldBakedStaticMeshNativeCallback Callback,
        const FglTFRuntimeStaticMeshConfig& Config);
    void LoadStaticMeshLODsAsyncNative(
        const TArray<int32>& MeshIndices,
        FGWorldBakedStaticMeshNativeCallback Callback,
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
    void LoadSkeletalMeshAsyncNative(
        int32 MeshIndex,
        int32 SkinIndex,
        FGWorldBakedSkeletalMeshNativeCallback Callback,
        const FglTFRuntimeSkeletalMeshConfig& Config);

private:
    friend class UWorldBakedMeshFinalizeRequest;

    FGuid UUID;
    FString Reference;
    TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Reader;
    /** Shared because every async range request must retain the table without copying all rows. */
    TSharedPtr<FGWorldModelManifest, ESPMode::ThreadSafe> Manifest;

    /** Reflected storage keeps the node view attached to this facade for Blueprint/native users. */
    UPROPERTY(Transient)
    TArray<FglTFRuntimeNode> Nodes;

    /**
     * Runtime texture cache for this baked-model facade. The weak map provides O(1) lookup while
     * a bounded reflected keep-alive set makes it safe for worker reads to omit recently used
     * texture.dat members that have already been reconstructed on the game thread. The strong set
     * is capped by count and decoded byte size so world streaming cannot retain every visited texture.
     */
    // Key packs baked texture id + active max-resolution setting so changing the texture cap
    // cannot accidentally reuse a larger runtime texture from an earlier streaming request.
    TMap<uint64, TWeakObjectPtr<UTexture2D>> WeakTextureCache;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UTexture2D>> TextureCacheKeepAlive;
    TArray<uint64> TextureCacheKeepAliveKeys;
    TArray<int64> TextureCacheKeepAliveBytes;
    int64 TextureCacheKeepAliveTotalBytes = 0;

    /**
     * Reuses immutable runtime MIDs across mesh groups of the same baked model. The cache key
     * includes the material-config signature, so a changed override/material setup never reuses an
     * incompatible instance. A bounded reflected set lets worker bundle reads skip material.dat
     * and all of its texture dependencies while the material is guaranteed resident.
     */
    TMap<uint64, TWeakObjectPtr<UMaterialInterface>> WeakMaterialCache;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UMaterialInterface>> MaterialCacheKeepAlive;
    TArray<uint64> MaterialCacheKeepAliveKeys;

    /**
     * Pending/active decoded-data finalizers. Archive I/O and RuntimeLOD conversion are completed
     * before these entries reach glTFRuntime; only UObject resource attachment and the plugin's
     * asynchronous finalizer cross the game thread. Strong reflected references keep transient
     * MIDs/textures/builders alive while the bounded native queue is waiting.
     */
    UPROPERTY(Transient)
    TArray<FGWorldBakedAsyncBuildReferences> AsyncBuildReferences;

    /** Independent request UObjects remove the former one-finalizer-per-facade bottleneck. */
    UPROPERTY(Transient)
    TArray<TObjectPtr<UWorldBakedMeshFinalizeRequest>> ActiveFinalizeRequests;

    /**
     * Weak completed-mesh cache plus in-flight coalescing for render-only world geometry. Static
     * actors use this facade as Outer when collision/CPU access are disabled, so identical LOD
     * requests across chunks build once and all waiters receive the same immutable UStaticMesh.
     * Weak storage preserves normal streaming GC when no component references the mesh anymore.
     */
    TMap<FString, TWeakObjectPtr<UStaticMesh>> SharedRenderMeshCache;
    TMap<FString, TArray<FGWorldBakedStaticMeshNativeCallback>> PendingSharedRenderMeshBuilds;

    bool TryBeginSharedRenderMeshRequest(
        const TArray<int32>& MeshIndices,
        const FglTFRuntimeStaticMeshConfig& Config,
        FGWorldBakedStaticMeshNativeCallback& InOutCallback,
        FString& OutCacheKey);
    void CompleteSharedRenderMeshRequest(const FString& CacheKey, UStaticMesh* Mesh);

    bool BuildRuntimeLODs(
        const FGWorldBakedAssetBundle& Bundle,
        const FglTFRuntimeMaterialsConfig& MaterialsConfig,
        int32 SkinIndex,
        FWorldBakedBuildReferenceGuard& ReferenceGuard,
        TArray<FglTFRuntimeMeshLOD>& OutLODs,
        FString& OutError);
    bool AttachRuntimeMaterials(
        const FGWorldBakedAssetBundle& Bundle,
        const FglTFRuntimeMaterialsConfig& MaterialsConfig,
        const TArray<TArray<int32>>& MaterialIds,
        FWorldBakedBuildReferenceGuard& ReferenceGuard,
        TArray<FglTFRuntimeMeshLOD>& InOutLODs,
        FString& OutError);
    void QueueStaticRuntimeLODFinalizer(
        TArray<FglTFRuntimeMeshLOD>&& LODs,
        const FglTFRuntimeStaticMeshConfig& Config,
        FGWorldBakedStaticMeshNativeCallback Callback,
        const FString& SourceReference,
        const FWorldBakedBuildReferenceGuard& ReferenceGuard);
    void QueueSkeletalRuntimeLODFinalizer(
        TArray<FglTFRuntimeMeshLOD>&& LODs,
        const FglTFRuntimeSkeletalMeshConfig& Config,
        FGWorldBakedSkeletalMeshNativeCallback Callback,
        const FString& SourceReference,
        const FWorldBakedBuildReferenceGuard& ReferenceGuard);
    void RetainAsyncBuildReferences(
        UglTFRuntimeAsset* Builder,
        const FWorldBakedBuildReferenceGuard& ReferenceGuard);
    void ReleaseAsyncBuildReferences(UglTFRuntimeAsset* Builder);
    void RemoveFinalizeRequest(UWorldBakedMeshFinalizeRequest* Request);

    UglTFRuntimeAsset* CreateMeshBuilder(FString& OutError);
    void CollectCachedTextureIds(TSet<int32>& OutTextureIds);
    void CollectCachedMaterialIds(
        const FglTFRuntimeMaterialsConfig& MaterialsConfig,
        TSet<int32>& OutMaterialIds);
    UMaterialInterface* FindCachedMaterial(
        int32 MaterialId,
        const FglTFRuntimeMaterialsConfig& MaterialsConfig,
        FWorldBakedBuildReferenceGuard& ReferenceGuard);
    UTexture2D* FindCachedTexture(
        int32 TextureId,
        FWorldBakedBuildReferenceGuard& ReferenceGuard);
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
