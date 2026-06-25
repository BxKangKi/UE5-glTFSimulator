// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Model/RuntimeData.h"
#include "glTFRuntimeAsset.h"
#include "Components/BoxComponent.h"
#include "StreamAsyncAction.generated.h"

class AStreamActor;
class UStaticMeshComponent;
class UStaticMesh;
class UInstancedStaticMeshComponent;
class UBoxComponent;
class UShapeComponent;
class ULightComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FOnUpdateCompleted,
    const FStreamAsyncWrapper &, MapWrapper);

UCLASS()
class GLTFSIMULATOR_API UStreamAsyncAction : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintAssignable)
    FOnUpdateCompleted Completed;

    UFUNCTION(BlueprintCallable, meta = (WorldContext = "WorldContextObject"))
    static UStreamAsyncAction *StreamAsync(
        UObject *WorldContextObject,
        AStreamActor *Actor,
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

    // [해결] UPROPERTY()를 완전히 제외하여 중첩 컨테이너 UHT 빌드 차단 에러 우회
    UPROPERTY()
    TMap<FName, FRuntimeComponentGroup> DynamicComponentMap;

    // UnloadBox를 FName 기반으로 별도 관리하는 TMap 추가
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

    void SpawnRuntimeComponents(const FName &NodeName, const FModelNodeData &NodeInfo, const FRuntimeMeshData &Data);
    void DestroyRuntimeComponents(const FName &NodeName);
};