// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

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
#include "System/GameUpdateSubSystem.h"
#include "TimerManager.h"
#include "World/SkyUpdateAsyncAction.h"
#include "World/WorldData.h"

AWorldEnvManager::AWorldEnvManager()
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
    Moon->SetIntensity(0.005f);
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

void AWorldEnvManager::BeginPlay()
{
    Super::BeginPlay();

    SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    ConfigureRenderingSettings();

    // Placed WorldEnvManager actors can begin rendering once GameManagerSubSystem has already loaded world data.
    if (UGameManagerSubSystem* GameManager = SubSystem.Get(); IsValid(GameManager) && IsValid(GameManager->GetWorldData()))
    {
        InitializeRendering(GameManager->GetWorldData());
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

    UGameSettings* Setting = GameManager->GetGameSettings();

    if (IsValid(Setting))
    {
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

        // Do not allocate the cloud component before validated world data is available.
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
    }

    ApplyCloudSettings();
    GameManager->UpdateSettings();
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
