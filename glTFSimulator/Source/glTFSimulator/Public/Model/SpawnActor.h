// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Model/RuntimeData.h"
#include "glTFRuntimeParser.h"
#include "SpawnActor.generated.h"

class AStreamActor;
class UglTFRuntimeAsset;

UCLASS()
class GLTFSIMULATOR_API ASpawnActor : public AActor
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable)
    void Init(const FString &Path);

    UFUNCTION(BlueprintCallable)
    bool GetIsLoaded() const { return bIsLoaded; }

    UFUNCTION(BlueprintCallable)
    bool GetIsDestroyed() const { return bIsDestroyed; }

    UFUNCTION(BlueprintCallable)
    FString GetFilePath() const { return FilePath; }

    UFUNCTION(BlueprintCallable)
    UglTFRuntimeAsset *GetAsset() const { return glTFRuntimeAsset; }

    UFUNCTION(BlueprintCallable)
    TMap<FName, FModelNodeData> GetAllNodeMap() const { return AllNodeMap; }

    UFUNCTION(BlueprintCallable)
    TMap<FName, FModelMeshData> GetAllMeshMap() const { return AllMeshMap; }

    // StreamActor 스폰 시 활용할 클래스 (BP 지정 가능)
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TSubclassOf<AStreamActor> StreamActorClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    int32 BatchCount = 0;

    UFUNCTION()
    void OnAssetLoaded(UglTFRuntimeAsset *Asset);

    UFUNCTION()
    void OnChunksLoaded(const FLoadAsyncWrapper &MapWrapper);
    float GetLoadingStatus() const { return LoadingStatus; }

protected:
    virtual void BeginPlay() override;
    virtual void Destroyed() override;

private:
    UPROPERTY()
    TArray<TObjectPtr<AStreamActor>> StreamActors;

    UPROPERTY()
    TObjectPtr<UglTFRuntimeAsset> glTFRuntimeAsset;

    UPROPERTY()
    TMap<FName, FModelNodeData> AllNodeMap;

    UPROPERTY()
    TMap<FName, FModelMeshData> AllMeshMap;

    FString FilePath;
    bool bIsLoaded = false;
    bool bAssetLoaded = false;
    bool bIsDestroyed;
    void SpawnStreamActor(const TMap<FName, FModelNodeData> &Nodes);
    void LoadRuntimeData();
    void CheckLoadedAsync();
    void CheckLoadedAsyncInternal();
    bool CheckAllStreamActorLoaded();
    void BatchNodeMap(const FLoadAsyncWrapper &Map);
    float LoadingStatus;
    UFUNCTION()
    void LoadAssetAsync();
};