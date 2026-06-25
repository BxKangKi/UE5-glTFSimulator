// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Model/StreamDefaultAsset.h"
#include "Model/RuntimeData.h"
#include "StreamActor.generated.h"

class ASpawnActor;
class UBoxComponent;
class UInstancedStaticMeshComponent;
class UglTFRuntimeAsset;
class UTexture2DArray;
struct FStreamAsyncWrapper; // AsyncWrapper 구조체 전방 선언

UCLASS()
class GLTFSIMULATOR_API AStreamActor : public AActor
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintReadOnly)
    bool bIsLoaded = false;

    UPROPERTY(BlueprintReadOnly)
    TObjectPtr<ASpawnActor> SpawnActor;

    UPROPERTY(BlueprintReadOnly)
    TMap<FName, FModelNodeData> NodeMap;

    UPROPERTY(BlueprintReadOnly)
    TMap<FName, TObjectPtr<UInstancedStaticMeshComponent>> InstanceMap;

    UPROPERTY(EditAnywhere, BlueprintReadOnly)
    TObjectPtr<UMaterialInterface> DecalLight;

    UPROPERTY()
    TMap<FName, FRuntimeComponentGroup> DynamicComponentMap;

    UPROPERTY()
    TMap<FName, TObjectPtr<UBoxComponent>> UnloadBoxMap;

    UPROPERTY(BlueprintReadOnly)
    TSet<FName> LoadedNodes;

    UPROPERTY(BlueprintReadOnly)
    TObjectPtr<UglTFRuntimeAsset> Asset;

    UPROPERTY(Transient)
    TObjectPtr<UTexture2DArray> RuntimeTerrainTextureArray;

    UPROPERTY(BlueprintReadOnly)
    bool bAsyncLoading = false;

    // 블루프린트 매크로/노드에서 사용하던 변수들 추가
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    int32 ChunkSize = 256;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float StreamDistance = 64.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FStreamDefaultAsset Default;

    UFUNCTION(BlueprintCallable)
    void Init(ASpawnActor *InActor, const TMap<FName, FModelNodeData> &Nodes);

protected:
    virtual void BeginPlay() override;
    void AsyncTick();

    UFUNCTION(BlueprintCallable)
    void UpdateProperties(FStreamAsyncWrapper Collection);

    // 비동기 태스크 완료 콜백
    UFUNCTION()
    void OnStreamAsyncCompleted(const FStreamAsyncWrapper &MapWrapper);

    void ApplyTerrainTextureArrayToLoadedMaterials();
    bool IsTerrainMaterial(const UMaterialInterface* Material) const;
};
