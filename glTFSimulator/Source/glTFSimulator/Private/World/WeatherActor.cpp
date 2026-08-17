// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "World/WeatherActor.h"
#include "System/GameManagerSubSystem.h"
#include "System/GameUpdateSubSystem.h"
#include "Engine/EngineTypes.h"
#include "NiagaraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Kismet/KismetSystemLibrary.h"
#include "World/UpdateRainAsync.h" // Header for the async rain update action.

AWeatherActor::AWeatherActor()
{
    PrimaryActorTick.bCanEverTick = false;
    MaxDistance = 1048576.0f;
    Param = FName("Matrix");
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SceneCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCapture"));
    SceneCapture->SetupAttachment(RootComponent);
    SceneCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
    SceneCapture->CaptureSource = ESceneCaptureSource::SCS_DeviceDepth;
    SceneCapture->MaxViewDistanceOverride = 10000.0f;
    SceneCapture->ProjectionType = ECameraProjectionMode::Orthographic;
    SceneCapture->OrthoWidth = 12100.0f;
    SceneCapture->bAutoCalculateOrthoPlanes = false;
    SceneCapture->bUpdateOrthoPlanes = true;
    Niagara = CreateDefaultSubobject<UNiagaraComponent>(TEXT("Niagara"));
    Niagara->SetupAttachment(RootComponent);
}

void AWeatherActor::ConfigureWeather(const FString& InPreset, float InIntensity)
{
    WeatherPreset = InPreset.IsEmpty() ? FString(TEXT("Rain")) : InPreset;
    WeatherIntensity = FMath::Max(0.0f, InIntensity);

    if (IsValid(Niagara))
    {
        // These parameter names are intentionally generic so Blueprint/Niagara variants can opt in.
        Niagara->SetFloatParameter(TEXT("Intensity"), WeatherIntensity);
        Niagara->SetFloatParameter(TEXT("WeatherIntensity"), WeatherIntensity);
        Niagara->SetFloatParameter(TEXT("RainIntensity"), WeatherIntensity);
    }
}

void AWeatherActor::BeginPlay()
{
    // 1. Call the parent BeginPlay implementation.
    Super::BeginPlay();
    Param = FName("Matrix");
    MaxDistance = 1048576.0f;
    // 2. Set LocationOffset to (0, 0, 1500).
    LocationOffset = FVector(0.0f, 0.0f, 1500.0f);
    if (!IsValid(SubSystem))
    {
        SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    }
    ConfigureWeather(WeatherPreset, WeatherIntensity);
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [WeakThis = TWeakObjectPtr<AWeatherActor>(this)](const float DeltaSeconds)
            {
                if (AWeatherActor* StrongThis = WeakThis.Get())
                {
                    StrongThis->UpdateFromGameUpdate(DeltaSeconds);
                }
            },
            30);
    }
}

void AWeatherActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
    }
    GameUpdateTickHandle = INDEX_NONE;

    Super::EndPlay(EndPlayReason);
}


void AWeatherActor::UpdateFromGameUpdate(float DeltaSeconds)
{
    if (IsValid(SubSystem))
    {
        Location = SubSystem->GetCameraLocation();
        SetActorLocation(Location + LocationOffset, false, nullptr, ETeleportType::TeleportPhysics);
    }

    UpdateRainParameters();
}

void AWeatherActor::UpdateRainParameters()
{
    // Native updates avoid allocating and binding a transient Blueprint action every frame.
    UUpdateRainAsync::ApplyRainParameters(
        SceneCapture,
        Niagara,
        MaxDistance,
        Param);
}
