// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file WorldEnvManager.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

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
class UMaterialParameterCollection;
struct FStreamableHandle;
class UExponentialHeightFogComponent;
class UGameUpdateSubSystem;

/**
 * Rendering-only world actor.
 *
 * GameManagerSubSystem owns loading, time, saving, water, and streamed model spawning.
 * WorldEnvManager only owns sky/fog/cloud/light components and continuously reflects the current UWorldData.
 * The default sky is procedural SkyAtmosphere, so a missing or uncooked sky-dome mesh can never
 * turn the packaged world black.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API AWorldEnvManager : public AActor
{
    GENERATED_BODY()

public:
    AWorldEnvManager();

    /**
     * Makes the dependency-free procedural sky visible before config/.dat/.gworld I/O starts.
     * This is deliberately separate from InitializeRendering because archive/config startup can
     * take time and no UWorldData exists during that interval.
     */
    void PrepareForWorldLoading();

    /** Starts sky/light rendering updates from the supplied world data object. */
    UFUNCTION(BlueprintCallable, Category="World|Rendering")
    void InitializeRendering(UWorldData* InWorldData);

    /** Stops scheduled rendering updates without touching gameplay-owned systems. */
    UFUNCTION(BlueprintCallable, Category="World|Rendering")
    void StopRendering();

    /** Exposes the active world data for debug widgets that only read rendering state. */
    UFUNCTION(BlueprintPure, Category="World|Rendering")
    UWorldData* GetWorldData() const { return Data.Get(); }

    /**
     * Re-applies settings.json fog/cloud toggles to an already running world.
     * Public because GameManagerSubSystem and Blueprint settings UIs may refresh
     * the active environment immediately after settings are applied.
     */
    UFUNCTION(BlueprintCallable, Category="World|Rendering")
    void RefreshRuntimeSettings();

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /** Runtime cloud material resolved on demand from the central Asset Registry. */
    UPROPERTY(Transient, BlueprintReadOnly, Category="World|Rendering")
    TObjectPtr<UMaterialInterface> CloudMaterial;

    /**
     * Optional authored sky dome. Existing WorldEnvManager Blueprints can assign SM_Skybox here;
     * the procedural atmosphere remains behind it as the loading/error fallback.
     */
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
    /** Re-enables the native core components even when a Blueprint default hid one of them. */
    void EnsureCoreSkyVisible();

    /** Starts one non-blocking batch for environment shader/sky assets from the AssetRegistry. */
    void QueueEnvironmentAssetLoad();
    void HandleEnvironmentAssetsReady();
    void RefreshShaderLibraryParameters();

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

    /** Strong refs for asynchronously resolved global shader assets. */
    UPROPERTY(Transient)
    TObjectPtr<UMaterialInterface> GlobalPostProcessMaterial;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialParameterCollection> ShaderLibraryMPC;

    TSharedPtr<FStreamableHandle> EnvironmentAssetLoadHandle;

    /** Non-owning reference: the game-instance subsystem outlives this world actor. */
    UPROPERTY(Transient)
    TWeakObjectPtr<UGameManagerSubSystem> SubSystem;

    int32 GameUpdateTickHandle = INDEX_NONE;
    bool bRenderingActive = false;
};
