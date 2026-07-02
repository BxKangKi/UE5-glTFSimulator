// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "World/LightRotation.h"
#include "WorldManager.generated.h"

class UGameManagerSubSystem;
class UDirectionalLightComponent;
class UWorldData;
class UPostProcessComponent;
class USkyAtmosphereComponent;
class UStaticMeshComponent;
class USkyLightComponent;
class UVolumetricCloudComponent;
class UMaterialInterface;
class UExponentialHeightFogComponent;

/**
 * Rendering-only world actor.
 *
 * GameManagerSubSystem owns loading, time, saving, water, and streamed model spawning.
 * WorldManager only owns sky/fog/cloud/light components and continuously reflects the current UWorldData.
 */
UCLASS()
class GLTFSIMULATOR_API AWorldManager : public AActor
{
    GENERATED_BODY()

public:
    AWorldManager();

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

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="World|Rendering")
    TObjectPtr<UStaticMeshComponent> Skybox;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="World|Rendering")
    TObjectPtr<UPostProcessComponent> PostProcess;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="World|Rendering")
    TObjectPtr<USkyAtmosphereComponent> SkyAtmosphere;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="World|Rendering")
    TObjectPtr<USkyLightComponent> SkyLight;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="World|Rendering")
    TObjectPtr<UDirectionalLightComponent> Sun;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="World|Rendering")
    TObjectPtr<UDirectionalLightComponent> Moon;

private:
    /** Reads current settings and creates optional fog/cloud components. */
    void ConfigureRenderingSettings();

    /** Queues the next asynchronous sky rotation calculation. */
    UFUNCTION()
    void AsyncTick();

    /** Applies the asynchronous sun/moon rotation result. */
    UFUNCTION()
    void SkyUpdate(FLightRotation Result);

    UPROPERTY()
    TObjectPtr<UWorldData> Data;

    UPROPERTY()
    TObjectPtr<UVolumetricCloudComponent> Cloud;

    UPROPERTY()
    TObjectPtr<UExponentialHeightFogComponent> Fog;

    UPROPERTY()
    TObjectPtr<UGameManagerSubSystem> SubSystem;

    bool bRenderingActive = false;
};
