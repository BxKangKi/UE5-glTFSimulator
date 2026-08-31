// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "MPCComponent.h"
#include "Engine/World.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"

const FName UMPCComponent::WeatherModeParameterName(TEXT("WeatherMode"));
const FName UMPCComponent::PrecipitationParameterName(TEXT("Precipitation"));
const FName UMPCComponent::CelShadingModeParameterName(TEXT("CelShadingMode"));

UMPCComponent::UMPCComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UMPCComponent::BeginPlay()
{
    Super::BeginPlay();

    if (bApplyDefaultsOnBeginPlay)
    {
        ApplyWeatherInternal();
    }
}

void UMPCComponent::SetWeather(float InWeatherMode, float InPrecipitation)
{
    WeatherMode = FMath::Clamp(InWeatherMode, 0.0f, 1.0f);

    // Precipitation = 0 means clear, independent of the rain/snow blend value.
    Precipitation = FMath::Clamp(InPrecipitation, 0.0f, 1.0f);

    ApplyWeatherInternal();
}

void UMPCComponent::SetCelShadingMode(float InCelShadingMode)
{
    CelShadingMode = FMath::Clamp(InCelShadingMode, 0.0f, 1.0f);

    ApplyCelShadingModeInternal();
}

void UMPCComponent::SetWeatherClear()
{
    Precipitation = 0.0f;
    ApplyWeatherInternal();
}

bool UMPCComponent::ApplyWeather()
{
    return ApplyWeatherInternal();
}

bool UMPCComponent::ApplyCelShadingMode()
{
    return ApplyCelShadingModeInternal();
}

UMaterialParameterCollectionInstance* UMPCComponent::GetMPC()
{
    UWorld* World = GetWorld();
    if (!World || !MPC)
    {
        return nullptr;
    }
    return World->GetParameterCollectionInstance(MPC);
}

bool UMPCComponent::ApplyCelShadingModeInternal()
{
    UMaterialParameterCollectionInstance* Instance = GetMPC();
    if (!IsValid(Instance))
    {
        return false;
    }
    const float SafeMode = FMath::Clamp(CelShadingMode, 0.0f, 1.0f);
    const bool bModeSet = Instance->SetScalarParameterValue(
        CelShadingModeParameterName,
        SafeMode);

    return bModeSet;
}

bool UMPCComponent::ApplyWeatherInternal()
{
    UMaterialParameterCollectionInstance* Instance = GetMPC();
    if (!IsValid(Instance))
    {
        return false;
    }

    const float SafeMode = FMath::Clamp(WeatherMode, 0.0f, 1.0f);
    const float SafePrecipitation = FMath::Clamp(Precipitation, 0.0f, 1.0f);

    // A zero precipitation value is the authoritative clear state.
    const float EffectivePrecipitation = SafePrecipitation > KINDA_SMALL_NUMBER
        ? SafePrecipitation
        : 0.0f;

    const bool bModeSet = Instance->SetScalarParameterValue(
        WeatherModeParameterName,
        SafeMode);

    const bool bPrecipitationSet = Instance->SetScalarParameterValue(
        PrecipitationParameterName,
        EffectivePrecipitation);

    return bModeSet && bPrecipitationSet;
}