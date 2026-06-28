// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "World/LightRotation.h"
#include "WorldManager.generated.h"

class UGameManagerSubSystem;
class UDirectionalLightComponent;
class UWorldData;
class ASpawnActor;
class UPostProcessComponent;
class USkyAtmosphereComponent;
class UStaticMeshComponent;
class USkyLightComponent;
class UVolumetricCloudComponent;
class UMaterialInterface;
class UStaticMesh;
class UExponentialHeightFogComponent;
class ARuntimeGameplayManager;

UCLASS() class GLTFSIMULATOR_API AWorldManager : public AActor
{
    GENERATED_BODY()
public:
    AWorldManager();
    void Load();
    float GetLoadingStatus() const { return LoadingStatus; }

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TSubclassOf<class ASpawnActor> SpawnActorClass;
    // 에디터에서 WBP_Loading_C 클래스를 선택할 수 있게 노출합니다.
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TSubclassOf<UUserWidget> LoadingWidgetClass;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TSubclassOf<AActor> WaterClass;
    // 바다 스폰 위치 (BP의 OceanTransform 변수)
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FTransform OceanTransform;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UMaterialInterface> CloudMaterial;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UStaticMeshComponent> Skybox;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UPostProcessComponent> PostProcess;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<USkyAtmosphereComponent> SkyAtmosphere;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<USkyLightComponent> SkyLight;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UDirectionalLightComponent> Sun;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UDirectionalLightComponent> Moon;

private:
    float LoadingStatus;
    void AddSpawnActor(ASpawnActor *Actor);
    bool CheckAllSpawnActorLoaded();
    void LoadSpawnActor(const FString &Path);
    FString GetFilePath(const FString &FileName);
    void SaveWorldData();
    void LoadWorldData();
    void WorldUpdate(float DeltaTime);
    bool CheckPlayerLoaded();
    void ActivatePlayer();
    bool SpawnPlayer();
    void SpawnWorld();
    bool CheckOcean();
    void SpawnOcean();
    void LoadWorldAsync();
    void LoadPlayerAsync();
    void ShowLoadingWidget();
    UPROPERTY()
    TObjectPtr<UWorldData> Data;
    UPROPERTY()
    TObjectPtr<AActor> Ocean;
    UPROPERTY()
    TObjectPtr<UVolumetricCloudComponent> Cloud;
    UPROPERTY()
    TObjectPtr<UExponentialHeightFogComponent> Fog;
    UPROPERTY()
    TObjectPtr<UGameManagerSubSystem> SubSystem;
    UPROPERTY()
    TArray<TObjectPtr<ASpawnActor>> SpawnActors;
    UPROPERTY()
    TObjectPtr<UUserWidget> LoadingWidgetInstance;
    UPROPERTY()
    TObjectPtr<ARuntimeGameplayManager> RuntimeGameplayManager;
    FTimerHandle TimerHandle_SaveTick;
    FTimerHandle TimerHandle_AsyncTick;
    FTimerHandle TimerHandle_LoadWorld;
    FTimerHandle TimerHandle_SpawnPlayer;
    FTimerHandle TimerHandle_LoadPlayer;
    UFUNCTION()
    void SpawnPlayerAsync();
    UFUNCTION()
    void AsyncTick();
    UFUNCTION()
    void SaveTick();
    void SpawnRuntimeGameplayManager();
    UFUNCTION()
    void SkyUpdate(FLightRotation Result);
};