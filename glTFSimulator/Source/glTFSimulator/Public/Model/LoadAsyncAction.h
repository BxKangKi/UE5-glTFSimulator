// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Model/RuntimeData.h"
#include "glTFRuntimeAsset.h"
#include "Dom/JsonObject.h" // FJsonObject 처리를 위해 포함
#include "LoadAsyncAction.generated.h"

class UglTFRuntimeAsset;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FLoadAsyncCompleted,
    const FLoadAsyncWrapper &, MapWrapper);

UCLASS(BlueprintType)
class GLTFSIMULATOR_API ULoadAsyncAction : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    /**
     * glTF 에셋을 청크 단위로 비동기 로드하고, 동일 경로의 JSON 설정을 함께 병합합니다.
     * @param InJsonFilePath 확장자를 포함한 대상 JSON 파일의 절대 경로 또는 프로젝트 상대 경로
     */
    UFUNCTION(BlueprintCallable, meta=(WorldContext="WorldContextObject"))
    static ULoadAsyncAction *LoadAsync(
        UObject *WorldContextObject,
        UglTFRuntimeAsset *Asset,
        const FglTFRuntimeStaticMeshConfig &StaticMeshConfig,
        const int32 ChunkSize,
        const FString& InJsonFilePath);

    UPROPERTY(BlueprintAssignable)
    FLoadAsyncCompleted Completed;  // 사용자 정의 델리게이트

    virtual void Activate() override;

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

    // [추가] JSON 비동기 처리를 위한 멤버 변수
    FString JsonFilePath;

    UPROPERTY()
    FRuntimeModelData LoadedJsonModelData;

    UFUNCTION()
    void GetStaticMesh(UStaticMesh *StaticMesh);

    void UpdateModelNodeData();
    void CalculateSize();
    void ProcessChunk();
    void UpdateNext();
    void LoadTextureAsync(FString ImagePath);

    // JSON 로드 및 병합 제어 함수
    void LoadJsonAsync();
    void MergeJsonDataToMeshMap();
    
    // [추가] 파일 유실 시 디폴트 파일 생성을 위한 내부 헬퍼 함수
    bool CreateDefaultJsonFile(const FString& Path);
};