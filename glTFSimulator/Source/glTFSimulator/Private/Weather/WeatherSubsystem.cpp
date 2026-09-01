// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Weather/WeatherSubsystem.h"

#include "Components/SceneComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Weather/WeatherEffectInterface.h"
#include "World/WorldData.h"

namespace
{
    constexpr float MinWeatherTickIntervalSeconds = 0.05f;
    constexpr float MaxWeatherTickIntervalSeconds = 3600.0f;
}

void UWeatherSubsystem::Deinitialize()
{
    StopWeather();
    WeatherActorClass = nullptr;
    WorldData = nullptr;
    WeatherCamera.Reset();
    Super::Deinitialize();
}

void UWeatherSubsystem::ConfigureWeatherActorClass(TSubclassOf<AActor> InWeatherActorClass)
{
    check(IsInGameThread());

    if (WeatherActorClass == InWeatherActorClass)
    {
        return;
    }

    // Do not leave an instance of an old Blueprint class alive after an editor configuration change.
    DestroyEffectActor();
    WeatherActorClass = InWeatherActorClass;
    ApplyEffectActorState();
}

void UWeatherSubsystem::ConfigureFromWorldData(UWorldData* InWorldData)
{
    check(IsInGameThread());

    WorldData = InWorldData;
    if (!IsValid(WorldData))
    {
        StopWeather();
        return;
    }

    const FLevelWeatherSettings& Settings = WorldData->Weather;
    bCommandOverrideActive = false;
    ApplyWeather(Settings.Preset, Settings.Intensity, Settings.bEnabled, -1.0f);
}

void UWeatherSubsystem::SetWeatherCamera(USceneComponent* InCamera)
{
    check(IsInGameThread());

    WeatherCamera = InCamera;
    ApplyEffectActorState();
}

bool UWeatherSubsystem::ApplyWeather(
    const FString& Preset,
    const float Intensity,
    const bool bEnabled,
    const float DurationSeconds)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("UWeatherSubsystem::ApplyWeather must run on the game thread")))
    {
        return false;
    }

    const FString NormalizedPreset = NormalizePreset(Preset);
    if (NormalizedPreset.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("Unknown weather preset '%s'. Expected clear, rain, or snow."), *Preset);
        return false;
    }

    bWeatherSystemEnabled = bEnabled;
    CurrentPreset = bEnabled ? NormalizedPreset : FString(TEXT("clear"));
    CurrentIntensity = FMath::Clamp(FMath::IsFinite(Intensity) ? Intensity : 1.0f, 0.0f, 10.0f);

    if (DurationSeconds >= 0.0f)
    {
        const float TickInterval = GetTickIntervalSeconds();
        RemainingWeatherTicks = FMath::Max(1, FMath::CeilToInt(DurationSeconds / TickInterval));
        bCommandOverrideActive = true;
    }
    else
    {
        RemainingWeatherTicks = ResolveRandomDurationTicks();
    }

    ApplyEffectActorState();
    RestartTickTimer();
    return true;
}

void UWeatherSubsystem::StopWeather()
{
    if (!IsInGameThread())
    {
        return;
    }

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(WeatherTickHandle);
    }

    DestroyEffectActor();
    bWeatherSystemEnabled = false;
    bCommandOverrideActive = false;
    CurrentPreset = TEXT("clear");
    RemainingWeatherTicks = 0;
    WorldData = nullptr;
}

void UWeatherSubsystem::RestartTickTimer()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    World->GetTimerManager().ClearTimer(WeatherTickHandle);
    if (!bWeatherSystemEnabled || !IsValid(WorldData))
    {
        return;
    }

    const float Interval = GetTickIntervalSeconds();
    World->GetTimerManager().SetTimer(
        WeatherTickHandle,
        this,
        &UWeatherSubsystem::WeatherTick,
        Interval,
        true,
        Interval);
}

void UWeatherSubsystem::WeatherTick()
{
    check(IsInGameThread());

    if (!bWeatherSystemEnabled || !IsValid(WorldData))
    {
        return;
    }

    if (RemainingWeatherTicks > 0)
    {
        --RemainingWeatherTicks;
    }

    if (RemainingWeatherTicks > 0)
    {
        return;
    }

    if (bCommandOverrideActive)
    {
        // A timed console/chat override expires back into the map-authored weather simulation.
        bCommandOverrideActive = false;
        if (!WorldData->Weather.bEnabled)
        {
            bWeatherSystemEnabled = false;
            CurrentPreset = TEXT("clear");
            DestroyEffectActor();
            if (UWorld* World = GetWorld())
            {
                World->GetTimerManager().ClearTimer(WeatherTickHandle);
            }
            return;
        }
    }

    if (WorldData->Weather.bAutoCycle)
    {
        ChooseNextAutomaticWeather();
    }
    else
    {
        CurrentPreset = NormalizePreset(WorldData->Weather.Preset);
        if (CurrentPreset.IsEmpty())
        {
            CurrentPreset = TEXT("clear");
        }
        RemainingWeatherTicks = ResolveRandomDurationTicks();
        ApplyEffectActorState();
    }
}

void UWeatherSubsystem::ChooseNextAutomaticWeather()
{
    const float RainWeight = FMath::Max(0.0f, WorldData->Weather.RainWeight);
    const float SnowWeight = FMath::Max(0.0f, WorldData->Weather.SnowWeight);
    const float ClearWeight = FMath::Max(0.0f, WorldData->Weather.ClearWeight);
    const float TotalWeight = RainWeight + SnowWeight + ClearWeight;

    FString NextPreset(TEXT("clear"));
    if (TotalWeight > UE_SMALL_NUMBER)
    {
        const float Pick = FMath::FRandRange(0.0f, TotalWeight);
        if (Pick < RainWeight)
        {
            NextPreset = TEXT("rain");
        }
        else if (Pick < RainWeight + SnowWeight)
        {
            NextPreset = TEXT("snow");
        }
    }

    CurrentPreset = MoveTemp(NextPreset);
    CurrentIntensity = FMath::Clamp(WorldData->Weather.Intensity, 0.0f, 10.0f);
    RemainingWeatherTicks = ResolveRandomDurationTicks();
    ApplyEffectActorState();
}

void UWeatherSubsystem::ApplyEffectActorState()
{
    check(IsInGameThread());

    const bool bNeedsEffectActor = bWeatherSystemEnabled && CurrentPreset != TEXT("clear");
    if (!bNeedsEffectActor)
    {
        DestroyEffectActor();
        return;
    }

    UWorld* World = GetWorld();
    if (!World || !WeatherActorClass)
    {
        return;
    }

    AActor* EffectActor = ActiveWeatherActor.Get();
    if (!IsValid(EffectActor) || EffectActor->GetClass() != WeatherActorClass.Get())
    {
        DestroyEffectActor();

        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Params.ObjectFlags |= RF_Transient;
        EffectActor = World->SpawnActor<AActor>(WeatherActorClass, FTransform::Identity, Params);
        ActiveWeatherActor = EffectActor;
    }

    if (!IsValid(EffectActor))
    {
        return;
    }

    USceneComponent* Camera = WeatherCamera.Get();
    if (IsValid(Camera))
    {
        EffectActor->AttachToComponent(Camera, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
        EffectActor->SetActorRelativeLocation(FVector::ZeroVector);
    }

    if (EffectActor->GetClass()->ImplementsInterface(UWeatherEffectInterface::StaticClass()))
    {
        IWeatherEffectInterface::Execute_SetWeatherCamera(EffectActor, Camera);
        IWeatherEffectInterface::Execute_SetWeatherPreset(EffectActor, CurrentPreset);
        IWeatherEffectInterface::Execute_SetWeatherIntensity(EffectActor, CurrentIntensity);
        IWeatherEffectInterface::Execute_SetWeatherActive(EffectActor, true);
    }

    EffectActor->SetActorHiddenInGame(false);
    EffectActor->SetActorTickEnabled(true);
}

void UWeatherSubsystem::DestroyEffectActor()
{
    AActor* EffectActor = ActiveWeatherActor.Get();
    if (!IsValid(EffectActor))
    {
        ActiveWeatherActor.Reset();
        return;
    }

    if (EffectActor->GetClass()->ImplementsInterface(UWeatherEffectInterface::StaticClass()))
    {
        IWeatherEffectInterface::Execute_SetWeatherActive(EffectActor, false);
    }

    EffectActor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    EffectActor->Destroy();
    ActiveWeatherActor.Reset();
}

int32 UWeatherSubsystem::ResolveRandomDurationTicks() const
{
    if (!IsValid(WorldData))
    {
        return 1;
    }

    const int32 MinTicks = FMath::Max(1, WorldData->Weather.MinDurationTicks);
    const int32 MaxTicks = FMath::Max(MinTicks, WorldData->Weather.MaxDurationTicks);
    return FMath::RandRange(MinTicks, MaxTicks);
}

float UWeatherSubsystem::GetTickIntervalSeconds() const
{
    const float Requested = IsValid(WorldData) ? WorldData->Weather.TickIntervalSeconds : 1.0f;
    return FMath::Clamp(
        FMath::IsFinite(Requested) ? Requested : 1.0f,
        MinWeatherTickIntervalSeconds,
        MaxWeatherTickIntervalSeconds);
}

FString UWeatherSubsystem::NormalizePreset(const FString& Preset)
{
    FString Value = Preset.TrimStartAndEnd().ToLower();
    if (Value == TEXT("clear") || Value == TEXT("rain") || Value == TEXT("snow"))
    {
        return Value;
    }
    return FString();
}
