// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Model/ModelData.h"
#include "glTFRuntimeAsset.h"
#include "Dom/JsonObject.h" // Required for FJsonObject handling.
#include "LoadAsyncAction.generated.h"

class UglTFRuntimeAsset;

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
     * Asynchronously loads a glTF asset in chunks and merges the JSON settings from the same path.
     * @param InJsonFilePath Absolute or project-relative target JSON file path including the extension.
     */
    UFUNCTION(BlueprintCallable, meta=(WorldContext="WorldContextObject"))
    static ULoadAsyncAction *LoadAsync(
        UObject *WorldContextObject,
        UglTFRuntimeAsset *Asset,
        const FglTFRuntimeStaticMeshConfig &StaticMeshConfig,
        const int32 ChunkSize,
        const FString& InJsonFilePath);

    UPROPERTY(BlueprintAssignable)
    FLoadAsyncCompleted Completed;  // Custom completion delegate.

    UPROPERTY(BlueprintAssignable)
    FLoadAsyncProgress Progress;

    virtual void Activate() override;

    void CancelAndRelease();

private:
    UPROPERTY()
    TObjectPtr<UObject> WorldContextObject;

    UPROPERTY()
    TObjectPtr<UglTFRuntimeAsset> Asset;

    UPROPERTY()
    TArray<FglTFRuntimeNode> Nodes;

    UPROPERTY()
    TMap<FName, FModelMeshData> MeshMap;

    UPROPERTY()
    TMap<FName, FModelNodeData> NodeMap;

    FglTFRuntimeNode CurrentNode;
    FTimerHandle ProcessTimerHandle;
    FglTFRuntimeStaticMeshConfig StaticMeshConfig;
    int32 CurrentIndex;
    int32 MaxCount = 0;
    FName CurrentMeshName;
    int32 ChunkSize;
    bool bCancelled = false;
    FModelData GeneratedModelData;

    // Members used for asynchronous JSON processing.
    FString JsonFilePath;

    UPROPERTY()
    FModelData LoadedJsonModelData;

    UFUNCTION()
    void GetStaticMesh(UStaticMesh *StaticMesh);

    void UpdateModelNodeData();
    void CalculateSize();
    void ProcessChunk();
    void UpdateNext();
    void LoadTextureAsync(FString ImagePath);

    // Functions that control JSON loading and merging.
    void LoadJsonAsync();
    void MergeJsonDataToMeshMap();
    void RefreshGeneratedModelData();
    void SaveGeneratedJsonAsync() const;
    void WriteLogAsync(const FString& Message) const;
    void ReleaseActionReferences();
    
    // Internal helper that creates a default file when the source file is missing.
    bool CreateDefaultJsonFile(const FString& Path);
};