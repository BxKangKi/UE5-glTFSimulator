#include "Weather/WeatherRuntimeSubsystem.h"

#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Weather/WeatherRuntimeActor.h"

void UWeatherRuntimeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddUObject(
        this,
        &UWeatherRuntimeSubsystem::HandleWorldCleanup);
}

void UWeatherRuntimeSubsystem::Deinitialize()
{
    DestroyActor();
    WeatherCamera.Reset();
    if (WorldCleanupHandle.IsValid())
    {
        FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
        WorldCleanupHandle = FDelegateHandle();
    }
    Super::Deinitialize();
}

void UWeatherRuntimeSubsystem::SetWeatherCamera(USceneComponent* InCamera)
{
    WeatherCamera = InCamera;
    if (IsValid(ActiveWeatherActor))
    {
        ActiveWeatherActor->SetWeatherCamera(InCamera);
    }
}

void UWeatherRuntimeSubsystem::ApplyWeather(const FString& InPreset, float InIntensity, bool bEnabled)
{
    PendingPreset = InPreset.IsEmpty() ? TEXT("Rain") : InPreset;
    PendingIntensity = FMath::Clamp(InIntensity, 0.0f, 1.0f);
    bPendingEnabled = bEnabled && PendingIntensity > 0.001f;

    EnsureActorForCurrentWorld();
    if (!bPendingEnabled)
    {
        DestroyActor();
        return;
    }

    if (IsValid(ActiveWeatherActor))
    {
        ActiveWeatherActor->ConfigureWeather(PendingPreset, PendingIntensity, WeatherCamera.Get());
    }
}

void UWeatherRuntimeSubsystem::ClearWeather()
{
    bPendingEnabled = false;
    PendingIntensity = 0.0f;
    DestroyActor();
}

void UWeatherRuntimeSubsystem::EnsureActorForCurrentWorld()
{
    UWorld* World = GetWorld();
    if (!IsValid(World))
    {
        DestroyActor();
        return;
    }

    if (ActiveWorld.Get() != World || !IsValid(ActiveWeatherActor))
    {
        DestroyActor();
        ActiveWorld = World;
    }

    if (bPendingEnabled && !IsValid(ActiveWeatherActor))
    {
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        ActiveWeatherActor = World->SpawnActor<AWeatherRuntimeActor>(
            AWeatherRuntimeActor::StaticClass(),
            FTransform::Identity,
            Params);

        if (IsValid(ActiveWeatherActor))
        {
            ActiveWeatherActor->SetWeatherCamera(WeatherCamera.Get());
            ActiveWeatherActor->ConfigureWeather(PendingPreset, PendingIntensity, WeatherCamera.Get());
        }
    }
}

void UWeatherRuntimeSubsystem::DestroyActor()
{
    if (IsValid(ActiveWeatherActor))
    {
        ActiveWeatherActor->Destroy();
        ActiveWeatherActor = nullptr;
    }
    ActiveWorld.Reset();
}

void UWeatherRuntimeSubsystem::HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
    if (ActiveWorld.Get() == World)
    {
        DestroyActor();
    }
}
