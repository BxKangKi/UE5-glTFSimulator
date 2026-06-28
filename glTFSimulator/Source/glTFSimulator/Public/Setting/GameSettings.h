// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

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

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Rendering")
    bool bRayTracing = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|World")
    bool bHeightFog = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|World")
    bool bCloud = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 ShadowQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 TextureQuality = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SettingData|Quality", meta=(ClampMin="0", ClampMax="3"))
    int32 ViewDistanceQuality = 2;

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