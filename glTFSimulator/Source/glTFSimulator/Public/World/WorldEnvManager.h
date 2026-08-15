// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WorldEnvManager.generated.h"

class UGameManagerSubSystem;
class UDirectionalLightComponent;
class UWorldData;
class UPostProcessComponent;
class USkyAtmosphereComponent;
class UStaticMeshComponent;
class USkyLightComponent;
class UVolumetricCloudComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UExponentialHeightFogComponent;
class UGameUpdateSubSystem;

/**
 * Rendering-only world actor.
 *
 * GameManagerSubSystem owns loading, time, saving, water, and streamed model spawning.
 * WorldEnvManager only owns sky/fog/cloud/light components and continuously reflects the current UWorldData.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API AWorldEnvManager : public AActor
{
    GENERATED_BODY()

public:
    AWorldEnvManager();

    /** Starts sky/light rendering updates from the supplied world data object. */
    UFUNCTION(BlueprintCallable, Category="World|Rendering")
    void InitializeRendering(UWorldData* InWorldData);

    /** Stops scheduled rendering updates without touching gameplay-owned systems. */
    UFUNCTION(BlueprintCallable, Category="World|Rendering")
    void StopRendering();

    /** Exposes the active world data for debug widgets that only read rendering state. */
    UFUNCTION(BlueprintPure, Category="World|Rendering")
    UWorldData* GetWorldData() const { return Data; }

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /** Optional cloud material assigned by the level or Blueprint subclass. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="World|Rendering")
    TObjectPtr<UMaterialInterface> CloudMaterial;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="World|Rendering")
    TObjectPtr<UStaticMeshComponent> Skybox;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="World|Rendering")
    TObjectPtr<UPostProcessComponent> PostProcess;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="World|Rendering")
    TObjectPtr<USkyAtmosphereComponent> SkyAtmosphere;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="World|Rendering")
    TObjectPtr<USkyLightComponent> SkyLight;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="World|Rendering")
    TObjectPtr<UDirectionalLightComponent> Sun;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="World|Rendering")
    TObjectPtr<UDirectionalLightComponent> Moon;

private:
    /** Reads current settings and creates optional fog/cloud components. */
    void ConfigureRenderingSettings();
    void ApplyCloudSettings();
    void DestroyCloudComponent();
    void DestroyFogComponent();
    void ReleaseDynamicRenderingResources();

    void RegisterGameUpdate();
    void UnregisterGameUpdate();
    void UpdateFromGameUpdate(float DeltaSeconds);

    /** Calculates and applies the current sun/moon rotations without a per-frame UObject. */
    void UpdateSkyLighting();

    UPROPERTY(Transient)
    TObjectPtr<UWorldData> Data;

    UPROPERTY(Transient)
    TObjectPtr<UVolumetricCloudComponent> Cloud;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> CloudMID;

    UPROPERTY(Transient)
    TObjectPtr<UExponentialHeightFogComponent> Fog;

    /** Non-owning reference: the game-instance subsystem outlives this world actor. */
    UPROPERTY(Transient)
    TWeakObjectPtr<UGameManagerSubSystem> SubSystem;

    int32 GameUpdateTickHandle = INDEX_NONE;
    bool bRenderingActive = false;
};
