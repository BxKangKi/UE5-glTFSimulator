// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "World/WeatherActor.h"
#include "System/GameManagerSubSystem.h"
#include "Engine/EngineTypes.h"
#include "NiagaraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Kismet/KismetSystemLibrary.h"
#include "World/UpdateRainAsync.h" // 해당 비동기 액션 헤더

AWeatherActor::AWeatherActor()
{
    PrimaryActorTick.bCanEverTick = true;
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
    // 1. OnBeginPlay (상위 클래스 함수 호출)
    Super::BeginPlay();
    Param = FName("Matrix");
    MaxDistance = 1048576.0f;
    // 2. LocationOffset 설정 (0, 0, 1500)
    LocationOffset = FVector(0.0f, 0.0f, 1500.0f);
    if (!IsValid(SubSystem))
    {
        SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    }
    // 3. Async Tick 시작
    AsyncTick();
}

void AWeatherActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (IsValid(SubSystem))
    {
        Location = SubSystem->GetCameraLocation();
        SetActorLocation(Location + LocationOffset, false, nullptr, ETeleportType::TeleportPhysics);
    }
}

void AWeatherActor::AsyncTick()
{
    // 블루프린트의 UpdateRainAsync 노드 호출 로직
    // 보통 UBlueprintAsyncActionBase는 Static 함수인 "UpdateRainAsync"를 통해 생성됩니다.
    UUpdateRainAsync* RainAction = UUpdateRainAsync::UpdateRainAsync(
        this, 
        SceneCapture, 
        Niagara, 
        MaxDistance, 
        Param
    );

    if (RainAction)
    {
        // 블루프린트의 "Completed" 핀을 델리게이트로 연결
        RainAction->Completed.AddDynamic(this, &AWeatherActor::OnRainUpdateCompleted);
        // 액션 활성화
        RainAction->Activate();
    }
}

void AWeatherActor::OnRainUpdateCompleted()
{
    // 블루프린트의 "DelayUntilNextTick" 후 다시 "Async Tick" 호출하는 루프 구현
    // C++에서는 다음 프레임에 함수를 예약 실행하거나 Timer를 사용합니다.
    GetWorld()->GetTimerManager().SetTimerForNextTick(this, &AWeatherActor::AsyncTick);
}