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
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [this](const float DeltaSeconds)
            {
                TickFromGameUpdate(DeltaSeconds);
            },
            30);
    }

    // 3. Start the async tick loop.
    AsyncTick();
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

void AWeatherActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    TickFromGameUpdate(DeltaSeconds);
}

void AWeatherActor::TickFromGameUpdate(float DeltaSeconds)
{
    if (IsValid(SubSystem))
    {
        Location = SubSystem->GetCameraLocation();
        SetActorLocation(Location + LocationOffset, false, nullptr, ETeleportType::TeleportPhysics);
    }
}

void AWeatherActor::AsyncTick()
{
    // Logic equivalent to calling the Blueprint UpdateRainAsync node.
    // UBlueprintAsyncActionBase objects are usually created through the static UpdateRainAsync function.
    UUpdateRainAsync* RainAction = UUpdateRainAsync::UpdateRainAsync(
        this, 
        SceneCapture, 
        Niagara, 
        MaxDistance, 
        Param
    );

    if (RainAction)
    {
        // Bind the Blueprint Completed pin to the delegate.
        RainAction->Completed.AddDynamic(this, &AWeatherActor::OnRainUpdateCompleted);
        // Activate the async action.
        RainAction->Activate();
    }
}

void AWeatherActor::OnRainUpdateCompleted()
{
    // Implements the loop that calls Async Tick again after Blueprint DelayUntilNextTick.
    // In C++, schedule the next call for the following frame or use a timer.
    GetWorld()->GetTimerManager().SetTimerForNextTick(this, &AWeatherActor::AsyncTick);
}