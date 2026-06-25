// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "World/WorldManager.h"
#include "System/GameManagerSubSystem.h"
#include "System/ActorHelper.h"
#include "System/FileFunctionLibrary.h"
#include "System/MacroLibrary.h"
#include "World/WorldData.h"
#include "Model/StreamActor.h"
#include "Model/SpawnActor.h"
#include "Components/PostProcessComponent.h"
#include "Character/CharacterController.h"
#include "Character/PlayerCharacterController.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Setting/GameSettings.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "World/SkyUpdateAsyncAction.h"
#include "Blueprint/UserWidget.h"
#include "Kismet/GameplayStatics.h"
#include "Components/VolumetricCloudComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Materials/MaterialInterface.h"
#include "Runtime/RuntimeGameplayManager.h"
#include "EngineUtils.h"

#define MODEL_DIRECTORY TEXT("/model/")
#define PLAYER_DIRECTORY TEXT("/player/")

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
    SkyAtmosphere->RayleighScatteringScale = 0.003996;
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
    SpawnActors.Empty();
    SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    if (IsValid(SubSystem))
    {
        SubSystem->SetWorldLoading(true);
        SubSystem->SetLoadingStatus(0.0f);
    }
    ShowLoadingWidget();
    if (IsValid(SubSystem) && IsValid(PostProcess))
    {
        UGameSettings *Setting = SubSystem->GetGameSettings();
        SubSystem->SetPostProcess(PostProcess);
        if (IsValid(Setting))
        {
            if (Setting->bHeightFog)
            {
                Fog = NewObject<UExponentialHeightFogComponent>(this);
                AddInstanceComponent(Fog);
                Fog->SetupAttachment(GetRootComponent());
                Fog->RegisterComponent();
                Fog->SetFogDensity(0.02f);
            }
            if (Setting->bCloud)
            {
                Cloud = NewObject<UVolumetricCloudComponent>(this);
                AddInstanceComponent(Cloud);
                Cloud->SetupAttachment(GetRootComponent());
                Cloud->RegisterComponent();
                Cloud->SetMaterial(CloudMaterial);
            }
        }
        SubSystem->UpdateSettings();
    }
    LoadWorldData();
    SpawnOcean();
    SpawnPlayerAsync();
}


void AWorldManager::LoadSpawnActor(const FString &Path)
{
    UWorld *World = GetWorld();
    if (!World || !SpawnActorClass)
    {
        UE_LOG(LogTemp, Warning, TEXT("WorldManager: SpawnActorClass가 유효하지 않습니다."));
        return;
    }
    FTransform SpawnTransform = GetActorTransform();
    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = this; // BP의 Owner 핀 연결 (Self)
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::Undefined;
    ASpawnActor *NewSpawnActor =
        FActorHelper::SpawnActorDeferred<ASpawnActor>(
            GetWorld(),
            SpawnActorClass,
            SpawnTransform,
            SpawnParams);
    if (NewSpawnActor)
    {
        NewSpawnActor->Init(Path);
        NewSpawnActor->FinishSpawning(SpawnTransform);
        SpawnActors.Emplace(NewSpawnActor);
    }
}

void AWorldManager::SpawnOcean()
{
    if (CheckOcean())
    {
        if (WaterClass)
        {
            FActorSpawnParameters SpawnParams;
            SpawnParams.Owner = this; // BP의 Self 핀 연결
            SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::Undefined;
            Ocean = GetWorld()->SpawnActor<AActor>(WaterClass, OceanTransform, SpawnParams);
        }
    }
}

void AWorldManager::ShowLoadingWidget()
{
    if (LoadingWidgetClass)
    {
        LoadingWidgetInstance = CreateWidget<UUserWidget>(GetWorld(), LoadingWidgetClass);
        if (LoadingWidgetInstance)
        {
            LoadingWidgetInstance->AddToViewport(0);
        }
    }
}

bool AWorldManager::CheckOcean()
{
    if (IsValid(Data))
    {
        return Data->bOcean;
    }
    else
    {
        return false;
    }
}

void AWorldManager::SpawnWorld()
{
    if (IsValid(Data))
    {
        TArray<FString> PathArray = UFileFunctionLibrary::GetFileNamesWithExtension(GetFilePath(MODEL_DIRECTORY), "glb");
        for (const FString &Path : PathArray)
        {
            LoadSpawnActor(Path);
        }
    }
}

bool AWorldManager::SpawnPlayer()
{
    if (IsValid(Data) && IsValid(SubSystem))
    {
        ACharacterController *Ctrl = SubSystem->GetPlayerActor<ACharacterController>();
        if (IsValid(Ctrl))
        {
            const FString Path = GetFilePath(PLAYER_DIRECTORY).Append(Data->Player);
            Ctrl->Load(Path);
            return true;
        }
    }
    return false;
}

void AWorldManager::ActivatePlayer()
{
    if (IsValid(SubSystem))
    {
        ACharacterController *Ctrl = SubSystem->GetPlayerActor<ACharacterController>();
        if (IsValid(Ctrl))
        {
            Ctrl->Activate(true);
        }
    }
}

void AWorldManager::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (IsValid(Data))
    {
        Data->WorldTime += DeltaSeconds * Data->TimeSpeed;
        Data->PlayerLocation = SubSystem->GetPlayerLocation();
    }
}


bool AWorldManager::CheckAllSpawnActorLoaded()
{
    const int32 Count = SpawnActors.Num();
    if (Count == 0)
    {
        // Empty worlds are valid. The old code returned false forever here, so
        // LoadWorldAsync never advanced and RuntimeGameplayManager was never spawned.
        if (IsValid(SubSystem))
        {
            SubSystem->SetLoadingStatus(1.0f);
        }
        return true;
    }

    bool bAllLoaded = true;
    float Percent = 0.0f;
    for (const ASpawnActor *SpawnActor : SpawnActors)
    {
        if (!IsValid(SpawnActor))
        {
            bAllLoaded = false;
            continue;
        }

        Percent += SpawnActor->GetLoadingStatus() / static_cast<float>(Count);
        if (!SpawnActor->GetIsLoaded())
        {
            bAllLoaded = false;
        }
    }
    if (IsValid(SubSystem))
    {
        SubSystem->SetLoadingStatus(FMath::Clamp(Percent, 0.0f, 1.0f));
    }
    return bAllLoaded;
}

bool AWorldManager::CheckPlayerLoaded()
{
    if (IsValid(Data) && IsValid(SubSystem))
    {
        ACharacterController *Ctrl = SubSystem->GetPlayerActor<ACharacterController>();
        if (IsValid(Ctrl))
            return Ctrl->bIsLoaded;
    }
    return false;
}

FString AWorldManager::GetFilePath(const FString &FileName)
{
    if (IsValid(SubSystem))
    {
        return FPaths::Combine(PATH_ROOT, SubSystem->GetCurrentWorldName()).Append(FileName);
    }
    return TEXT("");
}


void AWorldManager::LoadWorldData()
{
    Data = NewObject<UWorldData>(this);
    if (IsValid(Data) && IsValid(SubSystem))
    {
        FString Path = GetFilePath(LEVEL_FILE_NAME);
        TSharedPtr<FJsonObject> Json = UFileFunctionLibrary::FromJson(Path);
        if (!UWorldData::DeserializeData(Data, Json))
        {
            UE_LOG(LogTemp, Log, TEXT("World file doesn't exist. Generate new one."));
            SaveWorldData();
        }
        SubSystem->SetPlayerLocation(Data->PlayerLocation);
    }
}


void AWorldManager::SaveWorldData()
{
    if (IsValid(Data))
    {
        TSharedRef<FJsonObject> Json = UWorldData::SerializeData(Data);
        FString Path = GetFilePath(LEVEL_FILE_NAME);
        UFileFunctionLibrary::ToJsonAsync(Json, Path);
    }
}

// --- Spawn Player Async 섹션 ---
void AWorldManager::SpawnPlayerAsync()
{
    // BP: DelayUntilNextTick 후 SpawnPlayer 호출
    if (SpawnPlayer())
    {
        SpawnWorld();
        LoadWorldAsync(); // 다음 단계로
    }
    else
    {
        // False일 경우 다음 틱에 재시도 (재귀 호출 대신 타이머 권장)
        GetWorldTimerManager().SetTimerForNextTick(this, &AWorldManager::SpawnPlayerAsync);
    }
}

// --- Load World Async 섹션 ---
void AWorldManager::LoadWorldAsync()
{
    if (CheckAllSpawnActorLoaded() && IsValid(SubSystem))
    {
        SubSystem->SetLoadingStatus(1.0f);
        ActivatePlayer();
        LoadPlayerAsync(); // 다음 단계로
    }
    else
    {
        // 다음 틱에 다시 체크
        GetWorldTimerManager().SetTimerForNextTick(this, &AWorldManager::LoadWorldAsync);
    }
}

// --- Load Player Async 섹션 ---
void AWorldManager::LoadPlayerAsync()
{
    if (CheckPlayerLoaded())
    {
        // 모든 로딩 완료 시 루프들 시작
        AsyncTick();
        SaveTick();
        
        // 로딩 위젯만 제거합니다. RemoveAllWidgets는 기존 Blueprint HUD까지 지워 Runtime HUD와 충돌을 일으킵니다.
        if (IsValid(LoadingWidgetInstance))
        {
            LoadingWidgetInstance->RemoveFromParent();
            LoadingWidgetInstance = nullptr;
        }
        if (IsValid(SubSystem))
        {
            SubSystem->SetWorldLoading(false);
        }

        SpawnRuntimeGameplayManager();
        if ((!IsValid(SubSystem) || !SubSystem->GetGamePaused()))
        {
            if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(UGameplayStatics::GetPlayerController(this, 0)))
            {
                PlayerController->ApplyGameInputMode();
            }
        }
    }
    else
    {
        GetWorldTimerManager().SetTimerForNextTick(this, &AWorldManager::LoadPlayerAsync);
    }
}

void AWorldManager::AsyncTick()
{
    USkyUpdateAsyncAction *AsyncAction = USkyUpdateAsyncAction::SkyUpdateAsync(this, Data);
    if (AsyncAction)
    {
        // 중요: 결과값을 인자로 받는 현재 함수를 다시 바인딩합니다.
        AsyncAction->OnCompleted.AddDynamic(this, &AWorldManager::SkyUpdate);
        AsyncAction->Activate();
    }
}

void AWorldManager::SkyUpdate(FLightRotation Result)
{
    if (IsValid(Sun))
    {
        Sun->SetWorldRotation(Result.Sun);
        // float Intensity = 15.0f;
        // Sun->SetIntensity(Intensity);
    }

    if (IsValid(Moon))
    {
        Moon->SetWorldRotation(Result.Moon);
        // float Intensity = 15.0f;
        // Moon->SetIntensity(Intensity);
    }
    GetWorldTimerManager().SetTimerForNextTick(this, &AWorldManager::AsyncTick);
}

void AWorldManager::SaveTick()
{
    SaveWorldData();

    // BP: 10초 Delay 후 다음 틱에 실행
    FTimerDelegate SaveDelegate;
    SaveDelegate.BindLambda([this]()
    {
        GetWorldTimerManager().SetTimerForNextTick(this, &AWorldManager::SaveTick);
    });

    GetWorldTimerManager().SetTimer(TimerHandle_SaveTick, SaveDelegate, 10.0f, false);
}

void AWorldManager::SpawnRuntimeGameplayManager()
{
    UWorld* World = GetWorld();
    if (IsValid(RuntimeGameplayManager) || !World)
    {
        return;
    }

    if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(UGameplayStatics::GetPlayerController(this, 0)))
    {
        RuntimeGameplayManager = PlayerController->GetRuntimeGameplayManager();
        if (IsValid(RuntimeGameplayManager))
        {
            return;
        }
    }

    for (TActorIterator<ARuntimeGameplayManager> It(World); It; ++It)
    {
        RuntimeGameplayManager = *It;
        return;
    }

    FActorSpawnParameters Params;
    Params.Owner = this;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    RuntimeGameplayManager = World->SpawnActor<ARuntimeGameplayManager>(ARuntimeGameplayManager::StaticClass(), FTransform::Identity, Params);
}
