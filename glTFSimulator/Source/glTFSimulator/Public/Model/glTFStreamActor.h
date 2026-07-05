// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HAL/ThreadSafeCounter.h"
#include "Model/ModelData.h"
#include "Model/StreamDefaultAsset.h"
#include "glTFRuntimeParser.h"
#include "glTFStreamActor.generated.h"

class UBoxComponent;
class UInstancedStaticMeshComponent;
class UglTFRuntimeAsset;
class ULoadAsyncAction;
class UStreamAsyncAction;
class UTexture;
struct FLoadAsyncWrapper;
struct FStreamAsyncWrapper;

enum class EGLTFStreamAssetPhase : uint8
{
    None,
    SizeScan,
    Streaming
};

UCLASS()
class GLTFSIMULATOR_API AglTFStreamActor : public AActor
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable)
    void Init(const FString& Path);

    UFUNCTION(BlueprintCallable)
    bool GetIsLoaded() const { return bIsLoaded; }

    UFUNCTION(BlueprintCallable)
    bool GetIsDestroyed() const { return bIsDestroyed; }

    UFUNCTION(BlueprintCallable)
    FString GetFilePath() const { return FilePath; }

    UFUNCTION(BlueprintCallable)
    UglTFRuntimeAsset* GetAsset() const { return glTFAsset; }

    void ReleaseRuntimeResourcesForWorldExit();

    bool HasModelMetadata() const { return bHasModelMetadata; }
    const FModelData& GetModelMetadata() const { return ModelMetadata; }

    UFUNCTION(BlueprintCallable)
    TMap<FName, FModelNodeData> GetAllNodeMap() const { return AllNodeMap; }

    UFUNCTION(BlueprintCallable)
    TMap<FName, FModelMeshData> GetAllMeshMap() const { return AllMeshMap; }

    const TMap<FName, FModelNodeData>& GetAllNodeMapRef() const { return AllNodeMap; }
    const TMap<FName, FModelMeshData>& GetAllMeshMapRef() const { return AllMeshMap; }
    const TSet<FName>& GetLoadedNodesRef() const { return LoadedNodes; }
    const TMap<FName, TObjectPtr<UInstancedStaticMeshComponent>>& GetInstanceMapRef() const { return InstanceMap; }
    const TMap<FName, TObjectPtr<UBoxComponent>>& GetUnloadBoxMapRef() const { return UnloadBoxMap; }
    const TMap<FName, FComponentGroup>& GetDynamicComponentMapRef() const { return DynamicComponentMap; }
    UMaterialInterface* GetDecalLight() const { return DecalLight; }

    UFUNCTION(BlueprintCallable)
    float GetLoadingStatus() const { return LoadingStatus; }

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    int32 ChunkSize = 256;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float StreamDistance = 64.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FStreamDefaultAsset Default;

    /** Optional texture used by custom terrain materials. Leave empty to use the original glTF material textures. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UTexture> TerrainTextureOverride;

    UPROPERTY(EditAnywhere, BlueprintReadOnly)
    TObjectPtr<UMaterialInterface> DecalLight;

    UPROPERTY(BlueprintReadOnly)
    bool bIsLoaded = false;

    UPROPERTY(BlueprintReadOnly)
    bool bAsyncLoading = false;

    UFUNCTION()
    void OnAssetLoaded(UglTFRuntimeAsset* Asset);

    UFUNCTION()
    void OnChunksLoaded(const FLoadAsyncWrapper& MapWrapper);

    UFUNCTION()
    void OnSizeScanProgress(float Progress);

    UFUNCTION()
    void OnStreamAsyncProgress(float Progress);

    UFUNCTION()
    void OnStreamAsyncCompleted(const FStreamAsyncWrapper& MapWrapper);

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Destroyed() override;

private:
    UPROPERTY()
    TObjectPtr<UglTFRuntimeAsset> glTFAsset;

    UPROPERTY()
    TObjectPtr<ULoadAsyncAction> ActiveSizeScanAction;

    UPROPERTY()
    TObjectPtr<UStreamAsyncAction> ActiveStreamAction;

    UPROPERTY()
    TMap<FName, FModelNodeData> AllNodeMap;

    UPROPERTY()
    TMap<FName, FModelMeshData> AllMeshMap;

    UPROPERTY()
    TSet<FName> LoadedNodes;

    UPROPERTY()
    TMap<FName, TObjectPtr<UInstancedStaticMeshComponent>> InstanceMap;

    UPROPERTY()
    TMap<FName, TObjectPtr<UBoxComponent>> UnloadBoxMap;

    UPROPERTY()
    TMap<FName, FComponentGroup> DynamicComponentMap;

    UPROPERTY()
    FModelData ModelMetadata;

    FString FilePath;
    bool bHasModelMetadata = false;
    bool bIsDestroyed = false;
    float LoadingStatus = 0.0f;
    EGLTFStreamAssetPhase AssetLoadPhase = EGLTFStreamAssetPhase::None;
    TSharedPtr<FThreadSafeCounter, ESPMode::ThreadSafe> ActiveAssetLoadCancelToken;
    int32 AssetLoadRequestSerial = 0;

    void LoadAssetAsync(EGLTFStreamAssetPhase Phase);
    void CancelActiveAssetLoad();
    void StartSizeScan(UglTFRuntimeAsset* Asset);
    void CancelActiveAsyncActions();
    void ReleaseAsset(UglTFRuntimeAsset* Asset);
    void ReleaseStreamingResources();
    void StartStreaming();
    void AsyncTick();
    void UpdateProperties(const FStreamAsyncWrapper& Collection);
    void ApplyTerrainTextureOverrideToLoadedMaterials();
    bool IsTerrainMaterial(const UMaterialInterface* Material) const;
    bool IsPlayerInsideModelRange() const;
    void WriteLogAsync(const FString& Message) const;
    FglTFRuntimeStaticMeshConfig BuildStreamingStaticMeshConfig();
    int32 GetSizeScanChunkSize(int32 TotalNodeCount) const;
};
