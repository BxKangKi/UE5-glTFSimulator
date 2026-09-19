// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

/**
 * @file GameSettings.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Setting/GameSettings.h"
#include "System/FileFunctionLibrary.h"
#include "System/GameManagerSubSystem.h"
#include "System/MacroLibrary.h"
#include "Components/PostProcessComponent.h"
#include "GameFramework/GameUserSettings.h"
#include "Components/ActorComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/Scene.h"

#define SETTING_FILE_NAME TEXT("/settings.json")
#define SETTING_PATH FPaths::Combine(DIRECTORY_USER, DIRECTORY_GAME, SETTING_FILE_NAME)

TSharedRef<FJsonObject> UGameSettings::Serialization()
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(JSON_VERSION_FIELD, JSON_SCHEMA_VERSION);
    Json->SetNumberField(TEXT("BloomIntensity"), BloomIntensity);
    Json->SetNumberField(TEXT("BloomThreshold"), BloomThreshold);
    Json->SetNumberField(TEXT("AmbientOcclusionIntensity"), AmbientOcclusionIntensity);
    Json->SetNumberField(TEXT("Exposure"), Exposure);
    Json->SetNumberField(TEXT("ShadowQuality"), ShadowQuality);
    Json->SetNumberField(TEXT("TextureQuality"), TextureQuality);
    Json->SetNumberField(TEXT("MaxTextureResolution"), GetClampedMaxTextureResolution());
    Json->SetNumberField(TEXT("ViewDistanceQuality"), ViewDistanceQuality);
    Json->SetNumberField(TEXT("StreamingDistanceMultiplier"), StreamingDistanceMultiplier);
    Json->SetNumberField(TEXT("StreamingUnloadDistanceMultiplier"), StreamingUnloadDistanceMultiplier);
    Json->SetNumberField(TEXT("ObjectStreamingRadiusMeters"), ObjectStreamingRadiusMeters);
    Json->SetNumberField(TEXT("StreamingSceneSpawnBudget"), StreamingSceneSpawnBudget);
    Json->SetNumberField(TEXT("StreamingNodeBudgetPerFrame"), StreamingNodeBudgetPerFrame);
    Json->SetNumberField(TEXT("AntiAliasingQuality"), AntiAliasingQuality);
    Json->SetNumberField(TEXT("PostProcessingQuality"), PostProcessingQuality);
    Json->SetNumberField(TEXT("EffectsQuality"), EffectsQuality);
    Json->SetNumberField(TEXT("FoliageQuality"), FoliageQuality);
    Json->SetNumberField(TEXT("ShadingQuality"), ShadingQuality);
    Json->SetNumberField(TEXT("GlobalIlluminationQuality"), GlobalIlluminationQuality);
    Json->SetNumberField(TEXT("ReflectionQuality"), ReflectionQuality);
    Json->SetNumberField(TEXT("DynamicGlobalIlluminationMethod"), DynamicGlobalIlluminationMethod);
    Json->SetNumberField(TEXT("ReflectionMethod"), ReflectionMethod);
    Json->SetBoolField(TEXT("bRayTracing"), bRayTracing);
    Json->SetBoolField(TEXT("bHeightFog"), bHeightFog);
    Json->SetBoolField(TEXT("bCloud"), bCloud);
    Json->SetNumberField(TEXT("CelShadingMode"), CelShadingMode >= 0.5f ? 1.0f : 0.0f);
    return Json;
}

bool UGameSettings::Deserialization(TSharedPtr<FJsonObject> Json)
{
    if (Json.IsValid())
    {
        Json->TryGetNumberField(TEXT("BloomIntensity"), BloomIntensity);
        Json->TryGetNumberField(TEXT("BloomThreshold"), BloomThreshold);
        Json->TryGetNumberField(TEXT("AmbientOcclusionIntensity"), AmbientOcclusionIntensity);
        Json->TryGetNumberField(TEXT("Exposure"), Exposure);
        Exposure = Exposure <= -10.5f ? -11.0f : FMath::Clamp(Exposure, -10.0f, 20.0f);
        Json->TryGetNumberField(TEXT("ShadowQuality"), ShadowQuality);
        Json->TryGetNumberField(TEXT("TextureQuality"), TextureQuality);
        Json->TryGetNumberField(TEXT("MaxTextureResolution"), MaxTextureResolution);
        MaxTextureResolution = GetClampedMaxTextureResolution();
        Json->TryGetNumberField(TEXT("ViewDistanceQuality"), ViewDistanceQuality);
        Json->TryGetNumberField(TEXT("StreamingDistanceMultiplier"), StreamingDistanceMultiplier);
        Json->TryGetNumberField(TEXT("StreamingUnloadDistanceMultiplier"), StreamingUnloadDistanceMultiplier);
        Json->TryGetNumberField(TEXT("ObjectStreamingRadiusMeters"), ObjectStreamingRadiusMeters);
        Json->TryGetNumberField(TEXT("StreamingSceneSpawnBudget"), StreamingSceneSpawnBudget);
        Json->TryGetNumberField(TEXT("StreamingNodeBudgetPerFrame"), StreamingNodeBudgetPerFrame);
        StreamingDistanceMultiplier = FMath::Clamp(StreamingDistanceMultiplier, 1.0f, 512.0f);
        StreamingUnloadDistanceMultiplier = FMath::Clamp(StreamingUnloadDistanceMultiplier, 1.0f, 2.0f);
        ObjectStreamingRadiusMeters = FMath::Clamp(ObjectStreamingRadiusMeters, 512.0f, 4096.0f);
        StreamingSceneSpawnBudget = FMath::Clamp(StreamingSceneSpawnBudget, 1, 32);
        StreamingNodeBudgetPerFrame = FMath::Clamp(StreamingNodeBudgetPerFrame, 1, 256);
        Json->TryGetNumberField(TEXT("AntiAliasingQuality"), AntiAliasingQuality);
        Json->TryGetNumberField(TEXT("PostProcessingQuality"), PostProcessingQuality);
        Json->TryGetNumberField(TEXT("EffectsQuality"), EffectsQuality);
        Json->TryGetNumberField(TEXT("FoliageQuality"), FoliageQuality);
        Json->TryGetNumberField(TEXT("ShadingQuality"), ShadingQuality);
        Json->TryGetNumberField(TEXT("GlobalIlluminationQuality"), GlobalIlluminationQuality);
        Json->TryGetNumberField(TEXT("ReflectionQuality"), ReflectionQuality);
        Json->TryGetNumberField(TEXT("DynamicGlobalIlluminationMethod"), DynamicGlobalIlluminationMethod);
        Json->TryGetNumberField(TEXT("ReflectionMethod"), ReflectionMethod);
        Json->TryGetBoolField(TEXT("bRayTracing"), bRayTracing);
        Json->TryGetBoolField(TEXT("bHeightFog"), bHeightFog);
        Json->TryGetBoolField(TEXT("bCloud"), bCloud);
        Json->TryGetNumberField(TEXT("CelShadingMode"), CelShadingMode);
        CelShadingMode = CelShadingMode >= 0.5f ? 1.0f : 0.0f;
        return true;
    }
    return false;
}


int32 UGameSettings::GetClampedMaxTextureResolution() const
{
    // Runtime texture decode cost grows quadratically with resolution. Keep a
    // native clamp even when settings.json is edited by hand.
    return FMath::Clamp(MaxTextureResolution, 64, 8192);
}

float UGameSettings::GetViewDistanceScale() const
{
    // Preserve the historical custom-streaming radius at High (2). Lower tiers reduce I/O and
    // UObject churn; Epic increases the same authored size-proportional radius without changing data.
    switch (FMath::Clamp(ViewDistanceQuality, 0, 3))
    {
    case 0: return 0.50f;
    case 1: return 0.75f;
    case 3: return 1.50f;
    case 2:
    default: return 1.00f;
    }
}

float UGameSettings::GetEffectiveStreamingDistanceMultiplier() const
{
    return FMath::Clamp(StreamingDistanceMultiplier, 1.0f, 512.0f) * GetViewDistanceScale();
}

float UGameSettings::GetEffectiveObjectStreamingRadiusMeters() const
{
    return FMath::Clamp(ObjectStreamingRadiusMeters, 512.0f, 4096.0f) * GetViewDistanceScale();
}

float UGameSettings::GetStreamingUnloadDistanceMultiplier() const
{
    return FMath::Clamp(StreamingUnloadDistanceMultiplier, 1.0f, 2.0f);
}

int32 UGameSettings::GetStreamingSceneSpawnBudget() const
{
    return FMath::Clamp(StreamingSceneSpawnBudget, 1, 32);
}

int32 UGameSettings::GetStreamingNodeBudgetPerFrame() const
{
    return FMath::Clamp(StreamingNodeBudgetPerFrame, 1, 256);
}

int32 UGameSettings::ResolveMaxTextureResolution(const UObject* WorldContextObject)
{
    if (GEngine && WorldContextObject)
    {
        if (UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull))
        {
            if (UGameInstance* GameInstance = World->GetGameInstance())
            {
                if (const UGameManagerSubSystem* Manager = GameInstance->GetSubsystem<UGameManagerSubSystem>())
                {
                    if (const UGameSettings* Settings = Manager->GetGameSettings())
                    {
                        return Settings->GetClampedMaxTextureResolution();
                    }
                }
            }
        }
    }

    const FString Path = SETTING_PATH;
    if (const TSharedPtr<FJsonObject> Json = UFileFunctionLibrary::FromJson(Path); Json.IsValid())
    {
        int32 SavedResolution = GetDefaultMaxTextureResolution();
        if (Json->TryGetNumberField(TEXT("MaxTextureResolution"), SavedResolution))
        {
            return FMath::Clamp(SavedResolution, 64, 8192);
        }
    }

    return GetDefaultMaxTextureResolution();
}

UGameSettings *UGameSettings::CreateSettingsData(UObject *Onwer)
{
    TObjectPtr<UGameSettings> Data = NewObject<UGameSettings>(Onwer);
    if (Data)
    {
        Data->LoadSettingsData();
    }
    return Data;
}

void UGameSettings::LoadSettingsData()
{
    FString Path = SETTING_PATH;
    TSharedPtr<FJsonObject> Json = UFileFunctionLibrary::FromJson(Path);
    if (!Deserialization(Json))
    {
        UE_LOG(LogTemp, Log, TEXT("Setting file doesn't exist. Generate new one."));
        SaveSettingsData();
    }
}

void UGameSettings::SaveSettingsData()
{
    TSharedRef<FJsonObject> Json = Serialization();
    FString Path = SETTING_PATH;
    UFileFunctionLibrary::ToJsonAsync(Json, Path);
}

void UGameSettings::UpdateSettings(UPostProcessComponent *PostProcess)
{
    if (!IsValid(GEngine))
        return;

    UGameUserSettings *Settings = GEngine->GetGameUserSettings();
    if (Settings)
    {
        // 0-3 (or 0-4): Low through Epic (or Cinematic).
        Settings->SetShadowQuality(ShadowQuality);
        Settings->SetTextureQuality(TextureQuality);
        Settings->SetViewDistanceQuality(ViewDistanceQuality);
        Settings->SetAntiAliasingQuality(AntiAliasingQuality);
        Settings->SetPostProcessingQuality(PostProcessingQuality);
        Settings->SetFoliageQuality(FoliageQuality);
        Settings->SetShadingQuality(ShadingQuality);
        Settings->SetGlobalIlluminationQuality(GlobalIlluminationQuality);
        Settings->SetReflectionQuality(ReflectionQuality);
        Settings->SetVisualEffectQuality(EffectsQuality);
        // Use this path if resolution or window mode should be changed together.
        // Settings->SetScreenResolution(FIntPoint(1920, 1080));
        // Settings->SetFullscreenMode(EWindowMode::WindowedFullscreen);

        Settings->ApplySettings(false);
        Settings->SaveSettings();
    }

    if (IsValid(PostProcess))
    {
        FPostProcessSettings PPSettings = PostProcess->Settings;
        PPSettings.BloomIntensity = BloomIntensity;
        PPSettings.BloomThreshold = BloomThreshold;
        PPSettings.AmbientOcclusionIntensity = AmbientOcclusionIntensity;

        // Exposure=-11 is the explicit Auto sentinel. Auto uses the cheaper Basic metering mode
        // (single downsampled luminance value) with an EV100 adaptation window wide enough for
        // interiors/daylight. Manual values lock Min/Max to the requested EV100 without changing
        // the rest of the post-process stack.
        const bool bAutoExposure = Exposure <= -10.5f;
        const float ManualExposureEV = FMath::Clamp(Exposure, -10.0f, 20.0f);
        PPSettings.bOverride_AutoExposureMethod = true;
        PPSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Basic;
        PPSettings.bOverride_AutoExposureMinBrightness = true;
        PPSettings.bOverride_AutoExposureMaxBrightness = true;
        PPSettings.bOverride_AutoExposureSpeedUp = true;
        PPSettings.bOverride_AutoExposureSpeedDown = true;
        // Histogram-only controls are intentionally not overridden in Basic metering mode.
        PPSettings.bOverride_HistogramLogMin = false;
        PPSettings.bOverride_HistogramLogMax = false;
        PPSettings.bOverride_AutoExposureLowPercent = false;
        PPSettings.bOverride_AutoExposureHighPercent = false;
        PPSettings.bOverride_AutoExposureBias = true;
        PPSettings.AutoExposureBias = 0.0f;
        PPSettings.bOverride_AutoExposureBiasCurve = true;
        PPSettings.AutoExposureBiasCurve = nullptr;

        if (bAutoExposure)
        {
            PPSettings.AutoExposureMinBrightness = -5.0f;
            PPSettings.AutoExposureMaxBrightness = 13.0f;
            // F-stops/second: faster bright-scene recovery, slightly gentler dark adaptation.
            PPSettings.AutoExposureSpeedUp = 6.0f;
            PPSettings.AutoExposureSpeedDown = 4.0f;
        }
        else
        {
            PPSettings.AutoExposureMinBrightness = ManualExposureEV;
            PPSettings.AutoExposureMaxBrightness = ManualExposureEV;
            PPSettings.AutoExposureSpeedUp = 6.0f;
            PPSettings.AutoExposureSpeedDown = 4.0f;
        }

        PPSettings.DynamicGlobalIlluminationMethod = GetDynamicGlobalIlluminationMethod(DynamicGlobalIlluminationMethod);
        PPSettings.ReflectionMethod = GetReflectionMethod(ReflectionMethod);
        PostProcess->Settings = PPSettings;
    }
}

EDynamicGlobalIlluminationMethod::Type UGameSettings::GetDynamicGlobalIlluminationMethod(const int &Value)
{
    switch(Value)
    {
        case 0:
            return EDynamicGlobalIlluminationMethod::Type::None;
        case 1:
            return EDynamicGlobalIlluminationMethod::Type::Lumen;
        case 2:
            return EDynamicGlobalIlluminationMethod::Type::ScreenSpace;
        case 3:
            return EDynamicGlobalIlluminationMethod::Type::Plugin;
        default:
            return EDynamicGlobalIlluminationMethod::Type::None;
    }
}

EReflectionMethod::Type UGameSettings::GetReflectionMethod(const int &Value)
{
    switch (Value)
    {
        case 0:
            return EReflectionMethod::Type::None;
        case 1:
            return EReflectionMethod::Type::Lumen;
        case 2:
            return EReflectionMethod::Type::ScreenSpace;
        default:
            return EReflectionMethod::Type::None;
    }
}