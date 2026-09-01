#include "Weather/WeatherRuntimeActor.h"

#include "Components/PostProcessComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Weather/WeatherRuntimeSubsystem.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    static const TCHAR* NAME_Intensity = TEXT("Intensity");
    static const TCHAR* NAME_WeatherIntensity = TEXT("WeatherIntensity");
    static const TCHAR* NAME_RainIntensity = TEXT("RainIntensity");
    static const TCHAR* NAME_Precipitation = TEXT("Precipitation");
    static const TCHAR* NAME_WeatherMode = TEXT("WeatherMode");

    static bool IsSnowPreset(const FString& Preset)
    {
        return Preset.Contains(TEXT("Snow"), ESearchCase::IgnoreCase)
            || Preset.Contains(TEXT("Blizzard"), ESearchCase::IgnoreCase)
            || Preset.Contains(TEXT("Sleet"), ESearchCase::IgnoreCase);
    }
}

AWeatherRuntimeActor::AWeatherRuntimeActor()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickInterval = 0.0f;

    SceneCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("RainCapture"));
    SetRootComponent(SceneCapture);
    SceneCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
    SceneCapture->CaptureSource = ESceneCaptureSource::SCS_DeviceDepth;
    SceneCapture->bCaptureEveryFrame = true;
    SceneCapture->bCaptureOnMovement = true;
    SceneCapture->ProjectionType = ECameraProjectionMode::Orthographic;
    SceneCapture->OrthoWidth = CaptureOrthoWidth;
    SceneCapture->bAutoCalculateOrthoPlanes = false;
    SceneCapture->bUpdateOrthoPlanes = true;
    SceneCapture->CustomNearClippingPlane = 50.0f;
    SceneCapture->MaxViewDistanceOverride = CaptureMaxViewDistance;
    SceneCapture->SetRelativeRotation(FRotator(90.0f, 0.0f, 0.0f));

    Niagara = CreateDefaultSubobject<UNiagaraComponent>(TEXT("RainNiagara"));
    Niagara->SetupAttachment(SceneCapture);
    Niagara->SetRelativeRotation(FRotator(-90.0f, 0.0f, 0.0f));

    PostProcess = CreateDefaultSubobject<UPostProcessComponent>(TEXT("WeatherPostProcess"));
    PostProcess->SetupAttachment(SceneCapture);
    PostProcess->bUnbound = true;
    PostProcess->bEnabled = true;
    PostProcess->BlendWeight = 1.0f;
}

void AWeatherRuntimeActor::BeginPlay()
{
    Super::BeginPlay();
    SceneCapture->OrthoWidth = CaptureOrthoWidth;
    SceneCapture->MaxViewDistanceOverride = CaptureMaxViewDistance;
    UpdateRainCaptureParameters();
    UpdateWeatherPostProcess();
}

void AWeatherRuntimeActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    if (USceneComponent* Camera = WeatherCamera.Get())
    {
        SetActorLocation(
            Camera->GetComponentLocation() + FVector(0.0f, 0.0f, CaptureHeightOffset),
            false,
            nullptr,
            ETeleportType::TeleportPhysics);
    }

    const bool bSnow = IsSnowPreset(WeatherPreset);
    const float Target = WeatherIntensity;
    if (bSnow)
    {
        if (Target > 0.001f)
        {
            SurfaceCoverage = FMath::FInterpTo(
                SurfaceCoverage, Target, DeltaSeconds, SnowAccumulationRate * 6.0f);
        }
        else
        {
            SurfaceCoverage = FMath::Max(0.0f, SurfaceCoverage - SnowMeltRate * DeltaSeconds);
        }
    }
    else if (Target > 0.001f)
    {
        SurfaceCoverage = FMath::FInterpTo(
            SurfaceCoverage, Target, DeltaSeconds, WetnessRiseRate);
    }
    else
    {
        SurfaceCoverage = FMath::Max(0.0f, SurfaceCoverage - WetnessDryRate * DeltaSeconds);
    }

    UpdateRainCaptureParameters();
    UpdateWeatherPostProcess();
}

void AWeatherRuntimeActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Super::EndPlay(EndPlayReason);
}


void AWeatherRuntimeActor::ConfigureWeather(const FString& InPreset, float InIntensity, USceneComponent* InCamera)
{
    WeatherPreset = InPreset.IsEmpty() ? TEXT("Rain") : InPreset;
    WeatherIntensity = FMath::Clamp(InIntensity, 0.0f, 1.0f);
    SetWeatherCamera(InCamera);

    if (IsValid(Niagara))
    {
        Niagara->SetFloatParameter(NAME_Intensity, WeatherIntensity);
        Niagara->SetFloatParameter(NAME_WeatherIntensity, WeatherIntensity);
        const bool bSnow = IsSnowPreset(WeatherPreset);
        Niagara->SetVisibility(!bSnow && WeatherIntensity > 0.001f, true);
        Niagara->SetFloatParameter(NAME_RainIntensity, bSnow ? 0.0f : WeatherIntensity);
    }

    UpdateWeatherPostProcess();
}

void AWeatherRuntimeActor::SetWeatherCamera(USceneComponent* InCamera)
{
    WeatherCamera = InCamera;
    if (USceneComponent* Camera = WeatherCamera.Get())
    {
        SetActorLocation(Camera->GetComponentLocation() + FVector(0.0f, 0.0f, CaptureHeightOffset), false, nullptr, ETeleportType::TeleportPhysics);
    }
}

void AWeatherRuntimeActor::UpdateRainCaptureParameters()
{
    if (!IsValid(SceneCapture) || !IsValid(Niagara))
    {
        return;
    }

    const FTransform CaptureTransform = SceneCapture->GetComponentTransform();
    const FVector Location = CaptureTransform.GetLocation();
    const FQuat Rotation = CaptureTransform.GetRotation();
    const FVector Forward = Rotation.GetForwardVector();
    const FVector Right = Rotation.GetRightVector();
    const FVector Up = Rotation.GetUpVector();
    const float MaxView = FMath::Max(0.0f, CaptureMaxViewDistance);
    const FPlane XPlane(Location.X, Location.Y, Location.Z, SceneCapture->OrthoWidth);
    const FPlane YPlane(Right.X, Right.Y, Right.Z, 1.0f);
    const FPlane ZPlane(Up.X, Up.Y, Up.Z, SceneCapture->CustomNearClippingPlane);
    const FPlane WPlane(Forward.X, Forward.Y, Forward.Z, MaxView);
    Niagara->SetFloatParameter(TEXT("MaxViewDistanceOverride"), MaxView);
    Niagara->SetVariableMatrix(TEXT("Matrix"), FMatrix(XPlane, YPlane, ZPlane, WPlane));
}

void AWeatherRuntimeActor::UpdateWeatherPostProcess()
{
    if (!IsValid(WeatherPostProcessMID))
    {
        return;
    }

    const bool bSnow = IsSnowPreset(WeatherPreset);
    WeatherPostProcessMID->SetScalarParameterValue(NAME_Precipitation, FMath::Clamp(SurfaceCoverage, 0.0f, 1.0f));
    WeatherPostProcessMID->SetScalarParameterValue(TEXT("Wetness"), bSnow ? 0.0f : SurfaceCoverage);
    WeatherPostProcessMID->SetScalarParameterValue(TEXT("SnowAccumulation"), bSnow ? SurfaceCoverage : 0.0f);
    WeatherPostProcessMID->SetScalarParameterValue(TEXT("WeatherIntensity"), WeatherIntensity);

    if (UWorld* World = GetWorld())
    {
        if (UGameInstance* GameInstance = World->GetGameInstance())
        {
            if (UMaterialParameterCollection* MPC = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/ShaderLibrary/PostProcess/MPC_PostProcess.MPC_PostProcess")))
            {
                UKismetMaterialLibrary::SetScalarParameterValue(
                    GameInstance->GetWorld(),
                    MPC,
                    NAME_WeatherMode,
                    bSnow ? 1.0f : 0.0f);
            }
        }
    }
}
