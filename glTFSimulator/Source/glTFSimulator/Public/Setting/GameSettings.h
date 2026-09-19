// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

/**
 * @file GameSettings.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Interface/JsonData.h"
#include "Engine/EngineTypes.h"
#include "GameSettings.generated.h"

class UPostProcessComponent;
class UActorComponent;

UENUM(BlueprintType)
enum class EQualitySettings : uint8 {
    Low UMETA(DisplayName = "Low"),
    Medium UMETA(DisplayName = "Medium"),
    High UMETA(DisplayName = "High"),
    Epic UMETA(DisplayName = "Epic")
};

UCLASS(BlueprintType)
class GLTFSIMULATOR_API UGameSettings : public UObject, public IJsonData
{
    GENERATED_BODY()

public:
    virtual TSharedRef<FJsonObject> Serialization() override;
    virtual bool Deserialization(TSharedPtr<FJsonObject> Json) override;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|PostProcess")
    float BloomIntensity = 0.675f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|PostProcess")
    float BloomThreshold = -1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|PostProcess")
    float AmbientOcclusionIntensity = 0.5f;

    /** -11 = Histogram Auto Exposure. -10..20 = fixed manual EV100. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|PostProcess", meta=(ClampMin="-11.0", ClampMax="20.0"))
    float Exposure = -11.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Rendering")
    bool bRayTracing = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|World")
    bool bHeightFog = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|World")
    bool bCloud = true;

    /** 0.0 = disabled, 1.0 = enabled. Written directly to ShaderLibraryMPC.CelShadingMode. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Rendering", meta=(ClampMin="0.0", ClampMax="1.0"))
    float CelShadingMode = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 ShadowQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 TextureQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Rendering", meta=(ClampMin="64", ClampMax="8192"))
    int32 MaxTextureResolution = 768;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 ViewDistanceQuality = 2;

    /** Base size-proportional streaming radius multiplier. Effective radius also follows ViewDistanceQuality. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Streaming", meta=(ClampMin="1.0", ClampMax="512.0"))
    float StreamingDistanceMultiplier = 64.0f;

    /** Hysteresis applied only while a scene/model is already resident, preventing boundary thrash. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Streaming", meta=(ClampMin="1.0", ClampMax="2.0"))
    float StreamingUnloadDistanceMultiplier = 1.10f;

    /** Base mutable-object chunk radius. Effective radius also follows ViewDistanceQuality. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Streaming", meta=(ClampMin="512.0", ClampMax="4096.0"))
    float ObjectStreamingRadiusMeters = 2048.0f;

    /** Maximum coarse scene actors spawned per update and mesh-group stream actions activated per frame. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Streaming", meta=(ClampMin="1", ClampMax="32"))
    int32 StreamingSceneSpawnBudget = 32;

    /** Maximum node load/unload operations scheduled by one stream action frame. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Streaming", meta=(ClampMin="1", ClampMax="256"))
    int32 StreamingNodeBudgetPerFrame = 256;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 AntiAliasingQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 PostProcessingQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 EffectsQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 FoliageQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 ShadingQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 GlobalIlluminationQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 ReflectionQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Rendering", meta=(ClampMin="0", ClampMax="3"))
    int32 DynamicGlobalIlluminationMethod = 1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Rendering", meta=(ClampMin="0", ClampMax="2"))
    int32 ReflectionMethod = 1;

    static UGameSettings *CreateSettingsData(UObject *Onwer = nullptr);
    static int32 GetDefaultMaxTextureResolution() { return 768; }
    int32 GetClampedMaxTextureResolution() const;

    /** Quality scale shared by engine view distance and custom archive streaming. */
    UFUNCTION(BlueprintPure, Category="Settings|Streaming")
    float GetViewDistanceScale() const;

    UFUNCTION(BlueprintPure, Category="Settings|Streaming")
    float GetEffectiveStreamingDistanceMultiplier() const;

    UFUNCTION(BlueprintPure, Category="Settings|Streaming")
    float GetEffectiveObjectStreamingRadiusMeters() const;

    UFUNCTION(BlueprintPure, Category="Settings|Streaming")
    float GetStreamingUnloadDistanceMultiplier() const;

    UFUNCTION(BlueprintPure, Category="Settings|Streaming")
    int32 GetStreamingSceneSpawnBudget() const;

    UFUNCTION(BlueprintPure, Category="Settings|Streaming")
    int32 GetStreamingNodeBudgetPerFrame() const;

    static int32 ResolveMaxTextureResolution(const UObject* WorldContextObject);
    UFUNCTION()
    void LoadSettingsData();
    UFUNCTION()
    void SaveSettingsData();
    UFUNCTION()
    void UpdateSettings(UPostProcessComponent *PostProcess);

private:
    EDynamicGlobalIlluminationMethod::Type GetDynamicGlobalIlluminationMethod(const int &Value);
    EReflectionMethod::Type GetReflectionMethod(const int &Value);
};