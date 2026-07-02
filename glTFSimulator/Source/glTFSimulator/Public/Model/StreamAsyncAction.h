// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Model/Data.h"
#include "glTFRuntimeAsset.h"
#include "Components/BoxComponent.h"
#include "StreamAsyncAction.generated.h"

class AglTFStreamActor;
class UStaticMeshComponent;
class UStaticMesh;
class UInstancedStaticMeshComponent;
class UBoxComponent;
class UShapeComponent;
class ULightComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FOnUpdateCompleted,
    const FStreamAsyncWrapper &, MapWrapper);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FOnStreamProgress,
    float, Progress);

UCLASS()
class GLTFSIMULATOR_API UStreamAsyncAction : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintAssignable)
    FOnUpdateCompleted Completed;

    UPROPERTY(BlueprintAssignable)
    FOnStreamProgress Progress;

    UFUNCTION(BlueprintCallable, meta = (WorldContext = "WorldContextObject"))
    static UStreamAsyncAction *StreamAsync(
        UObject *WorldContextObject,
        AglTFStreamActor *Actor,
        const FVector &InPlayerLocation,
        const FglTFRuntimeStaticMeshConfig &StaticMeshConfig,
        float InDistance = 65536.0f,
        int32 InChunkSize = 256);

    virtual void Activate() override;

private:
    UPROPERTY()
    TObjectPtr<UObject> WorldContextObject;
    UPROPERTY()
    TMap<FName, FModelNodeData> NodeMap;
    UPROPERTY()
    TMap<FName, FModelMeshData> MeshMap;
    UPROPERTY()
    TSet<FName> LoadedNodes;
    UPROPERTY()
    TMap<FName, TObjectPtr<UInstancedStaticMeshComponent>> InstanceMap;

    // Excluded from UPROPERTY to avoid UHT nested-container build errors.
    UPROPERTY()
    TMap<FName, FComponentGroup> DynamicComponentMap;

    // Uses a separate FName-keyed map to manage unload boxes.
    UPROPERTY()
    TMap<FName, TObjectPtr<UBoxComponent>> UnloadBoxMap;

    UPROPERTY()
    TObjectPtr<UMaterialInterface> DecalLight;
    UPROPERTY()
    TArray<FName> PendingLoadNodes;
    UPROPERTY()
    TArray<FName> PendingUnloadNodes;
    UPROPERTY()
    TObjectPtr<UglTFRuntimeAsset> Asset;
    UPROPERTY()
    TObjectPtr<AActor> OwnerActor;
    UPROPERTY()
    TObjectPtr<UStaticMeshComponent> PendingComp = nullptr;

    FVector CurrentSize;
    bool bIsLoading = false;
    FName CurrentLoadingNode;
    FName CurrentLoadingMesh;

    int32 CurrentLoadIndex;
    int32 CurrentUnloadIndex;
    int32 ChunkSize;
    int32 TotalOperationCount = 0;

    FTimerHandle ProcessTimerHandle;
    FglTFRuntimeStaticMeshConfig StaticMeshConfig;
    FVector PlayerLocation;
    float Distance;

    UFUNCTION()
    void SetStaticMesh(UStaticMesh *StaticMesh);

    void ProcessChunk();
    void ResetLoadState();
    void LoadStaticMeshAsync(const FName &Name);
    void AddTrasnform(const FName &Name, UInstancedStaticMeshComponent *ISMC);
    void ProcessUnloadNode(const FName &Name);
    bool ProcessLoadNode(const FName &Name);

    void SpawnStreamComponents(const FName &NodeName, const FModelNodeData &NodeInfo, const FMeshData &Data);
    void DestroyStreamComponents(const FName &NodeName);
    void BroadcastProgress();
};