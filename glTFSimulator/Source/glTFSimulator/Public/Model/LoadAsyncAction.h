// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Model/ModelData.h"
#include "System/BinaryDataStore.h"
#include "glTFRuntimeAsset.h"
#include "LoadAsyncAction.generated.h"

class UglTFRuntimeAsset;
class UMaterialDefaultRuntimeCache;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FLoadAsyncCompleted,
    const FLoadAsyncWrapper &, MapWrapper);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FLoadAsyncProgress,
    float, Progress);

UCLASS(BlueprintType)
class GLTFSIMULATOR_API ULoadAsyncAction : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    /**
     * Asynchronously loads a glTF asset in chunks, merges read-only JSON settings, and uses the
     * sibling program-owned SCZ extent cache when its source-model hash matches.
     * @param InSourceFilePath Absolute source .glb/.gltf path used for hash validation.
     * @param InJsonFilePath Read-only user settings path. A missing template may be created once.
     * @param InSizeCacheFilePath Program-owned cache path including the .scz extension.
     */
    UFUNCTION(BlueprintCallable, meta=(WorldContext="WorldContextObject"))
    static ULoadAsyncAction *LoadAsync(
        UObject *WorldContextObject,
        UglTFRuntimeAsset *Asset,
        const FglTFRuntimeStaticMeshConfig &StaticMeshConfig,
        const int32 ChunkSize,
        const FString& InSourceFilePath,
        const FString& InJsonFilePath,
        const FString& InSizeCacheFilePath,
        bool bInCreateMissingJsonTemplate);

    UPROPERTY(BlueprintAssignable)
    FLoadAsyncCompleted Completed;  // Custom completion delegate.

    UPROPERTY(BlueprintAssignable)
    FLoadAsyncProgress Progress;

    virtual void Activate() override;

    void CancelAndRelease();

private:
    UPROPERTY()
    TObjectPtr<UObject> WorldContextObject;

    /** Shared GC guard retained until all native glTFRuntime callbacks have drained. */
    UPROPERTY(Transient)
    TObjectPtr<UMaterialDefaultRuntimeCache> MaterialReferenceGuard;

    UPROPERTY()
    TObjectPtr<UglTFRuntimeAsset> Asset;

    UPROPERTY()
    TArray<FglTFRuntimeNode> Nodes;

    /** One entry per parsed glTF node. Invalid/duplicate nodes remain progress work items. */
    UPROPERTY()
    TArray<uint8> NodeWorkValidity;

    UPROPERTY()
    TMap<FName, FModelMeshData> MeshMap;

    UPROPERTY()
    TMap<FName, FModelNodeData> NodeMap;

    UPROPERTY()
    TMap<FName, FWaterStreamNodeData> WaterNodeMap;

    FglTFRuntimeNode CurrentNode;
    FTimerHandle ProcessTimerHandle;
    FglTFRuntimeStaticMeshConfig StaticMeshConfig;
    int32 CurrentIndex;
    int32 MaxCount = 0;
    FName CurrentMeshName;
    int32 ChunkSize;
    bool bCancelled = false;
    bool bStaticMeshLoadInFlight = false;
    bool bCacheSaveInFlight = false;
    float LastProgressValue = 0.0f;

    UPROPERTY()
    FLoadAsyncWrapper PendingCompletionWrapper;

    /** Ticket held while this action owns or waits for its per-asset glTFRuntime slot. */
    uint64 GlTFRuntimeOperationTicket = 0;

    FModelData GeneratedModelData;
    FModelCacheData LoadedModelCache;
    FModelCacheData GeneratedModelCache;
    FString CurrentModelHash;
    bool bUseCachedMeshExtents = false;
    bool bModelCacheDirty = false;
    bool bCreateMissingJsonTemplate = true;

    // Immutable source/settings/cache paths used by asynchronous metadata processing.
    FString SourceFilePath;
    FString JsonFilePath;
    FString SizeCacheFilePath;

    UPROPERTY()
    FModelData LoadedJsonModelData;

    UFUNCTION()
    void GetStaticMesh(UStaticMesh *StaticMesh);

    void UpdateModelNodeData();
    void UpdateWaterNodeData();
    void CalculateSize();
    void ProcessChunk();
    void UpdateNext();
    void BroadcastProgressValue(float Value);
    void BroadcastNodeProgress();

    // Functions that control read-only JSON settings and program-owned SCZ metadata.
    void LoadSettingsAndCacheAsync();
    void SanitizeParsedData();
    void RefreshGeneratedModelData();
    void SaveGeneratedCacheThenComplete();
    void FinalizeCompletion();
    void WriteLogAsync(const FString& Message) const;
    void ReleaseActionReferences();

};
