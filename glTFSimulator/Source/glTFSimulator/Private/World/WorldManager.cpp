// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "World/WorldManager.h"

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
#include "System/GameUpdateSubSystem.h"
#include "TimerManager.h"
#include "World/SkyUpdateAsyncAction.h"
#include "World/WorldData.h"

AWorldManager::AWorldManager()
{
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

    Sun = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Sun"));
    Sun->SetupAttachment(RootComponent);
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

    Moon = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Moon"));
    Moon->SetupAttachment(RootComponent);
    Moon->SetIntensity(0.01f);
    Moon->SetUseTemperature(true);
    Moon->SetEnableLightShaftOcclusion(true);
    Moon->SetEnableLightShaftBloom(true);
    Moon->SetBloomScale(0.0001f);
    Moon->bCastShadowsOnClouds = true;
    Moon->bCastShadowsOnAtmosphere = true;
    Moon->SetForwardShadingPriority(1);

    PostProcess = CreateDefaultSubobject<UPostProcessComponent>(TEXT("PostProcess"));
    PostProcess->SetupAttachment(RootComponent);

    SkyAtmosphere = CreateDefaultSubobject<USkyAtmosphereComponent>(TEXT("SkyAtmosphere"));
    SkyAtmosphere->SetupAttachment(RootComponent);
    SkyAtmosphere->SetRayleighScatteringScale(0.003996f);
    SkyAtmosphere->SetGroundAlbedo(FColor(255.0f, 255.0f, 255.0f));
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

    Skybox = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Skybox"));
    Skybox->SetupAttachment(RootComponent);
    Skybox->SetWorldScale3D(FVector(8192.0f, 8192.0f, 8192.0f));
    Skybox->SetEnableGravity(false);

    SkyLight = CreateDefaultSubobject<USkyLightComponent>(TEXT("SkyLight"));
    SkyLight->SetupAttachment(RootComponent);
    SkyLight->SetRealTimeCapture(true);
    SkyLight->SetIntensity(0.5f);
    SkyLight->SetCastDeepShadow(true);
    SkyLight->bTransmission = true;
    SkyLight->bAffectTranslucentLighting = true;
}

void AWorldManager::BeginPlay()
{
    Super::BeginPlay();

    SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    ConfigureRenderingSettings();

    // Placed WorldManager actors can begin rendering once GameManagerSubSystem has already loaded world data.
    if (IsValid(SubSystem) && IsValid(SubSystem->GetWorldData()))
    {
        InitializeRendering(SubSystem->GetWorldData());
    }
}

void AWorldManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopRendering();
    Super::EndPlay(EndPlayReason);
}

void AWorldManager::InitializeRendering(UWorldData* InWorldData)
{
    Data = InWorldData;
    if (!IsValid(Data))
    {
        bRenderingActive = false;
        return;
    }

    bRenderingActive = true;
    ConfigureRenderingSettings();
    ApplyCloudSettings();
    RegisterGameUpdate();
    UpdateSkyLighting();
}

void AWorldManager::StopRendering()
{
    bRenderingActive = false;
    UnregisterGameUpdate();
    Data = nullptr;
}

void AWorldManager::ConfigureRenderingSettings()
{
    if (!IsValid(SubSystem) || !IsValid(PostProcess))
    {
        return;
    }

    UGameSettings* Setting = SubSystem->GetGameSettings();
    SubSystem->SetPostProcess(PostProcess);

    if (IsValid(Setting))
    {
        if (Setting->bHeightFog && !IsValid(Fog))
        {
            Fog = NewObject<UExponentialHeightFogComponent>(this);
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

        const bool bLevelAllowsCloud = !IsValid(Data) || Data->Cloud.bEnabled;
        if (Setting->bCloud && bLevelAllowsCloud && !IsValid(Cloud))
        {
            Cloud = NewObject<UVolumetricCloudComponent>(this);
            AddInstanceComponent(Cloud);
            Cloud->SetupAttachment(GetRootComponent());
            Cloud->RegisterComponent();
        }
    }

    ApplyCloudSettings();
    SubSystem->UpdateSettings();
}

void AWorldManager::ApplyCloudSettings()
{
    if (!IsValid(Data))
    {
        return;
    }

    if (!Data->Cloud.bEnabled)
    {
        if (IsValid(Cloud))
        {
            Cloud->DestroyComponent();
            Cloud = nullptr;
            CloudMID = nullptr;
        }
        return;
    }

    if (!IsValid(Cloud))
    {
        Cloud = NewObject<UVolumetricCloudComponent>(this);
        AddInstanceComponent(Cloud);
        Cloud->SetupAttachment(GetRootComponent());
        Cloud->RegisterComponent();
    }

    if (!IsValid(CloudMID))
    {
        UMaterialInterface* SourceMaterial = CloudMaterial.Get();
        if (!SourceMaterial && IsValid(Cloud))
        {
            SourceMaterial = Cloud->GetMaterial();
        }
        if (SourceMaterial)
        {
            CloudMID = UMaterialInstanceDynamic::Create(SourceMaterial, this);
            Cloud->SetMaterial(CloudMID);
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

void AWorldManager::RegisterGameUpdate()
{
    if (GameUpdateTickHandle != INDEX_NONE)
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [this](const float DeltaSeconds)
            {
                UpdateFromGameUpdate(DeltaSeconds);
            },
            35);
    }
}

void AWorldManager::UnregisterGameUpdate()
{
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
    }
    GameUpdateTickHandle = INDEX_NONE;
}

void AWorldManager::UpdateFromGameUpdate(float DeltaSeconds)
{
    // Cloud settings are applied when rendering/settings change, not redundantly every frame.
    UpdateSkyLighting();
}

void AWorldManager::UpdateSkyLighting()
{
    if (!bRenderingActive || !IsValid(Data))
    {
        return;
    }

    const FLightRotation Result = USkyUpdateAsyncAction::CalculateLightRotation(Data);

    if (IsValid(Sun))
    {
        Sun->SetWorldRotation(Result.Sun);
    }

    if (IsValid(Moon))
    {
        Moon->SetWorldRotation(Result.Moon);
    }
}
