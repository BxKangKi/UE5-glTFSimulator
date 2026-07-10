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
    Sun->bCastShadowsOnClouds = true;
    Sun->bCastShadowsOnAtmosphere = true;

    Moon = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Moon"));
    Moon->SetupAttachment(RootComponent);
    Moon->SetIntensity(0.005f);
    Moon->bUseTemperature = true;
    Moon->SetEnableLightShaftOcclusion(true);
    Moon->SetEnableLightShaftBloom(true);
    Moon->SetBloomScale(0.0001f);
    Moon->bCastShadowsOnAtmosphere = true;
    Moon->ForwardShadingPriority = 1;

    PostProcess = CreateDefaultSubobject<UPostProcessComponent>(TEXT("PostProcess"));
    PostProcess->SetupAttachment(RootComponent);

    SkyAtmosphere = CreateDefaultSubobject<USkyAtmosphereComponent>(TEXT("SkyAtmosphere"));
    SkyAtmosphere->SetupAttachment(RootComponent);
    SkyAtmosphere->RayleighScatteringScale = 0.003996f;

    Skybox = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Skybox"));
    Skybox->SetupAttachment(RootComponent);
    Skybox->SetWorldScale3D(FVector(8192.0f, 8192.0f, 8192.0f));

    SkyLight = CreateDefaultSubobject<USkyLightComponent>(TEXT("SkyLight"));
    SkyLight->SetupAttachment(RootComponent);
    SkyLight->bRealTimeCapture = true;
    SkyLight->SetIntensity(0.5f);
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
    StartSkyAsyncUpdate();
}

void AWorldManager::StopRendering()
{
    bRenderingActive = false;
    bSkyUpdateInFlight = false;
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
            Fog->SetupAttachment(GetRootComponent());
            Fog->RegisterComponent();
            Fog->SetFogDensity(0.02f);
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
    ApplyCloudSettings();
    StartSkyAsyncUpdate();
}

void AWorldManager::StartSkyAsyncUpdate()
{
    if (bSkyUpdateInFlight || !bRenderingActive || !IsValid(Data))
    {
        return;
    }

    USkyUpdateAsyncAction* AsyncAction = USkyUpdateAsyncAction::SkyUpdateAsync(this, Data);
    if (!AsyncAction)
    {
        return;
    }

    bSkyUpdateInFlight = true;
    AsyncAction->OnCompleted.AddDynamic(this, &AWorldManager::SkyUpdate);
    AsyncAction->Activate();
}

void AWorldManager::SkyUpdate(FLightRotation Result)
{
    bSkyUpdateInFlight = false;
    if (!bRenderingActive)
    {
        return;
    }

    if (IsValid(Sun))
    {
        Sun->SetWorldRotation(Result.Sun);
    }

    if (IsValid(Moon))
    {
        Moon->SetWorldRotation(Result.Moon);
    }
}
