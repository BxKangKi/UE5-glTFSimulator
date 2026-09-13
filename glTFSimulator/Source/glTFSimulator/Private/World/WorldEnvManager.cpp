// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file WorldEnvManager.cpp
 * 역할: 월드 하늘·안개·구름·조명을 렌더링합니다.
 * 핵심 기능: 환경 컴포넌트 초기화, 로딩 중 환경 준비, 시간·날씨 반영.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "World/WorldEnvManager.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Setting/GameSettings.h"
#include "System/GameManagerSubSystem.h"
#include "System/GlTFSimulatorGameInstance.h"
#include "System/GlTFSimulatorAssetRegistry.h"
#include "System/GameUpdateSubSystem.h"
#include "System/MathHelper.h"
#include "TimerManager.h"
#include "World/WorldData.h"

namespace WorldEnvManagerPrivate
{
    struct FSkyLightRotations
    {
        FRotator Sun = FRotator::ZeroRotator;
        FRotator Moon = FRotator::ZeroRotator;
    };

    /**
     * Calculates a guarded solar direction directly from immutable values already read on the
     * game thread. The former Blueprint async-action wrapper allocated a UObject for a few
     * trigonometric operations and was the final legacy owner of this calculation.
     */
    FRotator CalculateSunRotation(const UWorldData* WorldData)
    {
        if (!IsValid(WorldData)
            || !FMath::IsFinite(WorldData->WorldTime)
            || !FMath::IsFinite(WorldData->OneDayTime)
            || !FMath::IsFinite(WorldData->OneYearDays)
            || !FMath::IsFinite(WorldData->AxialTilt)
            || !FMath::IsFinite(WorldData->Latitude)
            || !FMath::IsFinite(WorldData->Longitude)
            || WorldData->OneDayTime <= UE_SMALL_NUMBER
            || WorldData->OneYearDays <= UE_SMALL_NUMBER)
        {
            return FRotator(-35.0f, -30.0f, 0.0f);
        }

        float TotalSeconds = FMath::Fmod(
            WorldData->WorldTime, WorldData->OneDayTime);
        if (TotalSeconds < 0.0f)
        {
            TotalSeconds += WorldData->OneDayTime;
        }

        double DayOfYear = FMath::Fmod(
            static_cast<double>(WorldData->WorldTime)
                / static_cast<double>(WorldData->OneDayTime),
            static_cast<double>(WorldData->OneYearDays));
        if (DayOfYear < 0.0)
        {
            DayOfYear += WorldData->OneYearDays;
        }
        const float AxialTiltRadians = Deg2Rad * WorldData->AxialTilt;
        const float LatitudeRadians = Deg2Rad * WorldData->Latitude;
        const float SunDeclination = AxialTiltRadians * FMath::Sin(
            2.0 * __PI__ * (DayOfYear / WorldData->OneYearDays));
        const float TimeInHours =
            (TotalSeconds / WorldData->OneDayTime) * 24.0f;
        // WorldTime is the world's local solar clock. Longitude rotates the compass orientation
        // below; multiplying longitude by 15 here previously pushed the default sun below the
        // horizon even at noon and made a healthy procedural atmosphere look unrendered.
        const float HourAngle = 15.0f * (TimeInHours - 12.0f);
        const float HourAngleRadians = Deg2Rad * HourAngle;

        const float Altitude = FMath::Asin(FMath::Clamp(
            FMath::Sin(LatitudeRadians) * FMath::Sin(SunDeclination)
                + FMath::Cos(LatitudeRadians) * FMath::Cos(SunDeclination)
                    * FMath::Cos(HourAngleRadians),
            -1.0f,
            1.0f));
        const float Azimuth = FMath::Atan2(
            -FMath::Cos(SunDeclination) * FMath::Sin(HourAngleRadians),
            FMath::Cos(LatitudeRadians) * FMath::Sin(SunDeclination)
                - FMath::Sin(LatitudeRadians) * FMath::Cos(SunDeclination)
                    * FMath::Cos(HourAngleRadians));
        const float OrientedAzimuth = Azimuth + Deg2Rad * WorldData->Longitude;
        const FVector Direction(
            FMath::Cos(Altitude) * FMath::Cos(OrientedAzimuth),
            FMath::Cos(Altitude) * FMath::Sin(OrientedAzimuth),
            FMath::Sin(Altitude));
        if (Direction.IsNearlyZero())
        {
            return FRotator(-35.0f, -30.0f, 0.0f);
        }

        FRotator Rotation =
            FRotationMatrix::MakeFromX(Direction.GetSafeNormal()).Rotator();
        Rotation.Normalize();
        return Rotation;
    }

    FSkyLightRotations CalculateLightRotations(const UWorldData* WorldData)
    {
        FSkyLightRotations Result;
        Result.Sun = CalculateSunRotation(WorldData);
        Result.Moon = FRotator(
            -Result.Sun.Pitch,
            Result.Sun.Yaw + 180.0f,
            -Result.Sun.Roll);
        Result.Moon.Normalize();
        return Result;
    }
}

AWorldEnvManager::AWorldEnvManager()
{
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    RootComponent->SetMobility(EComponentMobility::Movable);

    Sun = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Sun"));
    Sun->SetupAttachment(RootComponent);
    Sun->SetMobility(EComponentMobility::Movable);
    // SkyAtmosphere ignores an ordinary directional light when generating the sun disk and
    // aerial perspective. Explicitly reserve atmosphere-light slot zero for the sun.
    Sun->SetAtmosphereSunLight(true);
    Sun->SetAtmosphereSunLightIndex(0);
    Sun->bUseTemperature = true;
    Sun->SetEnableLightShaftOcclusion(true);
    Sun->SetEnableLightShaftBloom(true);
    Sun->SetBloomScale(0.0001f);
    Sun->SetIntensity(20.0f);
    Sun->SetLightSourceAngle(0.53f);
    Sun->SetTemperature(5700.0f);
    Sun->SetVolumetricScatteringIntensity(2.5f);
    Sun->bCastShadowsOnClouds = true;
    Sun->bCastShadowsOnAtmosphere = true;
    Sun->SetRelativeRotation(FRotator(-35.0f, -30.0f, 0.0f));

    Moon = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Moon"));
    Moon->SetupAttachment(RootComponent);
    Moon->SetMobility(EComponentMobility::Movable);
    // Slot one lets SkyAtmosphere render a separate moon disk without replacing the sun.
    Moon->SetAtmosphereSunLight(true);
    Moon->SetAtmosphereSunLightIndex(1);
    Moon->SetIntensity(0.005f);
    Moon->SetUseTemperature(true);
    Moon->SetEnableLightShaftOcclusion(true);
    Moon->SetEnableLightShaftBloom(true);
    Moon->SetBloomScale(0.0001f);
    Moon->bCastShadowsOnClouds = true;
    Moon->bCastShadowsOnAtmosphere = true;
    Moon->SetForwardShadingPriority(1);
    Moon->SetRelativeRotation(FRotator(35.0f, 150.0f, 0.0f));

    PostProcess = CreateDefaultSubobject<UPostProcessComponent>(TEXT("PostProcess"));
    PostProcess->SetupAttachment(RootComponent);
    PostProcess->bUnbound = true;

    SkyAtmosphere = CreateDefaultSubobject<USkyAtmosphereComponent>(TEXT("SkyAtmosphere"));
    SkyAtmosphere->SetupAttachment(RootComponent);
    SkyAtmosphere->SetMobility(EComponentMobility::Movable);
    SkyAtmosphere->SetGroundAlbedo(FColor::White);
    SkyAtmosphere->SetAtmosphereHeight(200.0f);
    SkyAtmosphere->SetMultiScatteringFactor(1.0f);
    SkyAtmosphere->SetRayleighScattering(FLinearColor(0.0058f, 0.0180f, 0.0331f));
    SkyAtmosphere->SetRayleighScatteringScale(0.35f);
    SkyAtmosphere->SetRayleighExponentialDistribution (8.0f);
    SkyAtmosphere->SetMieScatteringScale(0.012f);
    SkyAtmosphere->SetMieAbsorption(FLinearColor(0.00044f, 0.00044f, 0.00044f));
    SkyAtmosphere->SetMieAbsorptionScale(0.0004f);
    SkyAtmosphere->SetMieAnisotropy(0.85f);
    SkyAtmosphere->SetSkyLuminanceFactor(FLinearColor(1.0f, 1.0f, 1.0f));

    // Keep the original named component so existing Blueprint defaults retain their SM_Skybox
    // assignment after recompilation. No hard-coded content path is used by native code.
    Skybox = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Skybox"));
    Skybox->SetupAttachment(RootComponent);
    Skybox->SetWorldScale3D(FVector(8192.0f, 8192.0f, 8192.0f));
    Skybox->SetEnableGravity(false);

    SkyLight = CreateDefaultSubobject<USkyLightComponent>(TEXT("SkyLight"));
    SkyLight->SetupAttachment(RootComponent);
    SkyLight->SetMobility(EComponentMobility::Movable);
    SkyLight->SetRealTimeCapture(true);
    SkyLight->SetIntensity(0.5f);
    SkyLight->SetCastDeepShadow(true);
    SkyLight->bTransmission = true;
    SkyLight->bAffectTranslucentLighting = true;
}

void AWorldEnvManager::BeginPlay()
{
    Super::BeginPlay();

    if (UGlTFSimulatorAssetRegistry* Registry = UGlTFSimulatorGameInstance::GetAssetRegistryFromContext(this))
    {
        CloudMaterial = Registry->CloudMaterial.IsNull() ? nullptr : Registry->CloudMaterial.LoadSynchronous();
        if (IsValid(Skybox) && !Registry->SkyboxMesh.IsNull())
        {
            Skybox->SetStaticMesh(Registry->SkyboxMesh.LoadSynchronous());
        }
    }

    SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    PrepareForWorldLoading();
    ConfigureRenderingSettings();

    // Placed WorldEnvManager actors can begin rendering once GameManagerSubSystem has already loaded world data.
    if (UGameManagerSubSystem* GameManager = SubSystem.Get(); IsValid(GameManager) && IsValid(GameManager->GetWorldData()))
    {
        InitializeRendering(GameManager->GetWorldData());
    }
}

void AWorldEnvManager::PrepareForWorldLoading()
{
    if (!ensureMsgf(IsInGameThread(),
            TEXT("AWorldEnvManager::PrepareForWorldLoading must run on the game thread")))
    {
        return;
    }

    // The procedural atmosphere is the loading-state sky as well as the normal runtime sky. It
    // must not wait for config.json or the dynamic .dat state to become available.
    SetActorHiddenInGame(false);
    EnsureCoreSkyVisible();
}

void AWorldEnvManager::EnsureCoreSkyVisible()
{
    const auto EnableSceneComponent = [](USceneComponent* Component)
    {
        if (!IsValid(Component))
        {
            return;
        }

        // Blueprint subclasses may have serialized a disabled/hidden component. Restore the
        // native rendering contract every time this actor is adopted for a new world session.
        Component->SetActive(true, true);
        Component->SetVisibility(true, true);
        Component->SetHiddenInGame(false, true);
        Component->MarkRenderStateDirty();
    };

    EnableSceneComponent(SkyAtmosphere);
    EnableSceneComponent(Skybox);
    EnableSceneComponent(Sun);
    EnableSceneComponent(Moon);
    EnableSceneComponent(SkyLight);
    EnableSceneComponent(PostProcess);

    if (IsValid(Sun))
    {
        Sun->SetAtmosphereSunLight(true);
        Sun->SetAtmosphereSunLightIndex(0);
        if (!bRenderingActive)
        {
            Sun->SetIntensity(20.0f);
            Sun->SetRelativeRotation(FRotator(-35.0f, -30.0f, 0.0f));
        }
    }
    if (IsValid(Moon))
    {
        Moon->SetAtmosphereSunLight(true);
        Moon->SetAtmosphereSunLightIndex(1);
        if (!bRenderingActive)
        {
            Moon->SetIntensity(0.005f);
            Moon->SetRelativeRotation(FRotator(35.0f, 150.0f, 0.0f));
        }
    }
    if (IsValid(SkyLight))
    {
        // A movable real-time skylight captures both the procedural fallback atmosphere and the
        // optional authored SM_Skybox dome without requiring a separate cooked cube map.
        SkyLight->SetRealTimeCapture(true);
        if (!bRenderingActive)
        {
            SkyLight->SetIntensity(0.5f);
        }
    }
}

void AWorldEnvManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopRendering();
    SubSystem.Reset();
    Super::EndPlay(EndPlayReason);
}

void AWorldEnvManager::InitializeRendering(UWorldData* InWorldData)
{
    if (!IsValid(InWorldData))
    {
        StopRendering();
        return;
    }

    Data = InWorldData;
    bRenderingActive = true;
    PrepareForWorldLoading();
    ConfigureRenderingSettings();
    ApplyCloudSettings();
    RegisterGameUpdate();
    UpdateSkyLighting();
}

void AWorldEnvManager::StopRendering()
{
    bRenderingActive = false;
    UnregisterGameUpdate();
    ReleaseDynamicRenderingResources();
    Data = nullptr;
}

void AWorldEnvManager::ConfigureRenderingSettings()
{
    UGameManagerSubSystem* GameManager = SubSystem.Get();
    if (!IsValid(GameManager))
    {
        GameManager = UGameManagerSubSystem::GetSubSystem(this);
        SubSystem = GameManager;
    }

    if (!IsValid(GameManager) || !IsValid(PostProcess))
    {
        return;
    }

    RefreshRuntimeSettings();
    // Apply quality/post-process/streaming settings through the shared manager. The manager's
    // environment refresh calls back into RefreshRuntimeSettings(), which is intentionally a
    // leaf operation and therefore cannot recurse into UpdateSettings().
    GameManager->UpdateSettings();
}

void AWorldEnvManager::RefreshRuntimeSettings()
{
    check(IsInGameThread());
    UGameManagerSubSystem* GameManager = SubSystem.Get();
    if (!IsValid(GameManager))
    {
        GameManager = UGameManagerSubSystem::GetSubSystem(this);
        SubSystem = GameManager;
    }
    if (!IsValid(GameManager))
    {
        return;
    }

    UGameSettings* Setting = GameManager->GetGameSettings();
    if (!IsValid(Setting))
    {
        return;
    }

    if (Setting->bHeightFog)
    {
        if (!IsValid(Fog))
        {
            Fog = NewObject<UExponentialHeightFogComponent>(this, TEXT("RuntimeHeightFog"));
            if (IsValid(Fog))
            {
                AddInstanceComponent(Fog);
                Fog->SetVolumetricFog(true);
                Fog->SetVolumetricFogScatteringDistribution(0.25f);
                Fog->SetVolumetricFogExtinctionScale(1.2f);
                Fog->SetupAttachment(GetRootComponent());
                Fog->SetFogDensity(0.02f);
                Fog->SetFogHeightFalloff(0.2f);
                Fog->SetSecondFogDensity(0.0f);
                Fog->RegisterComponent();
            }
        }
    }
    else
    {
        DestroyFogComponent();
    }

    const bool bLevelAllowsCloud = IsValid(Data) && Data->Cloud.bEnabled;
    if (Setting->bCloud && bLevelAllowsCloud)
    {
        if (!IsValid(Cloud))
        {
            Cloud = NewObject<UVolumetricCloudComponent>(this, TEXT("RuntimeVolumetricCloud"));
            if (IsValid(Cloud))
            {
                AddInstanceComponent(Cloud);
                Cloud->SetupAttachment(GetRootComponent());
                Cloud->RegisterComponent();
            }
        }
    }
    else
    {
        DestroyCloudComponent();
    }

    ApplyCloudSettings();
}

void AWorldEnvManager::ApplyCloudSettings()
{
    if (!IsValid(Data))
    {
        return;
    }

    UGameSettings* Setting = nullptr;
    if (UGameManagerSubSystem* GameManager = SubSystem.Get())
    {
        Setting = GameManager->GetGameSettings();
    }

    const bool bCloudEnabled = Data->Cloud.bEnabled && (!IsValid(Setting) || Setting->bCloud);
    if (!bCloudEnabled)
    {
        DestroyCloudComponent();
        return;
    }

    if (!IsValid(Cloud))
    {
        Cloud = NewObject<UVolumetricCloudComponent>(this, TEXT("RuntimeVolumetricCloud"));
        if (!IsValid(Cloud))
        {
            return;
        }

        AddInstanceComponent(Cloud);
        Cloud->SetupAttachment(GetRootComponent());
        Cloud->RegisterComponent();
    }

    if (!IsValid(CloudMID))
    {
        UMaterialInterface* SourceMaterial = CloudMaterial.Get();
        if (!IsValid(SourceMaterial))
        {
            SourceMaterial = Cloud->GetMaterial();
        }
        if (IsValid(SourceMaterial))
        {
            CloudMID = UMaterialInstanceDynamic::Create(SourceMaterial, this);
            if (IsValid(CloudMID))
            {
                Cloud->SetMaterial(CloudMID);
            }
        }
    }

    if (IsValid(CloudMID))
    {
        // The parameter names are intentionally generic so different cloud materials can opt in.
        CloudMID->SetScalarParameterValue(TEXT("Coverage"), Data->Cloud.Coverage);
        CloudMID->SetScalarParameterValue(TEXT("Density"), Data->Cloud.Density);
        CloudMID->SetScalarParameterValue(TEXT("Opacity"), Data->Cloud.Opacity);
        CloudMID->SetScalarParameterValue(TEXT("WindSpeed"), Data->Cloud.WindSpeed);
        CloudMID->SetVectorParameterValue(TEXT("Tint"), Data->Cloud.Tint);
    }
}

void AWorldEnvManager::DestroyCloudComponent()
{
    // Runtime-created components are stored in the actor's InstanceComponents array. Remove the
    // ownership entry before destruction so a stopped/placed manager cannot retain a dead component
    // or its material graph until the actor itself is collected.
    if (IsValid(Cloud))
    {
        Cloud->SetMaterial(nullptr);
        RemoveInstanceComponent(Cloud);
        Cloud->UnregisterComponent();
        Cloud->DestroyComponent();
    }

    Cloud = nullptr;
    CloudMID = nullptr;
}

void AWorldEnvManager::DestroyFogComponent()
{
    if (IsValid(Fog))
    {
        RemoveInstanceComponent(Fog);
        Fog->UnregisterComponent();
        Fog->DestroyComponent();
    }

    Fog = nullptr;
}

void AWorldEnvManager::ReleaseDynamicRenderingResources()
{
    DestroyCloudComponent();
    DestroyFogComponent();
}

void AWorldEnvManager::RegisterGameUpdate()
{
    if (GameUpdateTickHandle != INDEX_NONE)
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        TWeakObjectPtr<AWorldEnvManager> WeakThis(this);
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [WeakThis](const float DeltaSeconds)
            {
                if (AWorldEnvManager* StrongThis = WeakThis.Get())
                {
                    StrongThis->UpdateFromGameUpdate(DeltaSeconds);
                }
            },
            35);
    }
}

void AWorldEnvManager::UnregisterGameUpdate()
{
    if (GameUpdateTickHandle == INDEX_NONE)
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
    }
    GameUpdateTickHandle = INDEX_NONE;
}

void AWorldEnvManager::UpdateFromGameUpdate(float /*DeltaSeconds*/)
{
    // Cloud settings are applied when rendering/settings change, not redundantly every frame.
    UpdateSkyLighting();
}

void AWorldEnvManager::UpdateSkyLighting()
{
    if (!bRenderingActive || !IsValid(Data))
    {
        return;
    }

    const WorldEnvManagerPrivate::FSkyLightRotations Result =
        WorldEnvManagerPrivate::CalculateLightRotations(Data);

    if (IsValid(Sun))
    {
        Sun->SetWorldRotation(Result.Sun);
    }

    if (IsValid(Moon))
    {
        Moon->SetWorldRotation(Result.Moon);
    }
}
