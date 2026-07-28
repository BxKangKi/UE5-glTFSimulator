// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "System/GameManagerSubSystem.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Editor/TransBuffer.h"
#endif
#include "System/GameManagerActor.h"
#include "Model/EditableMeshActor.h"
#include "World/PrefabActor.h"
#include "Vehicle/VehiclePawn.h"
#include "Weapon/WeaponActor.h"
#include "Model/glTFStreamActor.h"
#include "World/WorldManager.h"
#include "World/WeatherActor.h"
#include "World/PlayerData.h"
#include "ProceduralMeshComponent.h"
#include "System/MacroLibrary.h"
#include "System/MultiplayerWorldSubSystem.h"
#include "System/MultiplayerWorldStateActor.h"
#include "System/PhysicsHelper.h"
#include "Setting/GameSettings.h"
#include "World/WorldData.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/GameUserSettings.h"
#include "System/SystemInfoFunctionLibrary.h"
#include "Components/PostProcessComponent.h"
#include "Model/glTFSaveLibrary.h"
#include "Model/glTFStreamSubSystem.h"
#include "System/AssetManageSubSystem.h"
#include "World/WorldData.h"
#include "Character/CharacterController.h"
#include "Character/CharacterComponent.h"
#include "Character/PlayerCharacterController.h"
#include "System/FileFunctionLibrary.h"
#include "Camera/CameraComponent.h"
#include "Blueprint/UserWidget.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "RenderingThread.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/GarbageCollection.h"
#include "Engine/GameInstance.h"

static constexpr int32 ToolbarSlotCount = 7;
#define MODEL_DIRECTORY TEXT("/model/")
#define PLAYER_DIRECTORY TEXT("/player/")


UGameManagerSubSystem::UGameManagerSubSystem()
{
    bIsGamePaused = false;
    bIsWorldLoading = false;
    bIsGamePaused = false;
    LoadingStatus = 0;
    TotalSumFPS = 0;
    TotalCountFPS = 0;
    PrefabActorClass = APrefabActor::StaticClass();
    EditableMeshActorClass = AEditableMeshActor::StaticClass();
    VehiclePawnClass = AVehiclePawn::StaticClass();
    WeaponActorClass = AWeaponActor::StaticClass();
    WeatherActorClass = AWeatherActor::StaticClass();
    WorldManagerClass = AWorldManager::StaticClass();
    SpawnActorClass = AglTFStreamActor::StaticClass();
}


bool UGameManagerSubSystem::ShouldCreateSubsystem(UObject *Outer) const
{
    // Determines whether the subsystem should be created; returns true by default.
    return true;
}

void UGameManagerSubSystem::Initialize(FSubsystemCollectionBase &Collection)
{
    Super::Initialize(Collection);
    GameSettings = UGameSettings::CreateSettingsData(this);

    if (!PostLoadMapCleanupHandle.IsValid())
    {
        PostLoadMapCleanupHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(
            this,
            &UGameManagerSubSystem::HandlePostLoadMapRuntimeCleanup);
    }

    // Runs when the game instance is created; place initialization here.
}

void UGameManagerSubSystem::Deinitialize()
{
    // Runs when the game instance shuts down; clean up runtime actors owned by the subsystem.
    StopGameManager(EEndPlayReason::Destroyed);

    if (PostLoadMapCleanupHandle.IsValid())
    {
        FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapCleanupHandle);
        PostLoadMapCleanupHandle.Reset();
    }

    RegisteredWorldSelectionWorld.Reset();
    WorldSelectionTravelSourceWorld.Reset();
    WorldNameBeforeMenuTravel.Reset();
    bMenuTravelStatePrepared = false;
    bMenuTravelSaveCompleted = false;

    Super::Deinitialize();
}

void UGameManagerSubSystem::SaveSettings()
{
    if (IsValid(GameSettings))
    {
        GameSettings->SaveSettingsData();
    }
}

void UGameManagerSubSystem::TogglePause()
{
    if (bIsWorldLoading)
    {
        return;
    }
    SetGamePaused(!bIsGamePaused);
}

void UGameManagerSubSystem::SetGamePaused(bool bPaused)
{
    if (bIsWorldLoading && bPaused)
    {
        return;
    }
    bIsGamePaused = bPaused;
    if (UWorld* World = GetWorld())
    {
        UGameplayStatics::SetGamePaused(World, bIsGamePaused);
    }
}

void UGameManagerSubSystem::SetWorldLoading(bool bLoading)
{
    bIsWorldLoading = bLoading;
    if (bIsWorldLoading)
    {
        SetGamePaused(false);
    }

    if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(UGameplayStatics::GetPlayerController(this, 0)))
    {
        if (bIsWorldLoading)
        {
            PlayerController->ApplyLoadingInputMode(LoadingWidgetInstance.Get());
        }
        else if (!GetGamePaused())
        {
            PlayerController->ApplyGameInputMode();
        }
    }
}

void UGameManagerSubSystem::UpdateSettings()
{
    if (IsValid(GameSettings) && IsValid(PostProcess))
    {
        GameSettings->UpdateSettings(PostProcess);
    }
}

UGameManagerSubSystem *UGameManagerSubSystem::GetSubSystem(AActor *InActor)
{
    return IsValid(InActor) ? GetSubSystem(InActor->GetWorld()) : nullptr;
}

UGameManagerSubSystem* UGameManagerSubSystem::GetSubSystem(const UObject* WorldContextObject)
{
    return IsValid(WorldContextObject) ? GetSubSystem(WorldContextObject->GetWorld()) : nullptr;
}

UGameManagerSubSystem *UGameManagerSubSystem::GetSubSystem(UWorld *InWorld)
{
    if (!InWorld)
    {
        return nullptr;
    }

    UGameInstance *Instance = InWorld->GetGameInstance();
    if (IsValid(Instance))
    {
        return Instance->GetSubsystem<UGameManagerSubSystem>();
    }
    else
    {
        return nullptr;
    }
}


void UGameManagerSubSystem::RegisterWorldSelectionWorld(TSoftObjectPtr<UWorld> InWorldSelectionWorld)
{
    if (InWorldSelectionWorld.IsNull())
    {
        return;
    }

    RegisteredWorldSelectionWorld = InWorldSelectionWorld;
    UE_LOG(LogTemp, Display,
        TEXT("[MenuTravel] StartActor registered the current menu world as the pause-Exit fallback: %s"),
        *GetNameSafe(InWorldSelectionWorld.Get()));
}

void UGameManagerSubSystem::FinalizeWorldSelectionTravelState()
{
    bOpenWorldSelectionMenuOnNextMainWorld = false;
    bWorldSelectionMenuTravelInProgress = false;
    WorldSelectionTravelSourceWorld.Reset();
    WorldNameBeforeMenuTravel.Reset();
    bMenuTravelStatePrepared = false;
    bMenuTravelSaveCompleted = false;
}

void UGameManagerSubSystem::CancelWorldSelectionMenuTravel()
{
    if (bMenuTravelStatePrepared)
    {
        CurrentWorldName = WorldNameBeforeMenuTravel;
        if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
        {
            Multiplayer->SetSelectedWorldFolderName(CurrentWorldName);
        }
    }

    bPendingMainWorldRuntimePurge = false;
    FinalizeWorldSelectionTravelState();
    UE_LOG(LogTemp, Warning,
        TEXT("[MenuTravel] Cancelled the pending world-selection request and restored the active gameplay-world state."));
}

void UGameManagerSubSystem::ToggleFullscreen()
{
    UGameUserSettings *UserSettings = GEngine->GetGameUserSettings();
    if (IsValid(UserSettings))
    {
        // Read the current window mode.
        EWindowMode::Type CurrentMode = UserSettings->GetFullscreenMode();

        // Toggle from fullscreen/windowed-fullscreen to windowed, or from windowed to fullscreen.
        EWindowMode::Type NewMode = (CurrentMode == EWindowMode::Windowed)
                                        ? EWindowMode::WindowedFullscreen // Or EWindowMode::Fullscreen for exclusive fullscreen.
                                        : EWindowMode::Windowed;

        UserSettings->SetFullscreenMode(NewMode);
        UserSettings->ApplySettings(false); // Apply resolution changes.
    }
}

FString UGameManagerSubSystem::GetDebugText()
{
    FString Result = TEXT("[Debug]");
    Result.Append(LINE_TERMINATOR);
    Result = GetHardwareInfoText(Result);
    Result = GetFramerateInfoText(Result);
    return Result;
}

FString UGameManagerSubSystem::GetHardwareInfoText(FString InString)
{
    FSystemHardwareInfo Info = USystemInfoFunctionLibrary::GetSystemHardwareInfo();
    InString.Append(TEXT("CPU Name: ")).Append(Info.CPUBrand).Append(LINE_TERMINATOR);
    InString.Append(TEXT("CPU Core Count: ")).Append(FString::FromInt(Info.CoreCount));
    InString.Append(LINE_TERMINATOR);
    InString.Append(TEXT("GPU Name: ")).Append(Info.GPUBrand).Append(LINE_TERMINATOR);
    FString Used = FString::FromInt(USystemInfoFunctionLibrary::GetUsedMemory());
    FString Total = FString::FromInt(USystemInfoFunctionLibrary::GetTotalMemory());
    InString.Append(TEXT("Memory: ")).Append(Used).Append(TEXT(" / "));
    InString.Append(Total).Append(TEXT(" (MB)")).Append(LINE_TERMINATOR);
    return InString;
}

FString UGameManagerSubSystem::GetFramerateInfoText(FString InString)
{
    int32 FPS = FMath::RoundToInt(USystemInfoFunctionLibrary::GetFramerate());
    // Prevent overflow
    if (TotalSumFPS >= MAX_INT32 - FPS)
    {
        TotalSumFPS = 0;
        TotalCountFPS = 0;
    }
    TotalSumFPS += FPS;
    TotalCountFPS += 1;
    int32 AvgFPS = FMath::RoundToInt(TotalSumFPS / (float)TotalCountFPS);
    InString.Append(TEXT("FPS: ")).Append(FString::FromInt(FPS)).Append(LINE_TERMINATOR);
    InString.Append(TEXT("FPS(Avg): ")).Append(FString::FromInt(AvgFPS)).Append(LINE_TERMINATOR);
    return InString;
}

void UGameManagerSubSystem::ApplyEditorConfig(const AGameManagerActor* InConfigActor)
{
    if (!IsValid(InConfigActor))
    {
        return;
    }

    // Copy only editor-authored values from the actor. Runtime state remains owned by this subsystem.
    PlacementGridMaterial = InConfigActor->PlacementGridMaterial;
    PlacementGridSpacing = InConfigActor->PlacementGridSpacing;
    PlacementGridLineThickness = InConfigActor->PlacementGridLineThickness;
    PlacementGridMaxRadius = InConfigActor->PlacementGridMaxRadius;
    PlacementGridStrongRadius = InConfigActor->PlacementGridStrongRadius;
    PlacementGridFadeRadius = InConfigActor->PlacementGridFadeRadius;
    PrefabActorClass = InConfigActor->PrefabActorClass;
    EditableMeshActorClass = InConfigActor->EditableMeshActorClass;
    VehiclePawnClass = InConfigActor->VehiclePawnClass;
    WeaponActorClass = InConfigActor->WeaponActorClass;
    WeatherActorClass = InConfigActor->WeatherActorClass;
    WorldManagerClass = InConfigActor->WorldManagerClass;
    SpawnActorClass = InConfigActor->SpawnActorClass;
    WaterClass = InConfigActor->WaterClass;
    OceanTransform = InConfigActor->OceanTransform;
    LoadingWidgetClass = InConfigActor->LoadingWidgetClass;
    PlacementTraceDistance = InConfigActor->PlacementTraceDistance;
    CrosshairCollisionTraceDistance = InConfigActor->CrosshairCollisionTraceDistance;
    FreeSpacePlacementDistance = InConfigActor->FreeSpacePlacementDistance;
    bAllowFreeSpacePlacement = InConfigActor->bAllowFreeSpacePlacement;
    GridSize = InConfigActor->GridSize;
    SurfacePlacementOffset = InConfigActor->SurfacePlacementOffset;
    VertexSelectionRayDistance = InConfigActor->VertexSelectionRayDistance;
    VertexDragHoldSeconds = InConfigActor->VertexDragHoldSeconds;
    VertexDragStartDistance = InConfigActor->VertexDragStartDistance;
    VehicleEnterDistance = InConfigActor->VehicleEnterDistance;
    bEnableObjectVertexCreation = InConfigActor->bEnableObjectVertexCreation;
    bAutoSaveScene = InConfigActor->bAutoSaveScene;
    SceneAutoSaveIntervalSeconds = InConfigActor->SceneAutoSaveIntervalSeconds;
    bSaveSceneOnEndPlay = InConfigActor->bSaveSceneOnEndPlay;
    PlayMode = InConfigActor->PlayMode;
}

FTimerManager& UGameManagerSubSystem::GetWorldTimerManager() const
{
    check(GetWorld());
    return GetWorld()->GetTimerManager();
}

FVector UGameManagerSubSystem::GetManagerActorLocation() const
{
    return ConfigActor.IsValid() ? ConfigActor->GetActorLocation() : FVector::ZeroVector;
}

void UGameManagerSubSystem::EnsureRuntimeComponents()
{
    if (!ConfigActor.IsValid())
    {
        return;
    }

    Root = ConfigActor->GetRootComponent();
    if (IsValid(PlacementGridComponent))
    {
        return;
    }

    // The grid mesh is runtime state, so it is created by the subsystem on the config actor instead of being an editor property.
    PlacementGridComponent = NewObject<UProceduralMeshComponent>(ConfigActor.Get(), TEXT("PlacementGrid_Runtime"));
    if (IsValid(PlacementGridComponent))
    {
        PlacementGridComponent->SetupAttachment(Root);
        PlacementGridComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        PlacementGridComponent->SetGenerateOverlapEvents(false);
        PlacementGridComponent->SetCastShadow(false);
        PlacementGridComponent->SetHiddenInGame(true);
        PlacementGridComponent->RegisterComponent();
    }
}


void UGameManagerSubSystem::StartGameManager(AGameManagerActor* InConfigActor)
{
    if (ConfigActor.IsValid() && ConfigActor.Get() != InConfigActor)
    {
        StopGameManager(EEndPlayReason::Destroyed);
    }

    ConfigActor = InConfigActor;
    ApplyEditorConfig(InConfigActor);

    // A newly started gameplay manager belongs to a completed menu -> gameplay transition.
    bMenuTravelStatePrepared = false;
    bMenuTravelSaveCompleted = false;
    WorldNameBeforeMenuTravel.Reset();

    if (UWorld* World = GetWorld())
    {
        UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this);
        const TCHAR* WorldOption = World->URL.GetOption(TEXT("World="), nullptr);
        if (WorldOption && FCString::Strlen(WorldOption) > 0)
        {
            CurrentWorldName = FString(WorldOption);
        }
        else if (CurrentWorldName.IsEmpty() && IsValid(Multiplayer))
        {
            // Single-player OpenLevel historically omitted the World URL option. Recover from the
            // GameInstance subsystem so the selected folder survives MainWorld -> SingleWorld travel.
            CurrentWorldName = Multiplayer->GetSelectedWorldFolderName();
        }

        CurrentWorldName.TrimStartAndEndInline();
        if (!CurrentWorldName.IsEmpty() && IsValid(Multiplayer))
        {
            Multiplayer->SetSelectedWorldFolderName(CurrentWorldName);
        }

        if (World->GetNetMode() != NM_Standalone && World->GetNetMode() != NM_Client)
        {
            AMultiplayerWorldStateActor::SpawnOrUpdateForWorld(this, CurrentWorldName);
        }
    }

    EnsureRuntimeComponents();

    // Input and UI code may request the manager repeatedly; only run the boot sequence once per config actor.
    if (bManagerStarted)
    {
        return;
    }
    bManagerStarted = true;
    InitializeWorldBootstrap();
    EnsureAssetFolders();
    ScanAssetFolders();
    BuildAvailableItems();
    InitializeToolbarSlotsIfNeeded();
    ApplySelectedToolbarItem(false);
    UE_LOG(LogTemp, Display, TEXT("[Gameplay] GameManager active: %s Class=%s"),
        *GetNameSafe(this),
        *GetNameSafe(GetClass()));
    NotifyToolbarChanged();
    NotifyStateChanged();
    if (UWorld* World = GetWorld())
    {
        TWeakObjectPtr<UGameManagerSubSystem> WeakThis(this);
        World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([WeakThis]()
        {
            if (WeakThis.IsValid())
            {
                WeakThis->LoadSavedScene();
            }
        }));
    }

    if (bAutoSaveScene && SceneAutoSaveIntervalSeconds >= 5.0f)
    {
        GetWorldTimerManager().SetTimer(
            SceneAutoSaveTimerHandle,
            this,
            &UGameManagerSubSystem::AutoSaveScene,
            SceneAutoSaveIntervalSeconds,
            true);
    }
}

void UGameManagerSubSystem::StopGameManager(const EEndPlayReason::Type EndPlayReason)
{
    const bool bHadActiveMainWorld = bManagerStarted
        || bWorldBootstrapStarted
        || IsValid(StreamSubSystem)
        || IsValid(WorldManagerActor)
        || IsValid(OceanActor)
        || IsValid(LoadingWidgetInstance)
        || SpawnedPrefabs.Num() > 0
        || SpawnedGeneratedMeshes.Num() > 0
        || SpawnedVehicles.Num() > 0
        || IsValid(CurrentEditableActor)
        || IsValid(PendingEmptyObjectPreviewActor)
        || IsValid(EquippedWeapon);

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(SceneAutoSaveTimerHandle);
        World->GetTimerManager().ClearTimer(WorldDataSaveTimerHandle);
        World->GetTimerManager().ClearAllTimersForObject(this);
    }

    if (bHadActiveMainWorld && bSaveSceneOnEndPlay && !bMenuTravelSaveCompleted
        && EndPlayReason != EEndPlayReason::Destroyed)
    {
        // Saving is intentionally performed from EndPlay rather than the pause-menu OnClicked
        // handler. This keeps Exit responsive even when the streamed glTF scene is large.
        SaveScene();
        SaveWorldData();
        SavePlayerData();
        bMenuTravelSaveCompleted = true;
    }

    if (bMenuTravelStatePrepared)
    {
        // The source folder must remain valid until the EndPlay save above finishes. Clear it only
        // after level teardown has committed the gameplay state.
        CurrentWorldName.Reset();
        if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
        {
            Multiplayer->SetSelectedWorldFolderName(FString());
        }
    }

    ReleaseMainWorldRuntimeMemory(false);
}

void UGameManagerSubSystem::PrepareForMenuLevelTravelRequest()
{
    if (bMenuTravelStatePrepared)
    {
        SetGamePaused(false);
        return;
    }

    // The button callback only records intent and unpauses travel. Saving, actor destruction,
    // asset release, and the expensive purge all occur during level teardown/PostLoadMap.
    WorldNameBeforeMenuTravel = CurrentWorldName;
    bMenuTravelStatePrepared = true;
    bMenuTravelSaveCompleted = false;
    RequestPostLoadRuntimeMemoryCleanup();
    SetGamePaused(false);
}

void UGameManagerSubSystem::PrepareForReturnToMenuLevel()
{
    // Backward-compatible Blueprint/C++ entry point. It is deliberately non-destructive so legacy
    // Exit-button listeners cannot freeze the UI before OpenLevel has even been requested.
    PrepareForMenuLevelTravelRequest();
}

void UGameManagerSubSystem::PrepareForReturnToMainWorld()
{
    // MainWorld/menu entry point. The cleanup path is shared by menu and world-selection level travel.
    PrepareForReturnToMenuLevel();
}

void UGameManagerSubSystem::PrepareForReturnToStartWorld()
{
    // Backward-compatible Blueprint/C++ entry point for projects that have not refreshed renamed nodes yet.
    PrepareForReturnToMainWorld();
}

void UGameManagerSubSystem::RequestWorldSelectionMenuOnNextMainWorld()
{
    bOpenWorldSelectionMenuOnNextMainWorld = true;
}

void UGameManagerSubSystem::RequestWorldSelectionMenuOnNextStartWorld()
{
    // Backward-compatible Blueprint/C++ entry point for projects that have not refreshed renamed nodes yet.
    RequestWorldSelectionMenuOnNextMainWorld();
}

bool UGameManagerSubSystem::ConsumeWorldSelectionMenuRequest()
{
    const bool bShouldOpenWorldSelection = bOpenWorldSelectionMenuOnNextMainWorld;
    FinalizeWorldSelectionTravelState();
    return bShouldOpenWorldSelection;
}

void UGameManagerSubSystem::ClearWorldSelectionMenuRequest()
{
    FinalizeWorldSelectionTravelState();
}

void UGameManagerSubSystem::ReleaseMainWorldRuntimeMemory(bool bForceGarbageCollection)
{
    // GameInstance subsystems survive level travel, so every UPROPERTY reference held here can keep
    // gameplay-world actors, generated meshes/textures, glTF assets, and async-load state reachable.
    SetWorldLoading(false);
    SetGamePaused(false);
    HideLoadingWidget();

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(SceneAutoSaveTimerHandle);
        World->GetTimerManager().ClearTimer(WorldDataSaveTimerHandle);
        World->GetTimerManager().ClearAllTimersForObject(this);
    }

    StopWorldSystems();
    DestroyTrackedRuntimeActors();

    ClearPlacementGridMesh();
    if (IsValid(PlacementGridComponent))
    {
        PlacementGridComponent->DestroyComponent();
        PlacementGridComponent = nullptr;
    }

    if (UAssetManageSubSystem* AssetManager = GetAssetManagerSubsystem())
    {
        AssetManager->DeactivateAndRelease();
    }

    Root = nullptr;
    ConfigActor = nullptr;
    ClearTransientRuntimeReferences();
    ResetWorldRuntimeReferences();

    bManagerStarted = false;
    bWorldBootstrapStarted = false;
    bWorldLoadCompleted = false;
    bSpawnedWorldManager = false;
    bIsWorldLoading = false;
    bIsGamePaused = false;
    LoadingStatus = 0;

    if (bForceGarbageCollection)
    {
        RequestRuntimeGarbageCollection(TEXT("ReleaseMainWorldRuntimeMemory"));
        bPendingMainWorldRuntimePurge = false;
    }
}

void UGameManagerSubSystem::DestroyTrackedRuntimeActors()
{
    APawn* CurrentPlayerPawn = UGameplayStatics::GetPlayerPawn(this, 0);

    auto DestroyActorIfValid = [CurrentPlayerPawn](AActor* Actor)
    {
        if (IsValid(Actor) && !Actor->IsActorBeingDestroyed() && Actor != CurrentPlayerPawn)
        {
            Actor->Destroy();
        }
    };

    DestroyActorIfValid(PendingEmptyObjectPreviewActor.Get());
    if (CurrentEditableActor.Get() != PendingEmptyObjectPreviewActor.Get())
    {
        DestroyActorIfValid(CurrentEditableActor.Get());
    }
    DestroyActorIfValid(EquippedWeapon.Get());

    for (APrefabActor* Prefab : SpawnedPrefabs)
    {
        DestroyActorIfValid(Prefab);
    }
    for (AEditableMeshActor* MeshActor : SpawnedGeneratedMeshes)
    {
        DestroyActorIfValid(MeshActor);
    }
    for (AVehiclePawn* Vehicle : SpawnedVehicles)
    {
        if (IsValid(Vehicle))
        {
            if (Vehicle->IsOccupied())
            {
                Vehicle->ExitVehicle();
            }
            DestroyActorIfValid(Vehicle);
        }
    }

    if (ACharacterController* PlayerCharacter = Cast<ACharacterController>(CurrentPlayerPawn))
    {
        PlayerCharacter->PrepareForPawnReplacement();
    }

    PendingEmptyObjectPreviewActor = nullptr;
    CurrentEditableActor = nullptr;
    EquippedWeapon = nullptr;
    SpawnedPrefabs.Reset();
    SpawnedGeneratedMeshes.Reset();
    SpawnedVehicles.Reset();
}

void UGameManagerSubSystem::ResetWorldRuntimeReferences()
{
    PlayerActor = nullptr;
    CurrentCamera = nullptr;
    PostProcess = nullptr;
    CurrentWorldData = nullptr;
    ActiveWorldData = nullptr;
    WorldManagerActor = nullptr;
    OceanActor = nullptr;
    StreamSubSystem = nullptr;
    LoadingWidgetInstance = nullptr;
}

void UGameManagerSubSystem::RequestRuntimeGarbageCollection(const TCHAR* Reason) const
{
    if (IsGarbageCollecting())
    {
        return;
    }

    // Do not run a synchronous full GC while a menu level is still finishing its load.
    // FlushRenderingCommands + CollectGarbage can look like a hard stop around the editor/game
    // loading-progress phase when a large glTF world has just been released. Queue the purge instead.
    if (GEngine)
    {
        GEngine->ForceGarbageCollection(true);
        UE_LOG(LogTemp, Display, TEXT("[Gameplay] Runtime memory cleanup queued: %s"), Reason ? Reason : TEXT("Unknown"));
        return;
    }

    CollectGarbage(RF_NoFlags, true);
    UE_LOG(LogTemp, Display, TEXT("[Gameplay] Runtime memory cleanup completed: %s"), Reason ? Reason : TEXT("Unknown"));
}

UAssetManageSubSystem* UGameManagerSubSystem::GetAssetManagerSubsystem() const
{
    if (UGameInstance* GameInstance = GetGameInstance())
    {
        return GameInstance->GetSubsystem<UAssetManageSubSystem>();
    }
    return nullptr;
}

void UGameManagerSubSystem::RequestPostLoadRuntimeMemoryCleanup()
{
    bPendingMainWorldRuntimePurge = true;
}

void UGameManagerSubSystem::HandlePostLoadMapRuntimeCleanup(UWorld* LoadedWorld)
{
    if (!bPendingMainWorldRuntimePurge)
    {
        return;
    }

    if (LoadedWorld)
    {
        LoadedWorld->GetTimerManager().SetTimerForNextTick(
            FTimerDelegate::CreateUObject(this, &UGameManagerSubSystem::RunPostLoadRuntimeMemoryCleanup));
        return;
    }

    RunPostLoadRuntimeMemoryCleanup();
}

void UGameManagerSubSystem::RunPostLoadRuntimeMemoryCleanup()
{
    if (!bPendingMainWorldRuntimePurge)
    {
        return;
    }
    bPendingMainWorldRuntimePurge = false;

    // GameInstance subsystems survive OpenLevel. After a menu/world-selection level is active, clear any last
    // runtime references/caches that could keep gameplay glTF actors, generated assets, or render
    // resources alive, then run a full purge with render-thread synchronization.
    UglTFStreamSubSystem* GlobalStreamSubSystem = nullptr;
    if (UGameInstance* GameInstance = GetGameInstance())
    {
        GlobalStreamSubSystem = GameInstance->GetSubsystem<UglTFStreamSubSystem>();
    }
    if (GlobalStreamSubSystem)
    {
        GlobalStreamSubSystem->StopMainWorldStreaming();
    }

    if (UAssetManageSubSystem* AssetManager = GetAssetManagerSubsystem())
    {
        AssetManager->DeactivateAndRelease();
    }

    HideLoadingWidget();
    ClearPlacementGridMesh();
    ClearTransientRuntimeReferences();
    ResetWorldRuntimeReferences();

    RequestRuntimeGarbageCollection(TEXT("PostLoadMapRuntimeCleanup"));
}


void UGameManagerSubSystem::InitializeWorldSystems(UWorldData* InWorldData, const FString& InModelDirectory, const FString& InPlayerDirectory, const FString& InInitialPlayerName)
{
    ActiveWorldData = InWorldData;
    ApplyLevelSettings();

    // Keep gameplay-owned world actors centralized here: water and streamed GLB actors are not rendering concerns.
    SpawnOcean();
    MainWorldStreaming(InModelDirectory, InPlayerDirectory, InInitialPlayerName);
}

void UGameManagerSubSystem::StopWorldSystems()
{
    HideLoadingWidget();

    // Stop the streaming subsystem before destroying this manager so spawned stream actors release their assets cleanly.
    if (IsValid(StreamSubSystem))
    {
        StreamSubSystem->StopMainWorldStreaming();
        StreamSubSystem = nullptr;
    }
    else if (UglTFStreamSubSystem* GlobalStreamSubSystem = UglTFStreamSubSystem::Get(this))
    {
        // GameInstance subsystems persist after map travel. If our cached pointer was already cleared,
        // still force-stop the global streaming subsystem so glTF assets cannot stay resident.
        GlobalStreamSubSystem->StopMainWorldStreaming();
    }

    if (IsValid(ActiveWeatherActor))
    {
        ActiveWeatherActor->Destroy();
        ActiveWeatherActor = nullptr;
    }

    if (IsValid(OceanActor))
    {
        OceanActor->Destroy();
        OceanActor = nullptr;
    }

    if (IsValid(WorldManagerActor))
    {
        WorldManagerActor->StopRendering();
        if (bSpawnedWorldManager)
        {
            WorldManagerActor->Destroy();
        }
        WorldManagerActor = nullptr;
    }

    if (UAssetManageSubSystem* AssetManager = GetAssetManagerSubsystem())
    {
        AssetManager->DeactivateAndRelease();
    }

    bSpawnedWorldManager = false;
    ActivePlayerData = nullptr;
    bCurrentLevelCheatsEnabled = false;
    ActiveWorldData = nullptr;
    SetWorldData(nullptr);
}

bool UGameManagerSubSystem::AreWorldSystemsReady() const
{
    return !IsValid(StreamSubSystem) || StreamSubSystem->IsInitialWorldReady();
}

float UGameManagerSubSystem::GetWorldSystemsLoadingStatus() const
{
    return IsValid(StreamSubSystem) ? StreamSubSystem->GetLoadingStatus() : 1.0f;
}

void UGameManagerSubSystem::InitializeWorldBootstrap()
{
    if (bWorldBootstrapStarted)
    {
        return;
    }
    bWorldBootstrapStarted = true;

    SetWorldLoading(true);
    SetLoadingStatus(0.0f);

    // Loading UI, world data, world rendering, water, and GLB streaming now start from one owner.
    ShowLoadingWidget();
    LoadWorldData();
    LoadPlayerData();
    ApplyLevelSettings();
    SpawnWorldManager();

    if (IsValid(WorldManagerActor))
    {
        WorldManagerActor->InitializeRendering(ActiveWorldData);
    }

    InitializeWorldSystems(
        ActiveWorldData,
        GetWorldFilePath(MODEL_DIRECTORY),
        GetWorldFilePath(PLAYER_DIRECTORY),
        ActivePlayerId);

    LoadWorldAsync();
}

void UGameManagerSubSystem::SpawnWorldManager()
{
    UWorld* World = GetWorld();
    if (IsValid(WorldManagerActor) || !World)
    {
        return;
    }

    UClass* EffectiveWorldManagerClass = WorldManagerClass ? WorldManagerClass.Get() : AWorldManager::StaticClass();

    for (TActorIterator<AWorldManager> It(World); It; ++It)
    {
        AWorldManager* ExistingWorldManager = *It;
        if (IsValid(ExistingWorldManager) && ExistingWorldManager->IsA(EffectiveWorldManagerClass))
        {
            WorldManagerActor = ExistingWorldManager;
            bSpawnedWorldManager = false;
            return;
        }
    }

    FActorSpawnParameters Params;
    Params.Owner = ConfigActor.Get();
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    WorldManagerActor = World->SpawnActor<AWorldManager>(EffectiveWorldManagerClass, FTransform::Identity, Params);
    bSpawnedWorldManager = IsValid(WorldManagerActor);
}

bool UGameManagerSubSystem::CheckWorldSystemsLoaded()
{
    if (!IsValid(StreamSubSystem))
    {
        SetLoadingStatus(1.0f);
        return true;
    }

    const float Percent = StreamSubSystem->GetLoadingStatus();
    SetLoadingStatus(FMath::Clamp(Percent, 0.0f, 1.0f));
    return StreamSubSystem->IsInitialWorldReady();
}

void UGameManagerSubSystem::LoadWorldData()
{
    ActiveWorldData = NewObject<UWorldData>(this);
    if (!IsValid(ActiveWorldData))
    {
        return;
    }

    const FString Path = GetWorldFilePath(LEVEL_FILE_NAME);
    TSharedPtr<FJsonObject> Json = UFileFunctionLibrary::FromJson(Path);
    if (!UWorldData::DeserializeData(ActiveWorldData, Json))
    {
        UE_LOG(LogTemp, Log, TEXT("World file does not exist or is invalid. Generating a new world data file."));
        SaveWorldData();
    }

    // Legacy fallback only. The authoritative per-player transform is loaded from player.json next.
    SetPlayerLocation(ActiveWorldData->PlayerLocation);
    SetWorldData(ActiveWorldData);
}

void UGameManagerSubSystem::LoadPlayerData()
{
    ActivePlayerData = NewObject<UPlayerData>(this);
    if (!IsValid(ActivePlayerData))
    {
        return;
    }

    ActivePlayerId = IsValid(ActiveWorldData) && !ActiveWorldData->Player.IsEmpty()
        ? ActiveWorldData->Player
        : FString(TEXT("Player"));

    const FString Path = GetWorldFilePath(PLAYER_FILE_NAME);
    const TSharedPtr<FJsonObject> Json = UFileFunctionLibrary::FromJson(Path);
    if (!UPlayerData::DeserializeData(ActivePlayerData, Json) || ActivePlayerData->Players.Num() == 0)
    {
        FWorldPlayerRecord& DefaultPlayer = ActivePlayerData->FindOrAddPlayer(ActivePlayerId);
        DefaultPlayer.Location = IsValid(ActiveWorldData) ? ActiveWorldData->PlayerLocation : FVector::ZeroVector;
        DefaultPlayer.DisplayName = ActivePlayerId;
        SavePlayerData();
    }

    FWorldPlayerRecord& PlayerRecord = ActivePlayerData->FindOrAddPlayer(ActivePlayerId);
    SetPlayerLocation(PlayerRecord.Location);
}

void UGameManagerSubSystem::SaveWorldData()
{
    if (!IsValid(ActiveWorldData))
    {
        return;
    }

    TSharedRef<FJsonObject> Json = UWorldData::SerializeData(ActiveWorldData);
    UFileFunctionLibrary::ToJsonAsync(Json, GetWorldFilePath(LEVEL_FILE_NAME));
}

void UGameManagerSubSystem::SavePlayerData()
{
    if (!IsValid(ActivePlayerData))
    {
        return;
    }

    FWorldPlayerRecord& PlayerRecord = ActivePlayerData->FindOrAddPlayer(ActivePlayerId);
    PlayerRecord.Location = GetPlayerLocation();
    if (const AActor* Player = PlayerActor.Get())
    {
        PlayerRecord.Rotation = Player->GetActorRotation();
    }

    const TSharedRef<FJsonObject> Json = UPlayerData::SerializeData(ActivePlayerData);
    UFileFunctionLibrary::ToJsonAsync(Json, GetWorldFilePath(PLAYER_FILE_NAME));
}

void UGameManagerSubSystem::ApplyLevelSettings()
{
    ApplyGameplaySettings();
    ApplyWeatherSettings();
}

void UGameManagerSubSystem::ApplyWeatherSettings()
{
    UWorld* World = GetWorld();
    if (!World || !IsValid(ActiveWorldData))
    {
        return;
    }

    if (!ActiveWorldData->Weather.bEnabled)
    {
        if (IsValid(ActiveWeatherActor))
        {
            ActiveWeatherActor->Destroy();
            ActiveWeatherActor = nullptr;
        }
        return;
    }

    if (!IsValid(ActiveWeatherActor))
    {
        UClass* SpawnClass = WeatherActorClass ? WeatherActorClass.Get() : AWeatherActor::StaticClass();
        FActorSpawnParameters Params;
        Params.Owner = ConfigActor.Get();
        ActiveWeatherActor = World->SpawnActor<AWeatherActor>(SpawnClass, FTransform::Identity, Params);
    }

    if (IsValid(ActiveWeatherActor))
    {
        ActiveWeatherActor->ConfigureWeather(ActiveWorldData->Weather.Preset, ActiveWorldData->Weather.Intensity);
    }
}

void UGameManagerSubSystem::ApplyGameplaySettings()
{
    bCurrentLevelCheatsEnabled = IsValid(ActiveWorldData) && ActiveWorldData->Gameplay.bCheatsEnabled;

    if (IsValid(ActiveWorldData))
    {
        if (ActiveWorldData->Gameplay.WorldGameMode.Equals(TEXT("Creator"), ESearchCase::IgnoreCase))
        {
            PlayMode = EPlayMode::Creator;
        }
        else if (ActiveWorldData->Gameplay.WorldGameMode.Equals(TEXT("RealLife"), ESearchCase::IgnoreCase))
        {
            PlayMode = EPlayMode::RealLife;
        }
    }

    if (bCurrentLevelCheatsEnabled)
    {
        if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
        {
            PC->EnableCheats();
        }
    }
}

void UGameManagerSubSystem::SaveWorldDataDelayed()
{
    UWorld* World = GetWorld();
    if (World && World->GetNetMode() == NM_Client)
    {
        return;
    }

    SaveWorldData();
    SavePlayerData();

    if (!World || !bManagerStarted)
    {
        return;
    }

    // Bind the timer to this UObject instead of capturing raw this in a lambda.
    // ClearAllTimersForObject(this) can now reliably cancel the save loop during PIE shutdown.
    World->GetTimerManager().SetTimer(
        WorldDataSaveTimerHandle,
        this,
        &UGameManagerSubSystem::SaveWorldDataDelayed,
        10.0f,
        false);
}

void UGameManagerSubSystem::LoadWorldAsync()
{
    if (bWorldLoadCompleted)
    {
        return;
    }

    if (CheckWorldSystemsLoaded())
    {
        bWorldLoadCompleted = true;
        SetLoadingStatus(1.0f);
        HideLoadingWidget();
        SetWorldLoading(false);
        if (!GetWorld() || GetWorld()->GetNetMode() != NM_Client)
        {
            SaveWorldDataDelayed();
        }

        if (!GetGamePaused())
        {
            if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(UGameplayStatics::GetPlayerController(this, 0)))
            {
                PlayerController->ApplyGameInputMode();
            }
        }
    }
    else
    {
        if (UWorld* World = GetWorld())
        {
            World->GetTimerManager().SetTimerForNextTick(this, &UGameManagerSubSystem::LoadWorldAsync);
        }
    }
}

void UGameManagerSubSystem::UpdateWorldTime(float DeltaSeconds)
{
    if (!IsValid(ActiveWorldData))
    {
        return;
    }

    if (const UWorld* World = GetWorld())
    {
        if (World->GetNetMode() == NM_Client)
        {
            return;
        }
    }

    ActiveWorldData->WorldTime += DeltaSeconds * ActiveWorldData->TimeSpeed;

    // Player transforms live in player.json. Keep the legacy level.json location mirrored only
    // for backward compatibility with older worlds that have not generated player.json yet.
    const FVector CurrentLocation = GetPlayerLocation();
    ActiveWorldData->PlayerLocation = CurrentLocation;
    if (IsValid(ActivePlayerData))
    {
        FWorldPlayerRecord& PlayerRecord = ActivePlayerData->FindOrAddPlayer(ActivePlayerId);
        PlayerRecord.Location = CurrentLocation;
        if (const AActor* Player = PlayerActor.Get())
        {
            PlayerRecord.Rotation = Player->GetActorRotation();
        }
    }
}

FString UGameManagerSubSystem::GetWorldFilePath(const FString& FileName) const
{
    const FString WorldName = CurrentWorldName.IsEmpty() ? FString(TEXT("New World")) : CurrentWorldName;
    return FPaths::Combine(PATH_ROOT, WorldName).Append(FileName);
}

void UGameManagerSubSystem::ShowLoadingWidget()
{
    if (IsValid(LoadingWidgetInstance) || !LoadingWidgetClass)
    {
        return;
    }

    LoadingWidgetInstance = CreateWidget<UUserWidget>(GetWorld(), LoadingWidgetClass);
    if (IsValid(LoadingWidgetInstance))
    {
        LoadingWidgetInstance->AddToViewport(0);
        if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(UGameplayStatics::GetPlayerController(this, 0)))
        {
            PlayerController->ApplyLoadingInputMode(LoadingWidgetInstance.Get());
        }
    }
}

void UGameManagerSubSystem::HideLoadingWidget()
{
    if (IsValid(LoadingWidgetInstance))
    {
        LoadingWidgetInstance->RemoveFromParent();
        LoadingWidgetInstance = nullptr;
    }
}

bool UGameManagerSubSystem::ShouldSpawnOcean() const
{
    return IsValid(ActiveWorldData) && ActiveWorldData->bOcean;
}

void UGameManagerSubSystem::SpawnOcean()
{
    if (IsValid(OceanActor) || !ShouldSpawnOcean() || !WaterClass)
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = ConfigActor.Get();
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::Undefined;
    OceanActor = World->SpawnActor<AActor>(WaterClass, OceanTransform, SpawnParams);
}

void UGameManagerSubSystem::StartWorldStreaming(const FString& InModelDirectory, const FString& InPlayerDirectory, const FString& InInitialPlayerName)
{
    // Backward-compatible private wrapper for pre-rename C++ call sites.
    MainWorldStreaming(InModelDirectory, InPlayerDirectory, InInitialPlayerName);
}

void UGameManagerSubSystem::MainWorldStreaming(const FString& InModelDirectory, const FString& InPlayerDirectory, const FString& InInitialPlayerName)
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    StreamSubSystem = UglTFStreamSubSystem::Get(this);
    if (!IsValid(StreamSubSystem))
    {
        return;
    }

    // Fall back to the native stream actor if a Blueprint subclass was not assigned in the manager instance.
    TSubclassOf<AglTFStreamActor> EffectiveSpawnClass = SpawnActorClass;
    if (!EffectiveSpawnClass)
    {
        EffectiveSpawnClass = AglTFStreamActor::StaticClass();
    }

    const bool bRenderOnlyStreaming = UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this);
    StreamSubSystem->StartMainWorldStreaming(
        ConfigActor.Get(),
        EffectiveSpawnClass,
        InModelDirectory,
        InPlayerDirectory,
        InInitialPlayerName,
        bRenderOnlyStreaming);
}

void UGameManagerSubSystem::UpdateGameManager(float DeltaSeconds)
{
    UpdateWorldTime(DeltaSeconds);

    if (PlayMode != EPlayMode::Creator)
    {
        DestroyPendingEmptyObjectPreview();
        ClearPlacementGridMesh();
        return;
    }

    const bool bNeedsPlacementTick = CurrentMode != EToolMode::None
        || IsValid(CurrentEditableActor)
        || bPrimaryVertexPressActive
        || IsSelectedToolbarItemObjectCreation();
    if (!bNeedsPlacementTick)
    {
        DestroyPendingEmptyObjectPreview();
        ClearPlacementGridMesh();
        return;
    }

    FHitResult Hit;
    FVector Preview;
    if (TracePlacementLocation(Preview, Hit))
    {
        LastPreviewLocation = ApplyGridSnap(Preview);
    }

    UpdateObjectCreationPreview();
    UpdateEditableVertexPreviewAndSelection();
    UpdatePlacementGrid();
}

void UGameManagerSubSystem::ClearTransientRuntimeReferences()
{
    // GameInstanceSubsystems persist across level travel. Drop every strong reference to
    // MainWorld actors, components, generated assets, UI, and world data so GC can reclaim them
    // after the old UWorld is unloaded.
    PlayerActor = nullptr;
    CurrentCamera = nullptr;
    PostProcess = nullptr;
    CurrentWorldData = nullptr;
    ActiveWorldData = nullptr;
    LoadingWidgetInstance = nullptr;
    StreamSubSystem = nullptr;
    OceanActor = nullptr;
    WorldManagerActor = nullptr;
    Root = nullptr;
    PlacementGridComponent = nullptr;

    CurrentEditableActor = nullptr;
    PendingEmptyObjectPreviewActor = nullptr;
    EquippedWeapon = nullptr;
    SpawnedPrefabs.Reset();
    SpawnedGeneratedMeshes.Reset();
    SpawnedVehicles.Reset();

    PrefabFiles.Reset();
    VehicleFiles.Reset();
    WeaponFiles.Reset();
    AvailableItems.Reset();
    ToolbarSlots.Reset();
    bToolbarInitialized = false;

    // Keep CurrentWorldName across ordinary level travel. A confirmed menu return clears it only
    // after StopGameManager has finished saving the source gameplay world.
    PlayerLocation = FVector::ZeroVector;
    SelectedToolbarSlotIndex = 0;
    CurrentPrefabIndex = 0;
    CurrentWeaponIndex = 0;
    CurrentMode = EToolMode::None;
    bSnapToGrid = false;
    bFirstPerson = false;
    bItemListWindowOpen = false;

    LastPreviewLocation = FVector::ZeroVector;
    PendingEmptyObjectLocation = FVector::ZeroVector;
    PendingVertexLocation = FVector::ZeroVector;
    bHasPendingEmptyObjectLocation = false;
    bHasPendingVertexLocation = false;
    LastVertexDistance = 0.0f;

    LastTraceHit = FHitResult();
    bLastTraceBlockingHit = false;
    bLastTraceHasPlacementLocation = false;
    bLastTraceUsedFreeSpace = false;
    LastTraceStart = FVector::ZeroVector;
    LastTraceDirection = FVector::ForwardVector;
    LastSaveMessage.Reset();
    bSavedSceneLoaded = false;
    bIsSavingScene = false;

    HighlightedEditableVertexIndex = INDEX_NONE;
    bMovingHighlightedEditableVertex = false;
    bPrimaryVertexPressActive = false;
    bPrimaryVertexDragActive = false;
    PressedEditableVertexIndex = INDEX_NONE;
    ConnectedEditableVertexSourceIndex = INDEX_NONE;
    PrimaryVertexPressStartTime = 0.0;
    PrimaryVertexPressStartLocation = FVector::ZeroVector;
    bCurrentEditableActorWasExisting = false;
    bHasOriginalEditableMeshRecord = false;
    OriginalEditableMeshRecord = FGeneratedMeshRecord();
    CachedPlacementGridCenter = FVector::ZeroVector;
    CachedPlacementGridRadius = 0.0f;
    bPlacementGridBuilt = false;
}

void UGameManagerSubSystem::EnsureAssetFolders() const
{
    IFileManager::Get().MakeDirectory(*GetPrefabDirectory(), true);
    IFileManager::Get().MakeDirectory(*GetItemsDirectory(), true);
    IFileManager::Get().MakeDirectory(*FPaths::Combine(GetWorldRootPath(), TEXT("generated")), true);
}

FString UGameManagerSubSystem::GetWorldRootPath() const
{
    const FString WorldName = CurrentWorldName.IsEmpty() ? FString(TEXT("New World")) : CurrentWorldName;
    return FPaths::Combine(PATH_ROOT, WorldName);
}

FString UGameManagerSubSystem::GetPrefabDirectory() const
{
    return FPaths::Combine(GetWorldRootPath(), TEXT("prefab"));
}

FString UGameManagerSubSystem::GetItemsDirectory() const
{
    return FPaths::Combine(GetWorldRootPath(), TEXT("items"));
}

FString UGameManagerSubSystem::GetManifestPath() const
{
    return FPaths::Combine(GetWorldRootPath(), TEXT("entities.json"));
}

FString UGameManagerSubSystem::GetLegacyManifestPath() const
{
    return FPaths::Combine(GetWorldRootPath(), TEXT("runtime_installed.json"));
}

FString UGameManagerSubSystem::GetLegacyGltfScenePath() const
{
    return FPaths::Combine(GetWorldRootPath(), TEXT("runtime_installed.gltf"));
}

void UGameManagerSubSystem::ScanAssetFolders()
{
    PrefabFiles.Empty();
    VehicleFiles.Empty();
    WeaponFiles.Empty();

    IFileManager& FileManager = IFileManager::Get();
    const FString WorldName = FPaths::GetCleanFilename(GetWorldRootPath());

    TArray<FString> PrefabDirectories;
    PrefabDirectories.Add(GetPrefabDirectory());
    PrefabDirectories.Add(FPaths::Combine(FPaths::ProjectDir(), TEXT("World"), TEXT("prefab")));
    PrefabDirectories.Add(FPaths::Combine(FPaths::ProjectDir(), TEXT("World"), WorldName, TEXT("prefab")));

    TArray<FString> ItemDirectories;
    ItemDirectories.Add(GetItemsDirectory());
    ItemDirectories.Add(FPaths::Combine(FPaths::ProjectDir(), TEXT("World"), TEXT("items")));
    ItemDirectories.Add(FPaths::Combine(FPaths::ProjectDir(), TEXT("World"), WorldName, TEXT("items")));

    auto AppendGltfFiles = [&FileManager](const TArray<FString>& Directories, TArray<FString>& OutFiles)
    {
        for (const FString& Directory : Directories)
        {
            TArray<FString> Glb;
            TArray<FString> Gltf;
            FileManager.FindFilesRecursive(Glb, *Directory, TEXT("*.glb"), true, false, false);
            FileManager.FindFilesRecursive(Gltf, *Directory, TEXT("*.gltf"), true, false, false);
            for (FString& Path : Glb)
            {
                FPaths::NormalizeFilename(Path);
                OutFiles.AddUnique(Path);
            }
            for (FString& Path : Gltf)
            {
                FPaths::NormalizeFilename(Path);
                OutFiles.AddUnique(Path);
            }
        }
    };

    AppendGltfFiles(PrefabDirectories, PrefabFiles);
    AppendGltfFiles(ItemDirectories, WeaponFiles);

    // A glTF/GLB prefab that contains a mesh/node name ending with ;WHEL is a driveable vehicle asset.
    // Keep it out of the normal prefab list so the Vehicle placement path can load it into AVehiclePawn.
    for (int32 Index = PrefabFiles.Num() - 1; Index >= 0; --Index)
    {
        if (DoesAssetFileContainWheelTag(PrefabFiles[Index]))
        {
            VehicleFiles.AddUnique(PrefabFiles[Index]);
            PrefabFiles.RemoveAt(Index);
        }
    }

    PrefabFiles.Sort();
    VehicleFiles.Sort();
    WeaponFiles.Sort();

    CurrentPrefabIndex = PrefabFiles.Num() > 0 ? FMath::Clamp(CurrentPrefabIndex, 0, PrefabFiles.Num() - 1) : 0;
    CurrentWeaponIndex = WeaponFiles.Num() > 0 ? FMath::Clamp(CurrentWeaponIndex, 0, WeaponFiles.Num() - 1) : 0;
}

bool UGameManagerSubSystem::DoesAssetFileContainWheelTag(const FString& FilePath) const
{
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *FilePath) || Bytes.Num() <= 0)
    {
        return false;
    }

    auto ToUpperAscii = [](uint8 Value) -> uint8
    {
        return Value >= 'a' && Value <= 'z' ? Value - ('a' - 'A') : Value;
    };

    auto ContainsTag = [&Bytes, &ToUpperAscii](const ANSICHAR* Tag) -> bool
    {
        const int32 TagLen = FCStringAnsi::Strlen(Tag);
        if (TagLen <= 0 || Bytes.Num() < TagLen)
        {
            return false;
        }

        for (int32 Index = 0; Index <= Bytes.Num() - TagLen; ++Index)
        {
            bool bMatches = true;
            for (int32 TagIndex = 0; TagIndex < TagLen; ++TagIndex)
            {
                if (ToUpperAscii(Bytes[Index + TagIndex]) != static_cast<uint8>(Tag[TagIndex]))
                {
                    bMatches = false;
                    break;
                }
            }

            if (bMatches)
            {
                return true;
            }
        }

        return false;
    };

    return ContainsTag(";WHEL") || ContainsTag(";WHEEL");
}

FString UGameManagerSubSystem::GetAssetDisplayName(const FString& AssetPath) const
{
    const FString JsonPath = FPaths::ChangeExtension(AssetPath, TEXT("json"));
    FString JsonString;
    if (FFileHelper::LoadFileToString(JsonString, *JsonPath))
    {
        TSharedPtr<FJsonObject> RootObject;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
        if (FJsonSerializer::Deserialize(Reader, RootObject) && RootObject.IsValid())
        {
            FString DisplayName;
            if (RootObject->TryGetStringField(TEXT("DisplayName"), DisplayName) && !DisplayName.IsEmpty())
            {
                return DisplayName;
            }
            if (RootObject->TryGetStringField(TEXT("Name"), DisplayName) && !DisplayName.IsEmpty())
            {
                return DisplayName;
            }
        }
    }

    return FPaths::GetBaseFilename(AssetPath);
}

FToolbarItem UGameManagerSubSystem::MakeToolbarItem(EToolbarItemKind Kind, const FString& DisplayName, const FString& SourcePath, int32 SourceIndex) const
{
    FToolbarItem Item;
    Item.Kind = Kind;
    Item.DisplayName = DisplayName;
    Item.SourcePath = SourcePath;
    Item.SourceIndex = SourceIndex;
    Item.bAvailable = Kind != EToolbarItemKind::None;
    return Item;
}

void UGameManagerSubSystem::BuildAvailableItems()
{
    AvailableItems.Empty();

    if (PlayMode == EPlayMode::Creator)
    {
        if (bEnableObjectVertexCreation)
        {
            AvailableItems.Add(MakeToolbarItem(EToolbarItemKind::CreateObject, TEXT("오브젝트 만들기")));
        }

        AvailableItems.Add(MakeToolbarItem(EToolbarItemKind::Vehicle, TEXT("기본 차량 만들기")));

        for (int32 Index = 0; Index < VehicleFiles.Num(); ++Index)
        {
            AvailableItems.Add(MakeToolbarItem(EToolbarItemKind::Vehicle, GetAssetDisplayName(VehicleFiles[Index]), VehicleFiles[Index], Index));
        }

        for (int32 Index = 0; Index < PrefabFiles.Num(); ++Index)
        {
            AvailableItems.Add(MakeToolbarItem(EToolbarItemKind::Prefab, GetAssetDisplayName(PrefabFiles[Index]), PrefabFiles[Index], Index));
        }

        for (int32 Index = 0; Index < WeaponFiles.Num(); ++Index)
        {
            AvailableItems.Add(MakeToolbarItem(EToolbarItemKind::Weapon, GetAssetDisplayName(WeaponFiles[Index]), WeaponFiles[Index], Index));
        }
    }

    ReconcileToolbarSlotsWithAvailableItems();
}

void UGameManagerSubSystem::InitializeToolbarSlotsIfNeeded()
{
    if (ToolbarSlots.Num() != ToolbarSlotCount)
    {
        ToolbarSlots.SetNum(ToolbarSlotCount);
    }

    if (bToolbarInitialized)
    {
        ReconcileToolbarSlotsWithAvailableItems();
        return;
    }

    for (int32 Slot = 0; Slot < ToolbarSlotCount; ++Slot)
    {
        ToolbarSlots[Slot] = AvailableItems.IsValidIndex(Slot) ? AvailableItems[Slot] : FToolbarItem();
    }

    SelectedToolbarSlotIndex = 0;
    bToolbarInitialized = true;
    ReconcileToolbarSlotsWithAvailableItems();
}

int32 UGameManagerSubSystem::FindAvailableItemIndexMatching(const FToolbarItem& Item) const
{
    for (int32 Index = 0; Index < AvailableItems.Num(); ++Index)
    {
        const FToolbarItem& Candidate = AvailableItems[Index];
        if (Candidate.Kind != Item.Kind)
        {
            continue;
        }

        if (Item.Kind == EToolbarItemKind::CreateObject)
        {
            return Index;
        }

        if (Item.Kind == EToolbarItemKind::Vehicle)
        {
            if (Item.SourcePath.IsEmpty() && Candidate.SourcePath.IsEmpty())
            {
                return Index;
            }
            if (!Item.SourcePath.IsEmpty() && Candidate.SourcePath.Equals(Item.SourcePath, ESearchCase::IgnoreCase))
            {
                return Index;
            }
            continue;
        }

        if (!Item.SourcePath.IsEmpty() && Candidate.SourcePath.Equals(Item.SourcePath, ESearchCase::IgnoreCase))
        {
            return Index;
        }
    }

    return INDEX_NONE;
}

void UGameManagerSubSystem::ReconcileToolbarSlotsWithAvailableItems()
{
    if (ToolbarSlots.Num() != ToolbarSlotCount)
    {
        ToolbarSlots.SetNum(ToolbarSlotCount);
    }

    for (int32 Slot = 0; Slot < ToolbarSlotCount; ++Slot)
    {
        FToolbarItem& SlotItem = ToolbarSlots[Slot];
        const int32 MatchingIndex = FindAvailableItemIndexMatching(SlotItem);
        if (AvailableItems.IsValidIndex(MatchingIndex))
        {
            SlotItem = AvailableItems[MatchingIndex];
        }
        else if (SlotItem.Kind == EToolbarItemKind::None && AvailableItems.IsValidIndex(Slot))
        {
            SlotItem = AvailableItems[Slot];
        }
        else if (SlotItem.Kind != EToolbarItemKind::None)
        {
            SlotItem.bAvailable = false;
        }
    }

    SelectedToolbarSlotIndex = FMath::Clamp(SelectedToolbarSlotIndex, 0, ToolbarSlotCount - 1);
}

void UGameManagerSubSystem::NotifyToolbarChanged()
{
    OnToolbarChanged.Broadcast();
}

void UGameManagerSubSystem::NotifyStateChanged()
{
    OnStateChanged.Broadcast();
    OnMessageChanged.Broadcast(LastSaveMessage);
}

bool UGameManagerSubSystem::IsObjectCreationItem(const FToolbarItem& Item) const
{
    return bEnableObjectVertexCreation
        && PlayMode == EPlayMode::Creator
        && Item.Kind == EToolbarItemKind::CreateObject
        && Item.bAvailable;
}

bool UGameManagerSubSystem::IsSelectedToolbarItemObjectCreation() const
{
    return IsObjectCreationItem(GetSelectedToolbarItem());
}

FToolbarItem UGameManagerSubSystem::GetToolbarItemAtSlot(int32 SlotIndex) const
{
    return ToolbarSlots.IsValidIndex(SlotIndex) ? ToolbarSlots[SlotIndex] : FToolbarItem();
}

FToolbarItem UGameManagerSubSystem::GetSelectedToolbarItem() const
{
    return GetToolbarItemAtSlot(SelectedToolbarSlotIndex);
}

FToolbarItem UGameManagerSubSystem::GetAvailableItemAtIndex(int32 Index) const
{
    return AvailableItems.IsValidIndex(Index) ? AvailableItems[Index] : FToolbarItem();
}

bool UGameManagerSubSystem::SelectToolbarSlot(int32 SlotIndex)
{
    if (SlotIndex < 0 || SlotIndex >= ToolbarSlotCount)
    {
        return false;
    }

    SelectedToolbarSlotIndex = SlotIndex;
    ApplySelectedToolbarItem(true);
    return true;
}

void UGameManagerSubSystem::ScrollToolbarSelection(float ScrollValue)
{
    if (FMath::IsNearlyZero(ScrollValue) || ToolbarSlotCount <= 0)
    {
        return;
    }

    const int32 Direction = ScrollValue > 0.0f ? -1 : 1;
    SelectedToolbarSlotIndex = (SelectedToolbarSlotIndex + Direction + ToolbarSlotCount) % ToolbarSlotCount;
    ApplySelectedToolbarItem(true);
}

bool UGameManagerSubSystem::SetToolbarSlotFromAvailableItem(int32 SlotIndex, int32 AvailableItemIndex)
{
    if (!ToolbarSlots.IsValidIndex(SlotIndex) || !AvailableItems.IsValidIndex(AvailableItemIndex))
    {
        LastSaveMessage = TEXT("툴바에 넣을 아이템 인덱스가 유효하지 않습니다.");
        NotifyStateChanged();
        return false;
    }

    ToolbarSlots[SlotIndex] = AvailableItems[AvailableItemIndex];
    SelectedToolbarSlotIndex = SlotIndex;
    ApplySelectedToolbarItem(true);
    return true;
}

bool UGameManagerSubSystem::SelectAvailableItemForCurrentToolbarSlot(int32 AvailableItemIndex, bool bCloseItemList)
{
    if (!SetToolbarSlotFromAvailableItem(SelectedToolbarSlotIndex, AvailableItemIndex))
    {
        return false;
    }

    if (bCloseItemList)
    {
        SetItemListWindowOpen(false);
    }
    return true;
}

void UGameManagerSubSystem::ToggleItemListWindow()
{
    SetItemListWindowOpen(!bItemListWindowOpen);
}

void UGameManagerSubSystem::SetItemListWindowOpen(bool bOpen)
{
    if (bItemListWindowOpen == bOpen)
    {
        return;
    }

    bItemListWindowOpen = bOpen;
    LastSaveMessage = bItemListWindowOpen ? TEXT("전체 아이템 목록 열림") : TEXT("전체 아이템 목록 닫힘");

    if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr))
    {
        if (bItemListWindowOpen)
        {
            PlayerController->ApplyUIInputMode(nullptr);
        }
        else
        {
            PlayerController->ApplyGameInputMode();
        }
    }

    OnItemListWindowChanged.Broadcast(bItemListWindowOpen);
    NotifyStateChanged();
}

void UGameManagerSubSystem::SetPlayMode(EPlayMode NewMode)
{
    if (PlayMode == NewMode)
    {
        return;
    }

    PlayMode = NewMode;
    ScanAssetFolders();
    BuildAvailableItems();
    InitializeToolbarSlotsIfNeeded();
    ApplySelectedToolbarItem(false);
    LastSaveMessage = PlayMode == EPlayMode::Creator ? TEXT("Creator Mode") : TEXT("Real Life Mode");
    NotifyToolbarChanged();
    NotifyStateChanged();
}

void UGameManagerSubSystem::ApplySelectedToolbarItem(bool bBroadcastChange)
{
    // Cache the selected toolbar item once so the rest of the function uses a stable target even if finishing an edit broadcasts UI events.
    const FToolbarItem Item = GetSelectedToolbarItem();

    // A toolbar change is a hard mode change, so any live mesh edit must be resolved before the cursor preview swaps tools.
    // Valid meshes are finalized; meshes with only one/two dangling vertices are canceled or reverted by FinishCurrentEditableMesh().
    CloseCurrentEditableMeshForToolChange();

    // Remove the old object-creation cursor actor before the new toolbar item decides whether it needs a new preview.
    DestroyPendingEmptyObjectPreview();

    // Clear old pending locations so a previous tool cannot leave a detached cursor behind after the item changes.
    ClearPendingPlacementSelection();

    switch (Item.Kind)
    {
    case EToolbarItemKind::CreateObject:
        if (!bEnableObjectVertexCreation)
        {
            CurrentMode = EToolMode::None;
            LastSaveMessage = TEXT("오브젝트 정점 생성 기능은 현재 임시 비활성화되어 있습니다.");
            break;
        }
        CurrentMode = EToolMode::PlaceEmptyObject;
        LastSaveMessage = TEXT("오브젝트 만들기: 중앙 십자가 위치에 프리뷰가 표시됩니다. 좌클릭 또는 우클릭=새 오브젝트/기존 메시 편집");
        break;
    case EToolbarItemKind::Prefab:
        if (Item.bAvailable && PrefabFiles.IsValidIndex(Item.SourceIndex))
        {
            CurrentPrefabIndex = Item.SourceIndex;
        }
        CurrentMode = EToolMode::PlacePrefab;
        LastSaveMessage = FString::Printf(TEXT("Prefab 선택: %s"), *GetCurrentPrefabName());
        break;
    case EToolbarItemKind::Vehicle:
        CurrentMode = EToolMode::PlaceVehicle;
        LastSaveMessage = TEXT("차량 만들기: 중앙 십자가 위치에 좌클릭으로 차량을 설치합니다.");
        break;
    case EToolbarItemKind::Weapon:
        if (Item.bAvailable && WeaponFiles.IsValidIndex(Item.SourceIndex))
        {
            CurrentWeaponIndex = Item.SourceIndex;
        }
        EquipCurrentWeapon();
        if (bBroadcastChange)
        {
            NotifyToolbarChanged();
        }
        return;
    case EToolbarItemKind::None:
    default:
        CurrentMode = EToolMode::None;
        LastSaveMessage = TEXT("툴바 슬롯이 비어 있습니다.");
        break;
    }

    if (bBroadcastChange)
    {
        NotifyToolbarChanged();
    }
    NotifyStateChanged();
}

UGameManagerSubSystem* UGameManagerSubSystem::FindGameManager(const UObject* WorldContextObject)
{
    return GetSubSystem(WorldContextObject);
}

void UGameManagerSubSystem::ResetEditorTransactionBufferForWorldTravel(const UObject* WorldContextObject, const FString& Reason)
{
#if WITH_EDITOR
    UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    const bool bRuntimeWorld = !World || World->IsGameWorld();
    if (!bRuntimeWorld)
    {
        return;
    }

    if (GEditor && GEditor->Trans)
    {
        const FString ResetReason = Reason.IsEmpty() ? FString(TEXT("Menu world travel")) : Reason;
        GEditor->Trans->Reset(FText::FromString(ResetReason));
        UE_LOG(LogTemp, Display, TEXT("[Gameplay] Editor transaction buffer reset before world travel: %s"), *ResetReason);
    }
#else
    (void)WorldContextObject;
    (void)Reason;
#endif
}

void UGameManagerSubSystem::OpenWorldSelectionScreen(
    const UObject* WorldContextObject,
    TSoftObjectPtr<UWorld> WorldSelectionWorld)
{
    TryOpenWorldSelectionScreen(WorldContextObject, WorldSelectionWorld);
}

bool UGameManagerSubSystem::TryOpenWorldSelectionScreen(
    const UObject* WorldContextObject,
    TSoftObjectPtr<UWorld> WorldSelectionWorld)
{
    UWorld* SourceWorld = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    if (!WorldContextObject || !SourceWorld || WorldSelectionWorld.IsNull())
    {
        UE_LOG(LogTemp, Error,
            TEXT("[MenuTravel] Cannot open world selection because the context or directly referenced world is invalid."));
        return false;
    }

    if (WorldSelectionWorld.Get() == SourceWorld)
    {
        UE_LOG(LogTemp, Error,
            TEXT("[MenuTravel] Refused pause Exit because the destination resolves to the active gameplay world."));
        return false;
    }

    UGameManagerSubSystem* Manager = FindGameManager(WorldContextObject);
    if (!IsValid(Manager))
    {
        UE_LOG(LogTemp, Error,
            TEXT("[MenuTravel] Cannot open world selection because the GameInstance manager is unavailable."));
        return false;
    }

    if (Manager->bWorldSelectionMenuTravelInProgress)
    {
        if (Manager->WorldSelectionTravelSourceWorld.Get() == SourceWorld)
        {
            // A WBP and the native pause widget may both receive the same click. The first request
            // owns travel; later listeners should report success without issuing another OpenLevel.
            UE_LOG(LogTemp, Verbose,
                TEXT("[MenuTravel] Equivalent world-selection request is already pending from this world."));
            return true;
        }

        UE_LOG(LogTemp, Warning,
            TEXT("[MenuTravel] Cleared a stale world-selection travel guard left by a previous world."));
        Manager->FinalizeWorldSelectionTravelState();
    }

    // Set both guards before compatibility callbacks. Every later navigation request observes an
    // accepted transition and cannot start a competing OpenLevel.
    Manager->bWorldSelectionMenuTravelInProgress = true;
    Manager->WorldSelectionTravelSourceWorld = SourceWorld;
    Manager->RequestWorldSelectionMenuOnNextMainWorld();
    Manager->PrepareForMenuLevelTravelRequest();

    // StartActor clears the pending request only after the destination menu is input-safe.
    ResetEditorTransactionBufferForWorldTravel(WorldContextObject, TEXT("Open world-selection screen"));
    UE_LOG(LogTemp, Display, TEXT("[MenuTravel] Calling OpenLevelBySoftObjectPtr for world selection."));
    UGameplayStatics::OpenLevelBySoftObjectPtr(WorldContextObject, WorldSelectionWorld, true, FString());
    return true;
}

void UGameManagerSubSystem::OpenMainMenuFromWorldSelection(
    const UObject* WorldContextObject,
    TSoftObjectPtr<UWorld> MainMenuWorld)
{
    if (!WorldContextObject || MainMenuWorld.IsNull())
    {
        UE_LOG(LogTemp, Error,
            TEXT("GameManagerSubSystem cannot open the main menu because no world asset is assigned."));
        return;
    }

    if (UGameManagerSubSystem* Manager = FindGameManager(WorldContextObject))
    {
        Manager->ClearWorldSelectionMenuRequest();
        Manager->SetGamePaused(false);
    }

    ResetEditorTransactionBufferForWorldTravel(WorldContextObject, TEXT("Open main menu from world selection"));
    UGameplayStatics::OpenLevelBySoftObjectPtr(WorldContextObject, MainMenuWorld, true, FString());
}

FVector UGameManagerSubSystem::GetPendingPlacementSelection() const
{
    if (bHasPendingVertexLocation)
    {
        return PendingVertexLocation;
    }

    if (bHasPendingEmptyObjectLocation)
    {
        return PendingEmptyObjectLocation;
    }

    return LastPreviewLocation;
}

void UGameManagerSubSystem::ClearPendingPlacementSelection()
{
    bHasPendingEmptyObjectLocation = false;
    bHasPendingVertexLocation = false;
    PendingEmptyObjectLocation = FVector::ZeroVector;
    PendingVertexLocation = FVector::ZeroVector;
    if (IsValid(CurrentEditableActor))
    {
        CurrentEditableActor->ClearPreviewVertex();
    }
}

AActor* UGameManagerSubSystem::GetCrosshairHitActor() const
{
    return bLastTraceBlockingHit ? LastTraceHit.GetActor() : nullptr;
}

FString UGameManagerSubSystem::GetCurrentPrefabName() const
{
    return PrefabFiles.IsValidIndex(CurrentPrefabIndex) ? FPaths::GetBaseFilename(PrefabFiles[CurrentPrefabIndex]) : TEXT("없음");
}

FString UGameManagerSubSystem::GetCurrentWeaponName() const
{
    return WeaponFiles.IsValidIndex(CurrentWeaponIndex) ? FPaths::GetBaseFilename(WeaponFiles[CurrentWeaponIndex]) : TEXT("없음");
}

void UGameManagerSubSystem::SelectPreviousPrefab()
{
    ScanAssetFolders();
    BuildAvailableItems();
    if (PrefabFiles.Num() > 0)
    {
        CurrentPrefabIndex = (CurrentPrefabIndex - 1 + PrefabFiles.Num()) % PrefabFiles.Num();
        LastSaveMessage = FString::Printf(TEXT("Prefab 선택: %s"), *GetCurrentPrefabName());
    }
    else
    {
        LastSaveMessage = TEXT("prefab/ 폴더에 gltf 또는 glb가 없습니다.");
    }
    NotifyStateChanged();
}

void UGameManagerSubSystem::SelectNextPrefab()
{
    ScanAssetFolders();
    BuildAvailableItems();
    if (PrefabFiles.Num() > 0)
    {
        CurrentPrefabIndex = (CurrentPrefabIndex + 1) % PrefabFiles.Num();
        LastSaveMessage = FString::Printf(TEXT("Prefab 선택: %s"), *GetCurrentPrefabName());
    }
    else
    {
        LastSaveMessage = TEXT("prefab/ 폴더에 gltf 또는 glb가 없습니다.");
    }
    NotifyStateChanged();
}

void UGameManagerSubSystem::SelectPrefabPlacementTool()
{
    // Direct Blueprint buttons bypass the toolbar path, so close any in-progress mesh edit here as well.
    CloseCurrentEditableMeshForToolChange();

    ScanAssetFolders();
    BuildAvailableItems();
    CurrentMode = EToolMode::PlacePrefab;
    LastSaveMessage = TEXT("Prefab 도구: 중앙 십자가 위치에 좌클릭으로 현재 Prefab을 설치합니다.");
    NotifyStateChanged();
}

void UGameManagerSubSystem::SelectEmptyObjectTool()
{
    if (!bEnableObjectVertexCreation)
    {
        CloseCurrentEditableMeshForToolChange();
        DestroyPendingEmptyObjectPreview();
        CurrentMode = EToolMode::None;
        LastSaveMessage = TEXT("오브젝트 정점 생성 기능은 현재 임시 비활성화되어 있습니다.");
        NotifyStateChanged();
        return;
    }

    // Selecting the object tool explicitly means "leave edit mode and return to placement".
    CloseCurrentEditableMeshForToolChange();

    CurrentMode = EToolMode::PlaceEmptyObject;
    LastSaveMessage = TEXT("오브젝트 만들기 도구: 좌클릭으로 새 오브젝트를 만들고 자동으로 정점 편집에 들어갑니다.");
    NotifyStateChanged();
}

void UGameManagerSubSystem::SelectVertexTool()
{
    if (!bEnableObjectVertexCreation)
    {
        LastSaveMessage = TEXT("오브젝트 정점 생성 기능은 현재 임시 비활성화되어 있습니다.");
        NotifyStateChanged();
        return;
    }

    if (IsValid(CurrentEditableActor) && !CurrentEditableActor->IsFinalized())
    {
        CurrentMode = EToolMode::EditVertices;
        LastSaveMessage = TEXT("정점 편집: 중앙 십자가 위치에 정점 프리뷰가 표시됩니다. 좌클릭=정점 추가/선택 정점 이동, 우클릭=완료");
    }
    else
    {
        LastSaveMessage = TEXT("편집 중인 오브젝트가 없습니다. 오브젝트 만들기 아이템을 선택하고 좌클릭하세요.");
    }
    NotifyStateChanged();
}

void UGameManagerSubSystem::SelectVehicleTool()
{
    // Direct Blueprint buttons should not leave a half-edited mesh actor owning the runtime cursor.
    CloseCurrentEditableMeshForToolChange();

    CurrentMode = EToolMode::PlaceVehicle;
    LastSaveMessage = TEXT("차량 도구: 중앙 십자가 위치에 좌클릭으로 차량을 설치합니다.");
    NotifyStateChanged();
}

void UGameManagerSubSystem::SelectPreviousWeapon()
{
    ScanAssetFolders();
    BuildAvailableItems();
    if (WeaponFiles.Num() > 0)
    {
        CurrentWeaponIndex = (CurrentWeaponIndex - 1 + WeaponFiles.Num()) % WeaponFiles.Num();
        LastSaveMessage = FString::Printf(TEXT("무기 선택: %s"), *GetCurrentWeaponName());
    }
    else
    {
        LastSaveMessage = TEXT("items/ 폴더에 gltf 또는 glb 무기가 없습니다.");
    }
    NotifyStateChanged();
}

void UGameManagerSubSystem::SelectNextWeapon()
{
    ScanAssetFolders();
    BuildAvailableItems();
    if (WeaponFiles.Num() > 0)
    {
        CurrentWeaponIndex = (CurrentWeaponIndex + 1) % WeaponFiles.Num();
        LastSaveMessage = FString::Printf(TEXT("무기 선택: %s"), *GetCurrentWeaponName());
    }
    else
    {
        LastSaveMessage = TEXT("items/ 폴더에 gltf 또는 glb 무기가 없습니다.");
    }
    NotifyStateChanged();
}

void UGameManagerSubSystem::EquipCurrentWeapon()
{
    ScanAssetFolders();

    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (!IsValid(PC))
    {
        LastSaveMessage = TEXT("PlayerController를 찾을 수 없습니다.");
        NotifyStateChanged();
        return;
    }

    if (IsValid(EquippedWeapon))
    {
        EquippedWeapon->Destroy();
        EquippedWeapon = nullptr;
    }

    USceneComponent* AttachTarget = nullptr;
    if (ACharacter* CharacterPawn = Cast<ACharacter>(PC->GetPawn()))
    {
        AttachTarget = CharacterPawn->GetMesh();
    }
    if (!IsValid(AttachTarget))
    {
        AttachTarget = GetCameraComponent<UCameraComponent>();
    }

    if (!IsValid(AttachTarget))
    {
        LastSaveMessage = TEXT("무기를 부착할 캐릭터 메시 또는 카메라를 찾을 수 없습니다.");
        NotifyStateChanged();
        return;
    }

    const bool bHasConfiguredWeaponFile = WeaponFiles.IsValidIndex(CurrentWeaponIndex);
    const FString SelectedWeaponFile = bHasConfiguredWeaponFile ? WeaponFiles[CurrentWeaponIndex] : FString();

    FActorSpawnParameters Params;
    Params.Owner = PC->GetPawn() ? Cast<AActor>(PC->GetPawn()) : ConfigActor.Get();
    Params.Instigator = PC->GetPawn();
    UClass* WeaponSpawnClass = WeaponActorClass ? WeaponActorClass.Get() : AWeaponActor::StaticClass();
    AWeaponActor* Weapon = GetWorld()->SpawnActor<AWeaponActor>(WeaponSpawnClass, FTransform::Identity, Params);

    const bool bEquipped = IsValid(Weapon) && (bHasConfiguredWeaponFile
        ? Weapon->EquipFromFile(SelectedWeaponFile, AttachTarget)
        : Weapon->EquipDefault(AttachTarget));

    if (bEquipped)
    {
        EquippedWeapon = Weapon;
        CurrentMode = EToolMode::Weapon;
        LastSaveMessage = bHasConfiguredWeaponFile
            ? FString::Printf(TEXT("무기 장착: %s"), *GetCurrentWeaponName())
            : TEXT("기본 테스트 무기 장착");
    }
    else if (IsValid(Weapon))
    {
        Weapon->Destroy();
        LastSaveMessage = TEXT("무기 로드 실패");
    }
    NotifyStateChanged();
}

void UGameManagerSubSystem::ToggleSnap()
{
    bSnapToGrid = !bSnapToGrid;
    LastSaveMessage = bSnapToGrid ? TEXT("Grid Snap 켜짐") : TEXT("Grid Snap 꺼짐");
    NotifyStateChanged();
}

void UGameManagerSubSystem::SetSnapEnabled(bool bEnabled)
{
    bSnapToGrid = bEnabled;
    LastSaveMessage = bSnapToGrid ? TEXT("Grid Snap 켜짐") : TEXT("Grid Snap 꺼짐");
    NotifyStateChanged();
}

void UGameManagerSubSystem::SetGridSize(float NewGridSize)
{
    GridSize = FMath::Max(1.0f, NewGridSize);
    LastSaveMessage = FString::Printf(TEXT("Grid Size: %.0f cm"), GridSize);
    NotifyStateChanged();
}

void UGameManagerSubSystem::ToggleFirstPerson()
{
    bFirstPerson = !bFirstPerson;
    if (UGameManagerSubSystem* GameSys = UGameManagerSubSystem::GetSubSystem(GetWorld()))
    {
        if (ACharacterController* Character = GameSys->GetPlayerActor<ACharacterController>())
        {
            Character->SetFirstPersonEnabled(bFirstPerson);
        }
    }
    LastSaveMessage = bFirstPerson ? TEXT("1인칭 모드 켜짐") : TEXT("1인칭 모드 꺼짐");
    NotifyStateChanged();
}

bool UGameManagerSubSystem::TracePlacementLocation(FVector& OutLocation, FHitResult& OutHit)
{
    // Resolve the world and the local player controller because the cursor ray must come from the active camera.
    UWorld* World = GetWorld();
    APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
    if (!IsValid(World) || !IsValid(PC))
    {
        // Fall back to the manager actor location only so callers never read an uninitialized vector.
        OutLocation = GetManagerActorLocation();
        // Reset the cached ray to a safe default when there is no player camera.
        LastTraceStart = GetManagerActorLocation();
        LastTraceDirection = FVector::ForwardVector;
        // Clear the hit result because no collision query was actually possible.
        OutHit = FHitResult();
        LastTraceHit = FHitResult();
        // Mark both collision and placement as unavailable in this exceptional case.
        bLastTraceBlockingHit = false;
        bLastTraceHasPlacementLocation = false;
        bLastTraceUsedFreeSpace = false;
        return false;
    }

    // Build query parameters for the short collision probe under the center crosshair.
    // This subsystem is not an actor, so ignored actors must be added explicitly below.
    FCollisionQueryParams Params(SCENE_QUERY_STAT(PlacementTrace), true);
    // Ignore the manager configuration actor if one exists, matching the old actor-owned trace behavior.
    if (AGameManagerActor* ManagerActor = ConfigActor.Get())
    {
        Params.AddIgnoredActor(ManagerActor);
    }
    // Ignore the controlled pawn so first-person cameras do not immediately hit the player capsule.
    if (APawn* Pawn = PC->GetPawn())
    {
        Params.AddIgnoredActor(Pawn);
    }
    // Ignore the equipped weapon so firing/held weapon meshes do not block placement directly in front of the camera.
    if (IsValid(EquippedWeapon))
    {
        Params.AddIgnoredActor(EquippedWeapon);
    }
    // Ignore the translucent object-creation preview so the cursor can move through its own preview mesh.
    if (IsValid(PendingEmptyObjectPreviewActor))
    {
        Params.AddIgnoredActor(PendingEmptyObjectPreviewActor);
    }

    // Start with a harmless fallback ray; it is overwritten by deprojection or camera viewpoint below.
    FVector Start = FVector::ZeroVector;
    FVector Direction = FVector::ForwardVector;
    // Query the viewport size so the center-crosshair ray is independent from the OS mouse cursor position.
    int32 ViewportX = 0;
    int32 ViewportY = 0;
    PC->GetViewportSize(ViewportX, ViewportY);
    if (ViewportX > 0 && ViewportY > 0)
    {
        // Deproject the exact center of the viewport, matching the Minecraft-like crosshair interaction model.
        PC->DeprojectScreenPositionToWorld(static_cast<float>(ViewportX) * 0.5f, static_cast<float>(ViewportY) * 0.5f, Start, Direction);
    }
    else
    {
        // Dedicated/serverless or unusual viewport states can still use the camera viewpoint as a ray source.
        FRotator ViewRotation;
        PC->GetPlayerViewPoint(Start, ViewRotation);
        Direction = ViewRotation.Vector();
    }

    // Normalize the ray direction before multiplying it by centimeter distances.
    Direction = Direction.GetSafeNormal();
    if (Direction.IsNearlyZero())
    {
        // Avoid NaNs and zero-length line traces if the camera returned an invalid direction.
        Direction = FVector::ForwardVector;
    }

    // Cache the raw ray for vertex-nearest-to-ray selection, even when no collision is hit.
    LastTraceStart = Start;
    LastTraceDirection = Direction;
    // Clear last-frame collision state before evaluating the new frame.
    OutHit = FHitResult();
    LastTraceHit = FHitResult();
    bLastTraceBlockingHit = false;
    bLastTraceHasPlacementLocation = false;
    bLastTraceUsedFreeSpace = false;

    // Clamp the free-space point to a hard maximum distance from the camera; 1000 cm equals 10 meters.
    const float SafeFreeSpaceDistance = FMath::Max(1.0f, FreeSpacePlacementDistance);
    // Collision probing is intentionally short and is never allowed to exceed the free-space placement cap.
    const float SafeCollisionDistance = FMath::Clamp(CrosshairCollisionTraceDistance, 0.0f, SafeFreeSpaceDistance);

    // Only perform the expensive/physical blocking test inside the configured short range.
    bool bHit = false;
    if (SafeCollisionDistance > KINDA_SMALL_NUMBER)
    {
        // Anything beyond this end point is treated as empty air, even if a far-away wall exists behind it.
        const FVector CollisionEnd = Start + Direction * SafeCollisionDistance;
        bHit = FPhysicsHelper::Raycast(World, Start, CollisionEnd, Params, OutHit);
    }

    if (bHit)
    {
        // A physical hit wins over free-space placement and places slightly above the blocking surface.
        const FVector SurfaceOffset = OutHit.ImpactNormal.GetSafeNormal() * SurfacePlacementOffset;
        OutLocation = OutHit.ImpactPoint + SurfaceOffset;
        // Cache the hit so Blueprint UI and existing-mesh editing can identify the object under the crosshair.
        LastTraceHit = OutHit;
        bLastTraceBlockingHit = true;
        bLastTraceHasPlacementLocation = true;
        bLastTraceUsedFreeSpace = false;
        return true;
    }

    // No nearby blocking object was hit; resolve the cursor to a free-space point if air placement is enabled.
    OutLocation = Start + Direction * SafeFreeSpaceDistance;
    // Keep LastTraceHit empty because there is no actor/component under the cursor in air placement mode.
    LastTraceHit = FHitResult();
    bLastTraceBlockingHit = false;
    bLastTraceUsedFreeSpace = true;
    bLastTraceHasPlacementLocation = bAllowFreeSpacePlacement;

    // Returning true means callers may create objects/vertices at the air point.
    return bLastTraceHasPlacementLocation;
}

FVector UGameManagerSubSystem::ApplyGridSnap(const FVector& Location) const
{
    if (!bSnapToGrid || GridSize <= KINDA_SMALL_NUMBER)
    {
        return Location;
    }
    return FVector(
        FMath::GridSnap(Location.X, static_cast<double>(GridSize)),
        FMath::GridSnap(Location.Y, static_cast<double>(GridSize)),
        FMath::GridSnap(Location.Z, static_cast<double>(GridSize)));
}

bool UGameManagerSubSystem::ShouldShowPlacementGrid() const
{
    if (PlayMode != EPlayMode::Creator || !bLastTraceHasPlacementLocation)
    {
        return false;
    }

    const FToolbarItem SelectedItem = GetSelectedToolbarItem();
    if (CurrentMode == EToolMode::PlaceVehicle || SelectedItem.Kind == EToolbarItemKind::Vehicle)
    {
        return false;
    }

    if (CurrentMode == EToolMode::PlacePrefab)
    {
        return true;
    }

    if (bEnableObjectVertexCreation && (CurrentMode == EToolMode::PlaceEmptyObject || CurrentMode == EToolMode::EditVertices))
    {
        return true;
    }

    return IsObjectCreationItem(SelectedItem);
}

void UGameManagerSubSystem::UpdatePlacementGrid()
{
    if (!IsValid(PlacementGridComponent))
    {
        return;
    }

    if (!ShouldShowPlacementGrid())
    {
        ClearPlacementGridMesh();
        return;
    }

    const float Spacing = FMath::Max(1.0f, PlacementGridSpacing);
    // Keep the placement guide extremely small: three 1m cells from the cursor, axes only.
    const float Radius = Spacing * 3.0f;
    const FVector Center(
        FMath::GridSnap(LastPreviewLocation.X, static_cast<double>(Spacing)),
        FMath::GridSnap(LastPreviewLocation.Y, static_cast<double>(Spacing)),
        FMath::GridSnap(LastPreviewLocation.Z, static_cast<double>(Spacing)));

    const float RebuildMoveThreshold = Spacing * 0.5f;
    const bool bNeedsRebuild = !bPlacementGridBuilt
        || FVector::DistSquared(Center, CachedPlacementGridCenter) > FMath::Square(RebuildMoveThreshold)
        || !FMath::IsNearlyEqual(Radius, CachedPlacementGridRadius, 1.0f);

    if (bNeedsRebuild)
    {
        RebuildPlacementGridMesh(Center, Radius);
    }

    PlacementGridComponent->SetHiddenInGame(false);
    PlacementGridComponent->SetVisibility(true, true);
}

void UGameManagerSubSystem::ClearPlacementGridMesh()
{
    if (IsValid(PlacementGridComponent))
    {
        PlacementGridComponent->ClearAllMeshSections();
        PlacementGridComponent->SetHiddenInGame(true);
        PlacementGridComponent->SetVisibility(false, true);
    }

    bPlacementGridBuilt = false;
    CachedPlacementGridCenter = FVector::ZeroVector;
    CachedPlacementGridRadius = 0.0f;
}

static void AppendPlacementGridLine(const FVector& Start, const FVector& End, float Thickness, const FColor& LineColor, TArray<FVector>& Vertices, TArray<int32>& Triangles, TArray<FVector>& Normals, TArray<FVector2D>& UV0, TArray<FColor>& VertexColors, TArray<FProcMeshTangent>& Tangents)
{
    const FVector Axis = End - Start;
    const float Length = Axis.Size();
    if (Length <= KINDA_SMALL_NUMBER)
    {
        return;
    }

    const FVector Direction = Axis / Length;
    const FVector Reference = FMath::Abs(Direction.Z) < 0.95f ? FVector::UpVector : FVector::RightVector;
    const FVector Side = FVector::CrossProduct(Direction, Reference).GetSafeNormal() * Thickness;
    const FVector Up = FVector::CrossProduct(Side.GetSafeNormal(), Direction).GetSafeNormal() * Thickness;

    const int32 BaseIndex = Vertices.Num();
    Vertices.Add(Start - Side - Up);
    Vertices.Add(Start + Side - Up);
    Vertices.Add(Start + Side + Up);
    Vertices.Add(Start - Side + Up);
    Vertices.Add(End - Side - Up);
    Vertices.Add(End + Side - Up);
    Vertices.Add(End + Side + Up);
    Vertices.Add(End - Side + Up);

    const int32 FaceIndices[] =
    {
        0, 1, 5, 0, 5, 4,
        1, 2, 6, 1, 6, 5,
        2, 3, 7, 2, 7, 6,
        3, 0, 4, 3, 4, 7,
        0, 3, 2, 0, 2, 1,
        4, 5, 6, 4, 6, 7
    };
    for (int32 Index : FaceIndices)
    {
        Triangles.Add(BaseIndex + Index);
    }

    for (int32 VertexIndex = 0; VertexIndex < 8; ++VertexIndex)
    {
        Normals.Add(FVector::UpVector);
        UV0.Add(FVector2D::ZeroVector);
        VertexColors.Add(LineColor);
        Tangents.Add(FProcMeshTangent(Direction, false));
    }
}

void UGameManagerSubSystem::RebuildPlacementGridMesh(const FVector& Center, float Radius)
{
    if (!IsValid(PlacementGridComponent))
    {
        return;
    }

    const float Spacing = FMath::Max(1.0f, PlacementGridSpacing);
    const float BaseThickness = FMath::Max(0.25f, PlacementGridLineThickness * 0.65f);

    // Drastically simplified 3D placement guide: only the three center axes plus short 1m tick marks.
    // This preserves scale/orientation without the visually noisy volumetric/plane grid.
    const int32 FadeCells = 3;
    const float SafeRadius = FMath::Min(FMath::Max(Radius, Spacing), Spacing * static_cast<float>(FadeCells));
    const float FadeRadius = FMath::Max(Spacing, SafeRadius);

    TArray<FVector> Vertices;
    TArray<int32> Triangles;
    TArray<FVector> Normals;
    TArray<FVector2D> UV0;
    TArray<FColor> VertexColors;
    TArray<FProcMeshTangent> Tangents;

    constexpr int32 EstimatedLines = 27;
    Vertices.Reserve(EstimatedLines * 8);
    Triangles.Reserve(EstimatedLines * 36);
    Normals.Reserve(EstimatedLines * 8);
    UV0.Reserve(EstimatedLines * 8);
    VertexColors.Reserve(EstimatedLines * 8);
    Tangents.Reserve(EstimatedLines * 8);

    const FTransform ToLocal = PlacementGridComponent->GetComponentTransform().Inverse();
    auto ToLocalPosition = [&ToLocal](const FVector& WorldPosition)
    {
        return ToLocal.TransformPosition(WorldPosition);
    };

    auto AlphaForDistance = [FadeRadius](float Distance)
    {
        const float T = FMath::Clamp(Distance / FMath::Max(1.0f, FadeRadius), 0.0f, 1.0f);
        const float SmoothT = T * T * (3.0f - 2.0f * T);
        return FMath::Clamp(1.0f - SmoothT, 0.0f, 1.0f);
    };

    auto MakeColor = [](float Alpha, bool bAxis)
    {
        const float Brightness = bAxis ? 1.0f : FMath::Square(Alpha);
        FLinearColor LinearColor(
            0.02f + 0.18f * Brightness,
            0.04f + 0.28f * Brightness,
            0.05f + 0.35f * Brightness,
            Alpha);
        FColor Color = LinearColor.ToFColor(true);
        Color.A = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(255.0f * Alpha), 0, 255));
        return Color;
    };

    auto AppendGuideLine = [&](const FVector& WorldStart, const FVector& WorldEnd, float Distance, bool bAxis)
    {
        const float Alpha = bAxis ? 0.95f : AlphaForDistance(Distance) * 0.55f;
        if (Alpha <= 0.05f)
        {
            return;
        }

        const float Thickness = BaseThickness * (bAxis ? 1.3f : FMath::Lerp(0.45f, 0.8f, Alpha));
        AppendPlacementGridLine(ToLocalPosition(WorldStart), ToLocalPosition(WorldEnd), Thickness, MakeColor(Alpha, bAxis), Vertices, Triangles, Normals, UV0, VertexColors, Tangents);
    };

    const FVector AxisX(SafeRadius, 0.0f, 0.0f);
    const FVector AxisY(0.0f, SafeRadius, 0.0f);
    const FVector AxisZ(0.0f, 0.0f, SafeRadius);

    AppendGuideLine(Center - AxisX, Center + AxisX, 0.0f, true);
    AppendGuideLine(Center - AxisY, Center + AxisY, 0.0f, true);
    AppendGuideLine(Center - AxisZ, Center + AxisZ, 0.0f, true);

    const float TickHalfLength = Spacing * 0.075f;
    for (int32 Cell = -FadeCells; Cell <= FadeCells; ++Cell)
    {
        if (Cell == 0)
        {
            continue;
        }

        const float Offset = static_cast<float>(Cell) * Spacing;
        const float Distance = FMath::Abs(Offset);
        if (Distance > SafeRadius + KINDA_SMALL_NUMBER)
        {
            continue;
        }

        const FVector XTickCenter = Center + FVector(Offset, 0.0f, 0.0f);
        const FVector YTickCenter = Center + FVector(0.0f, Offset, 0.0f);
        const FVector ZTickCenter = Center + FVector(0.0f, 0.0f, Offset);

        AppendGuideLine(XTickCenter - FVector(0.0f, TickHalfLength, 0.0f), XTickCenter + FVector(0.0f, TickHalfLength, 0.0f), Distance, false);
        AppendGuideLine(YTickCenter - FVector(TickHalfLength, 0.0f, 0.0f), YTickCenter + FVector(TickHalfLength, 0.0f, 0.0f), Distance, false);
        AppendGuideLine(ZTickCenter - FVector(TickHalfLength, 0.0f, 0.0f), ZTickCenter + FVector(TickHalfLength, 0.0f, 0.0f), Distance, false);
    }

    PlacementGridComponent->ClearAllMeshSections();
    if (Vertices.Num() > 0)
    {
        PlacementGridComponent->CreateMeshSection(0, Vertices, Triangles, Normals, UV0, VertexColors, Tangents, false);
        if (PlacementGridMaterial)
        {
            PlacementGridComponent->SetMaterial(0, PlacementGridMaterial);
        }
    }

    CachedPlacementGridCenter = Center;
    CachedPlacementGridRadius = SafeRadius;
    bPlacementGridBuilt = true;
}


void UGameManagerSubSystem::AutoSaveScene()
{
    if (!bAutoSaveScene || bIsSavingScene)
    {
        return;
    }

    SaveScene();
}

int32 UGameManagerSubSystem::CountExistingBaseName(const FString& BaseName, EPlacedObjectKind Kind) const
{
    int32 Count = 0;
    if (Kind == EPlacedObjectKind::Prefab)
    {
        for (const APrefabActor* Prefab : SpawnedPrefabs)
        {
            if (IsValid(Prefab) && Prefab->GetBaseName().Equals(BaseName, ESearchCase::IgnoreCase))
            {
                Count++;
            }
        }
    }
    else if (Kind == EPlacedObjectKind::GeneratedMesh)
    {
        for (const AEditableMeshActor* Mesh : SpawnedGeneratedMeshes)
        {
            if (IsValid(Mesh) && Mesh->GetObjectName().StartsWith(BaseName))
            {
                Count++;
            }
        }
        if (IsValid(CurrentEditableActor) && CurrentEditableActor->GetObjectName().StartsWith(BaseName))
        {
            Count++;
        }
    }
    else if (Kind == EPlacedObjectKind::Vehicle)
    {
        for (const AVehiclePawn* Vehicle : SpawnedVehicles)
        {
            if (IsValid(Vehicle))
            {
                Count++;
            }
        }
    }
    return Count;
}

FString UGameManagerSubSystem::MakeObjectName(const FString& BaseName, EPlacedObjectKind Kind) const
{
    const FString SafeBaseName = BaseName.IsEmpty() ? TEXT("GeneratedEntity") : BaseName;
    const int32 ExistingCount = CountExistingBaseName(SafeBaseName, Kind);
    if (ExistingCount <= 0)
    {
        return SafeBaseName;
    }
    if (ExistingCount == 1)
    {
        return SafeBaseName + TEXT(";INST");
    }
    return FString::Printf(TEXT("%s;INST_%d"), *SafeBaseName, ExistingCount);
}
void UGameManagerSubSystem::InputPrimaryAction()
{
    InputPrimaryPressed();
    InputPrimaryReleased();
}

void UGameManagerSubSystem::InputPrimaryPressed()
{
    if (bItemListWindowOpen)
    {
        return;
    }

    FHitResult Hit;
    FVector Location;
    const bool bHasPlacementLocation = TracePlacementLocation(Location, Hit);
    const FToolbarItem Item = GetSelectedToolbarItem();
    const bool bVehiclePlacementRequest = CurrentMode != EToolMode::EditVertices
        && (CurrentMode == EToolMode::PlaceVehicle || Item.Kind == EToolbarItemKind::Vehicle);
    if (!bVehiclePlacementRequest)
    {
        Location = ApplyGridSnap(Location);
    }

    if (CurrentMode == EToolMode::EditVertices && IsValid(CurrentEditableActor) && !CurrentEditableActor->IsFinalized())
    {
        if (HighlightedEditableVertexIndex != INDEX_NONE)
        {
            BeginEditableVertexPrimaryPress(HighlightedEditableVertexIndex);
        }
        else
        {
            if (!bHasPlacementLocation)
            {
                LastSaveMessage = TEXT("정점을 찍을 중앙 십자가 위치를 계산할 수 없습니다.");
                NotifyStateChanged();
                return;
            }
            AddVertexToEditableObject(Location);
        }
        NotifyStateChanged();
        return;
    }

    switch (Item.Kind)
    {
    case EToolbarItemKind::CreateObject:
        // Object-creation item activation is shared by LMB and RMB so either click enters vertex edit in one action.
        TryUseObjectCreationItemAtCrosshair();
        break;
    case EToolbarItemKind::Prefab:
        if (bHasPlacementLocation)
        {
            PlaceCurrentPrefab(Location);
        }
        else
        {
            LastSaveMessage = TEXT("Prefab을 설치할 중앙 십자가 위치를 계산할 수 없습니다.");
        }
        break;
    case EToolbarItemKind::Vehicle:
        if (bHasPlacementLocation)
        {
            PlaceVehicle(Location, Item.SourcePath);
        }
        else
        {
            LastSaveMessage = TEXT("차량을 설치할 중앙 십자가 위치를 계산할 수 없습니다.");
        }
        break;
    case EToolbarItemKind::Weapon:
        if (IsValid(EquippedWeapon))
        {
            APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
            EquippedWeapon->Fire(PC);
        }
        else
        {
            EquipCurrentWeapon();
        }
        break;
    case EToolbarItemKind::None:
    default:
        if (CurrentMode == EToolMode::PlacePrefab)
        {
            if (bHasPlacementLocation)
            {
                PlaceCurrentPrefab(Location);
            }
            else
            {
                LastSaveMessage = TEXT("Prefab을 설치할 중앙 십자가 위치를 계산할 수 없습니다.");
            }
        }
        else if (CurrentMode == EToolMode::PlaceVehicle)
        {
            if (bHasPlacementLocation)
            {
                PlaceVehicle(Location, GetSelectedToolbarItem().SourcePath);
            }
            else
            {
                LastSaveMessage = TEXT("차량을 설치할 중앙 십자가 위치를 계산할 수 없습니다.");
            }
        }
        else if (IsValid(EquippedWeapon))
        {
            APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
            EquippedWeapon->Fire(PC);
        }
        break;
    }

    NotifyStateChanged();
}

void UGameManagerSubSystem::InputPrimaryReleased()
{
    if (CurrentMode == EToolMode::EditVertices && bPrimaryVertexPressActive)
    {
        EndEditableVertexPrimaryPress();
        NotifyStateChanged();
    }
}

void UGameManagerSubSystem::InputSecondaryAction()
{
    // Do not let gameplay clicks leak through while the Blueprint item list is open.
    if (bItemListWindowOpen)
    {
        return;
    }

    // If a live editable mesh exists, RMB is always the one-click finish/cancel command, even if a stale mode flag says otherwise.
    if (IsValid(CurrentEditableActor) && !CurrentEditableActor->IsFinalized())
    {
        FinishOrCancelCurrentVertexEditing();
        NotifyStateChanged();
        return;
    }

    // In object-placement mode, RMB now mirrors LMB so placing the object and entering vertex mode takes exactly one click.
    if (CurrentMode == EToolMode::PlaceEmptyObject && IsSelectedToolbarItemObjectCreation())
    {
        TryUseObjectCreationItemAtCrosshair();
        NotifyStateChanged();
        return;
    }

    // Other tools currently do not use secondary input, but still broadcast so UI status text can refresh consistently.
    NotifyStateChanged();
}

void UGameManagerSubSystem::InputInteractAction()
{
    TryEnterOrExitVehicle();
}

void UGameManagerSubSystem::InputToggleFirstPersonAction()
{
    ToggleFirstPerson();
}

void UGameManagerSubSystem::InputToolbarScrollAction(float ScrollValue)
{
    ScrollToolbarSelection(ScrollValue);
}

void UGameManagerSubSystem::InputToggleItemListAction()
{
    ToggleItemListWindow();
}

void UGameManagerSubSystem::InputToggleSnapModeAction()
{
    ToggleSnap();
}

void UGameManagerSubSystem::InputVehicleMoveAction(const FVector2D& MoveValue)
{
    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (AVehiclePawn* Vehicle = PC ? Cast<AVehiclePawn>(PC->GetPawn()) : nullptr)
    {
        Vehicle->SetDriveInput(MoveValue.Y, MoveValue.X);
    }
}

void UGameManagerSubSystem::InputVehicleThrottleAction(float Throttle)
{
    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (AVehiclePawn* Vehicle = PC ? Cast<AVehiclePawn>(PC->GetPawn()) : nullptr)
    {
        Vehicle->SetThrottleInput(Throttle);
    }
}

void UGameManagerSubSystem::InputVehicleSteeringAction(float Steering)
{
    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (AVehiclePawn* Vehicle = PC ? Cast<AVehiclePawn>(PC->GetPawn()) : nullptr)
    {
        Vehicle->SetSteeringInput(Steering);
    }
}

void UGameManagerSubSystem::SelectCurrentTraceLocation()
{
    InputPrimaryAction();
}

void UGameManagerSubSystem::ConfirmCurrentPendingLocation()
{
    InputSecondaryAction();
}

bool UGameManagerSubSystem::TryUseObjectCreationItemAtCrosshair()
{
    if (!bEnableObjectVertexCreation)
    {
        DestroyPendingEmptyObjectPreview();
        CurrentMode = EToolMode::None;
        LastSaveMessage = TEXT("오브젝트 정점 생성 기능은 현재 임시 비활성화되어 있습니다.");
        return false;
    }

    // Compute the current center-crosshair placement point at the exact moment the click happens.
    FHitResult Hit;

    // This location can be either a short collision hit surface or the configured free-space point.
    FVector Location = FVector::ZeroVector;

    // TracePlacementLocation also refreshes LastTraceHit, LastTraceStart, and LastTraceDirection for selection feedback.
    const bool bHasPlacementLocation = TracePlacementLocation(Location, Hit);

    // Grid snapping applies equally to surface placement and air placement.
    Location = ApplyGridSnap(Location);

    // Looking at an existing finalized runtime mesh with the object item means "edit this mesh" instead of "spawn a new one".
    if (AEditableMeshActor* ExistingMesh = GetEditableMeshFromHit(Hit))
    {
        // BeginEditingExistingMesh stores a rollback copy, removes the mesh from the finalized list, and switches to EditVertices.
        BeginEditingExistingMesh(ExistingMesh);

        // The click was consumed successfully.
        return true;
    }

    // If neither a surface nor a free-space point exists, no object can be created safely.
    if (!bHasPlacementLocation)
    {
        // Keep this message Blueprint-readable for the custom toolbar/status widget.
        LastSaveMessage = TEXT("오브젝트를 만들 중앙 십자가 위치를 계산할 수 없습니다.");

        // Nothing was placed.
        return false;
    }

    // Reuse the current translucent object-creation preview actor as the real editable object when possible.
    AEditableMeshActor* PreviewActor = PendingEmptyObjectPreviewActor.Get();

    // Detach the pointer before PlaceEmptyObject converts/destroys/owns the actor so the cursor preview cannot become a stale ghost.
    PendingEmptyObjectPreviewActor = nullptr;

    // Spawn or convert the object and make it the active editable mesh.
    PlaceEmptyObject(Location, PreviewActor);

    // Force the expected one-click state: after object creation, the next click must add/edit vertices immediately.
    if (IsValid(CurrentEditableActor) && !CurrentEditableActor->IsFinalized())
    {
        // The mode assignment is duplicated intentionally to protect against stale mode changes caused by UI event order.
        CurrentMode = EToolMode::EditVertices;

        // Keep the cached preview location in sync with the spawn location so the first vertex preview appears immediately.
        LastPreviewLocation = Location;

        // Make the newly active actor show the center-crosshair vertex preview without waiting for a second click/tick.
        UpdateEditableVertexPreviewAndSelection();

        // Tell UI code that the one-click transition succeeded.
        LastSaveMessage += TEXT(" 정점 편집 모드로 즉시 전환되었습니다.");

        // Successfully entered edit mode.
        return true;
    }

    // PlaceEmptyObject failed to create a valid actor.
    return false;
}

bool UGameManagerSubSystem::CloseCurrentEditableMeshForToolChange()
{
    // No live actor means there is nothing for the mode change to resolve.
    if (!IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized())
    {
        // Clear stale click/drag flags anyway so a previous input cannot affect the next tool.
        ClearEditableVertexMoveState();

        // The toolbar switch may proceed normally.
        return true;
    }

    // FinishCurrentEditableMesh finalizes valid meshes and cancels/reverts invalid or one/two-vertex edits.
    const bool bClosedAsValidMesh = FinishCurrentEditableMesh();

    // Always clear press/drag/source flags after a tool switch so the cursor cannot remain attached to an old edit actor.
    ClearEditableVertexMoveState();

    // Return whether the edit became a valid finalized mesh; callers currently use the cleanup side effect either way.
    return bClosedAsValidMesh;
}

void UGameManagerSubSystem::PlaceCurrentPrefab(const FVector& Location)
{
    ScanAssetFolders();
    if (!PrefabFiles.IsValidIndex(CurrentPrefabIndex))
    {
        LastSaveMessage = TEXT("prefab/ 폴더에 gltf 또는 glb가 없습니다.");
        return;
    }

    const FString SourceFile = PrefabFiles[CurrentPrefabIndex];
    const FString BaseName = FPaths::GetBaseFilename(SourceFile);
    const FString ObjectName = MakeObjectName(BaseName, EPlacedObjectKind::Prefab);

    FActorSpawnParameters Params;
    Params.Owner = ConfigActor.Get();
    const FRotator SpawnRot = FRotator(0.0f, GetWorld()->GetFirstPlayerController() ? GetWorld()->GetFirstPlayerController()->GetControlRotation().Yaw : 0.0f, 0.0f);
    UClass* PrefabSpawnClass = PrefabActorClass ? PrefabActorClass.Get() : APrefabActor::StaticClass();
    APrefabActor* Actor = GetWorld()->SpawnActor<APrefabActor>(PrefabSpawnClass, FTransform(SpawnRot, Location), Params);
    if (IsValid(Actor))
    {
        Actor->SetRenderOnlyMode(UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this));
    }
    if (IsValid(Actor) && Actor->LoadPrefab(SourceFile, ObjectName))
    {
        SpawnedPrefabs.Add(Actor);
        LastSaveMessage = FString::Printf(TEXT("설치됨: %s"), *ObjectName);
    }
    else if (IsValid(Actor))
    {
        Actor->Destroy();
        LastSaveMessage = TEXT("Prefab 로드 실패");
    }
}

void UGameManagerSubSystem::PlaceEmptyObject(const FVector& Location, AEditableMeshActor* ExistingPreviewActor)
{
    if (IsValid(CurrentEditableActor) && !CurrentEditableActor->IsFinalized())
    {
        LastSaveMessage = TEXT("이미 편집 중인 오브젝트가 있습니다. 우클릭으로 먼저 편집을 끝내세요.");
        return;
    }

    const FString ObjectName = MakeObjectName(TEXT("GeneratedMesh"), EPlacedObjectKind::GeneratedMesh);
    AEditableMeshActor* Actor = ExistingPreviewActor;
    if (!IsValid(Actor))
    {
        FActorSpawnParameters Params;
        Params.Owner = ConfigActor.Get();
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        UClass* EditableSpawnClass = EditableMeshActorClass ? EditableMeshActorClass.Get() : AEditableMeshActor::StaticClass();
        Actor = GetWorld()->SpawnActor<AEditableMeshActor>(EditableSpawnClass, FTransform(Location), Params);
    }
    else
    {
        Actor->SetActorLocation(Location);
    }

    if (IsValid(Actor))
    {
        Actor->BeginObject(ObjectName);
        Actor->SetActorHiddenInGame(false);
        Actor->SetActorEnableCollision(true);
        CurrentEditableActor = Actor;
        CurrentMode = EToolMode::EditVertices;
        bCurrentEditableActorWasExisting = false;
        bHasOriginalEditableMeshRecord = false;
        ClearEditableVertexMoveState();
        // Make the actor display a first candidate vertex at the object spawn/crosshair point immediately.
        Actor->SetPreviewVertexWorld(Location);
        LastSaveMessage = FString::Printf(TEXT("오브젝트 생성: %s. 빈 공간 좌클릭=정점 추가, 기존 정점 짧은 클릭=연결 정점 생성 모드, 기존 정점 누른 채 이동=위치 편집, 우클릭=완료/취소"), *ObjectName);
    }
}

AEditableMeshActor* UGameManagerSubSystem::GetEditableMeshFromHit(const FHitResult& Hit) const
{
    AEditableMeshActor* MeshActor = Cast<AEditableMeshActor>(Hit.GetActor());
    if (!IsValid(MeshActor) && Hit.GetComponent())
    {
        MeshActor = Cast<AEditableMeshActor>(Hit.GetComponent()->GetOwner());
    }

    if (IsValid(MeshActor) && MeshActor != PendingEmptyObjectPreviewActor.Get() && MeshActor->IsFinalized())
    {
        return MeshActor;
    }
    return nullptr;
}

void UGameManagerSubSystem::BeginEditingExistingMesh(AEditableMeshActor* MeshActor)
{
    if (!IsValid(MeshActor))
    {
        return;
    }

    DestroyPendingEmptyObjectPreview();
    if (IsValid(CurrentEditableActor) && !CurrentEditableActor->IsFinalized() && CurrentEditableActor != MeshActor)
    {
        FinishOrCancelCurrentVertexEditing();
    }

    CurrentEditableActor = MeshActor;
    OriginalEditableMeshRecord = MeshActor->ToGeneratedMeshRecord();
    bHasOriginalEditableMeshRecord = true;
    bCurrentEditableActorWasExisting = true;
    SpawnedGeneratedMeshes.Remove(MeshActor);
    MeshActor->BeginEditingExistingObject();
    CurrentMode = EToolMode::EditVertices;
    ClearEditableVertexMoveState();
    LastSaveMessage = FString::Printf(TEXT("기존 메시 편집 시작: %s. 빈 공간 좌클릭=정점 추가, 기존 정점 짧은 클릭=연결 정점 생성 모드, 기존 정점 누른 채 이동=위치 편집, 우클릭=완료"), *MeshActor->GetObjectName());
    NotifyStateChanged();
}

void UGameManagerSubSystem::AddVertexToEditableObject(const FVector& Location)
{
    // Guard against clicks when no runtime mesh is open for editing.
    if (!IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized())
    {
        // Store a BP-readable message instead of silently ignoring the click.
        LastSaveMessage = TEXT("편집할 오브젝트가 없습니다.");

        // Stop because there is no actor that can receive a new vertex.
        return;
    }

    // If gameplay state has a connected source but the actor does not, resync the actor before adding the next vertex.
    if (ConnectedEditableVertexSourceIndex != INDEX_NONE && CurrentEditableActor->GetConnectedVertexSourceIndex() != ConnectedEditableVertexSourceIndex)
    {
        // Ask the actor to seed its active polygon chain from the selected source vertex.
        if (!CurrentEditableActor->StartConnectedVertexFromIndex(ConnectedEditableVertexSourceIndex))
        {
            // Drop stale source indices that no longer exist on the actor.
            ConnectedEditableVertexSourceIndex = INDEX_NONE;
        }
    }

    // Remember the source before adding, so the user message can say what this segment extended from.
    const int32 PreviousConnectedSourceIndex = ConnectedEditableVertexSourceIndex;

    // Add a brand-new vertex at the center-crosshair hit location and let the actor extend the current n-gon.
    const int32 NewIndex = CurrentEditableActor->AddVertexWorld(Location);

    // Pull the actor's new source index back into manager state; this normally becomes the newly added vertex.
    ConnectedEditableVertexSourceIndex = CurrentEditableActor->GetConnectedVertexSourceIndex();

    // Highlight the new point so feedback follows the actual chain end.
    HighlightedEditableVertexIndex = NewIndex;

    // Adding a vertex is not a drag operation.
    bMovingHighlightedEditableVertex = false;

    // A committed click ends any click-vs-hold classification that may have been active.
    bPrimaryVertexPressActive = false;

    // A committed click also cancels drag mode.
    bPrimaryVertexDragActive = false;

    // No vertex is currently being pressed after a committed add.
    PressedEditableVertexIndex = INDEX_NONE;

    // Read topology after the actor has rebuilt its fan triangles.
    const bool bTopologyValid = CurrentEditableActor->IsTopologyValid();

    // Connected-source messages are more useful when the user is extending from an existing point.
    if (PreviousConnectedSourceIndex != INDEX_NONE)
    {
        // Tell the user that the new point continued the chain from a specific source vertex.
        LastSaveMessage = FString::Printf(TEXT("정점 %d 추가: 정점 %d에서 이어지는 n-gon 선분입니다. 현재 위상: %s."), NewIndex, PreviousConnectedSourceIndex, bTopologyValid ? TEXT("정상(초록)") : TEXT("미완성/이상(빨강)"));
    }
    else
    {
        // Ensure the actor highlight matches the new vertex instead of clearing all feedback.
        CurrentEditableActor->SetHighlightedVertexIndex(NewIndex, false);

        // Tell the user that the polygon remains open for additional points beyond triangles/quads.
        LastSaveMessage = FString::Printf(TEXT("정점 %d 추가. 계속 좌클릭하면 삼각형/사각형 이후에도 같은 면에 정점이 이어집니다. 현재 위상: %s."), NewIndex, bTopologyValid ? TEXT("정상(초록)") : TEXT("미완성/이상(빨강)"));
    }
}

bool UGameManagerSubSystem::AddExistingVertexToEditableObject(int32 ExistingVertexIndex)
{
    // Guard against merge clicks when no editable mesh is active.
    if (!IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized())
    {
        // Store a clear message for Blueprint UI status text.
        LastSaveMessage = TEXT("병합할 편집 중 오브젝트가 없습니다.");

        // Stop because the merge target has nowhere to go.
        return false;
    }

    // Remember the source before merging so the status message can describe the new segment.
    const int32 PreviousConnectedSourceIndex = ConnectedEditableVertexSourceIndex;

    // Ask the actor to append the existing vertex index to the active polygon without duplicating the vertex.
    if (!CurrentEditableActor->AddExistingVertexToCurrentFace(ExistingVertexIndex))
    {
        // Invalid merges include same-vertex edges or repeated non-first vertices inside the active n-gon.
        LastSaveMessage = FString::Printf(TEXT("정점 %d 병합 실패: 같은 선분 또는 중복 정점으로 인해 위상이 꼬일 수 있습니다."), ExistingVertexIndex);

        // Keep current highlights unchanged so the user can see the rejected target.
        return false;
    }

    // Mirror the actor's connected source; after a merge this becomes the merged target vertex.
    ConnectedEditableVertexSourceIndex = CurrentEditableActor->GetConnectedVertexSourceIndex();

    // Keep manager selection state aligned with the merged target.
    HighlightedEditableVertexIndex = ExistingVertexIndex;

    // A merge click is a short click, not a drag move.
    bMovingHighlightedEditableVertex = false;

    // The press classification has completed by the time this merge is called.
    bPrimaryVertexPressActive = false;

    // A successful merge cannot simultaneously be a drag.
    bPrimaryVertexDragActive = false;

    // Clear the pressed vertex because no button is currently held.
    PressedEditableVertexIndex = INDEX_NONE;

    // Check the rebuilt face fan so the message matches the red/green preview.
    const bool bTopologyValid = CurrentEditableActor->IsTopologyValid();

    // Use a source-aware message when the segment came from a previously selected vertex.
    if (PreviousConnectedSourceIndex != INDEX_NONE && PreviousConnectedSourceIndex != ExistingVertexIndex)
    {
        // Tell the user that no duplicate point was created; the existing target was reused.
        LastSaveMessage = FString::Printf(TEXT("정점 %d → 정점 %d 병합 연결 완료. 기존 정점을 재사용했습니다. 현재 위상: %s."), PreviousConnectedSourceIndex, ExistingVertexIndex, bTopologyValid ? TEXT("정상(초록)") : TEXT("미완성/이상(빨강)"));
    }
    else
    {
        // Cover the case where the first vertex is selected as the starting/closing vertex.
        LastSaveMessage = FString::Printf(TEXT("정점 %d을 현재 n-gon 체인에 병합했습니다. 현재 위상: %s."), ExistingVertexIndex, bTopologyValid ? TEXT("정상(초록)") : TEXT("미완성/이상(빨강)"));
    }

    // True lets the release handler know that it does not need to fall back to source-selection mode.
    return true;
}

void UGameManagerSubSystem::BeginEditableVertexPrimaryPress(int32 VertexIndex)
{
    if (!IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized())
    {
        return;
    }

    bPrimaryVertexPressActive = true;
    bPrimaryVertexDragActive = false;
    bMovingHighlightedEditableVertex = false;
    PressedEditableVertexIndex = VertexIndex;
    HighlightedEditableVertexIndex = VertexIndex;
    PrimaryVertexPressStartTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
    PrimaryVertexPressStartLocation = CurrentEditableActor->GetVertexWorldLocation(VertexIndex);
    CurrentEditableActor->SetHighlightedVertexIndex(VertexIndex, false);
    CurrentEditableActor->ClearPreviewVertex();
    LastSaveMessage = FString::Printf(TEXT("정점 %d 선택: 짧게 떼면 연결 정점 생성 모드, 누른 채 중앙 십자가를 움직이면 정점 위치 편집."), VertexIndex);
}

void UGameManagerSubSystem::UpdateEditableVertexPrimaryPressDrag()
{
    if (!bPrimaryVertexPressActive || !IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized() || PressedEditableVertexIndex == INDEX_NONE)
    {
        return;
    }

    const double NowSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : PrimaryVertexPressStartTime;
    const double HeldSeconds = NowSeconds - PrimaryVertexPressStartTime;
    const float MoveDistance = bLastTraceHasPlacementLocation ? FVector::Dist(LastPreviewLocation, PrimaryVertexPressStartLocation) : 0.0f;

    if (!bPrimaryVertexDragActive && HeldSeconds >= FMath::Max(0.0f, VertexDragHoldSeconds) && MoveDistance >= FMath::Max(1.0f, VertexDragStartDistance))
    {
        bPrimaryVertexDragActive = true;
        bMovingHighlightedEditableVertex = true;
        ClearConnectedVertexCreationState();
        LastSaveMessage = FString::Printf(TEXT("정점 %d 위치 편집 중: 버튼을 떼면 현재 위치로 확정됩니다."), PressedEditableVertexIndex);
        NotifyStateChanged();
    }

    if (bPrimaryVertexDragActive)
    {
        if (bLastTraceHasPlacementLocation)
        {
            CurrentEditableActor->MoveVertexWorld(PressedEditableVertexIndex, LastPreviewLocation);
        }
        CurrentEditableActor->SetHighlightedVertexIndex(PressedEditableVertexIndex, true);
    }
    else
    {
        CurrentEditableActor->SetHighlightedVertexIndex(PressedEditableVertexIndex, false);
        CurrentEditableActor->ClearPreviewVertex();
    }
}

void UGameManagerSubSystem::EndEditableVertexPrimaryPress()
{
    if (!bPrimaryVertexPressActive)
    {
        return;
    }

    const int32 ReleasedVertexIndex = PressedEditableVertexIndex;
    const bool bWasDragging = bPrimaryVertexDragActive;
    bPrimaryVertexPressActive = false;
    bPrimaryVertexDragActive = false;
    bMovingHighlightedEditableVertex = false;
    PressedEditableVertexIndex = INDEX_NONE;

    if (!IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized() || ReleasedVertexIndex == INDEX_NONE)
    {
        ClearEditableVertexMoveState();
        return;
    }

    if (bWasDragging)
    {
        // A hold-and-move gesture edits the selected vertex position.
        if (bLastTraceHasPlacementLocation)
        {
            // Commit the dragged vertex to the current center-crosshair location, which can be a surface hit or a 10m air point.
            CurrentEditableActor->MoveVertexWorld(ReleasedVertexIndex, LastPreviewLocation);
        }

        // Return the vertex highlight to the normal selected color after release.
        CurrentEditableActor->SetHighlightedVertexIndex(ReleasedVertexIndex, false);

        // Keep the moved vertex selected for immediate visual confirmation.
        HighlightedEditableVertexIndex = ReleasedVertexIndex;

        // Moving a vertex does not change the edge source unless the actor had already set one.
        ConnectedEditableVertexSourceIndex = CurrentEditableActor->GetConnectedVertexSourceIndex();

        // Store a user-visible status message for Blueprint UI.
        LastSaveMessage = FString::Printf(TEXT("정점 %d 위치 변경 완료"), ReleasedVertexIndex);
    }
    else if (ConnectedEditableVertexSourceIndex != INDEX_NONE && CurrentEditableActor->GetCurrentFaceVertexCount() > 0)
    {
        // A short click on an existing vertex while a chain is active first attempts to merge/connect to that existing vertex.
        if (!AddExistingVertexToEditableObject(ReleasedVertexIndex))
        {
            // If the vertex was already part of the active face, reinterpret the click as choosing a new source vertex.
            BeginConnectedVertexCreationFromIndex(ReleasedVertexIndex);
        }
    }
    else
    {
        // A short click with no active chain means this vertex becomes the source for a new connected segment.
        BeginConnectedVertexCreationFromIndex(ReleasedVertexIndex);
    }
}

void UGameManagerSubSystem::BeginConnectedVertexCreationFromIndex(int32 VertexIndex)
{
    if (!IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized())
    {
        return;
    }

    // Ask the mesh actor to reset its active polygon chain so this vertex becomes the first point.
    if (CurrentEditableActor->StartConnectedVertexFromIndex(VertexIndex))
    {
        // Mirror the source index in manager state for input decisions and status text.
        ConnectedEditableVertexSourceIndex = VertexIndex;

        // Keep the clicked vertex selected.
        HighlightedEditableVertexIndex = VertexIndex;

        // Short-click source selection is not a move operation.
        bMovingHighlightedEditableVertex = false;

        // Tell the user they can either add a new point in space or merge into another highlighted vertex.
        LastSaveMessage = FString::Printf(TEXT("정점 %d 연결 생성 모드: 빈 곳 좌클릭=새 정점, 기존 정점에 맞춰 좌클릭=병합 연결."), VertexIndex);
    }
}

void UGameManagerSubSystem::ClearConnectedVertexCreationState()
{
    ConnectedEditableVertexSourceIndex = INDEX_NONE;
    if (IsValid(CurrentEditableActor))
    {
        CurrentEditableActor->ClearConnectedVertexSource();
    }
}

bool UGameManagerSubSystem::FinishOrCancelCurrentVertexEditing()
{
    return FinishCurrentEditableMesh();
}

bool UGameManagerSubSystem::FinishCurrentEditableMesh()
{
    if (!IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized())
    {
        LastSaveMessage = TEXT("완성할 편집 중 메시가 없습니다.");
        NotifyStateChanged();
        return false;
    }

    // Track whether the failure is specifically the user-requested point/line cancellation case before optional cleanup.
    const bool bPointOrLineOnly = CurrentEditableActor->GetVertices().Num() > 0 && CurrentEditableActor->GetVertices().Num() <= 2;

    // Track unfinished connected edits such as "existing vertex + one new vertex" that would otherwise leave an unusable cursor/line.
    const bool bDanglingPointOrLineEdit = CurrentEditableActor->HasDanglingPointOrLineEdit();

    // If a valid mesh already exists but the user leaves a one/two-vertex branch unfinished, discard only that branch.
    if (bDanglingPointOrLineEdit && CurrentEditableActor->HasAnyTriangle() && CurrentEditableActor->IsTopologyValid())
    {
        // This keeps completed faces while preventing orphan vertices from becoming part of the finalized object.
        CurrentEditableActor->DiscardDanglingPointOrLineEdit();
    }

    // A real finalized object needs at least one triangle, valid triangle topology, and no remaining dangling one/two-vertex edit chain.
    const bool bHasValidFace = CurrentEditableActor->CanFinalizeAsObject();

    if (!bHasValidFace)
    {
        // Give a clearer message when the edit contains only one/two vertices or an unfinished one/two-vertex branch.
        const bool bCanceledBecausePointOrLine = bPointOrLineOnly || bDanglingPointOrLineEdit;

        if (bCurrentEditableActorWasExisting && bHasOriginalEditableMeshRecord)
        {
            CurrentEditableActor->LoadFromGeneratedMeshRecord(OriginalEditableMeshRecord);
            CurrentEditableActor->SetActorHiddenInGame(false);
            SpawnedGeneratedMeshes.AddUnique(CurrentEditableActor);
            LastSaveMessage = bCanceledBecausePointOrLine
                ? TEXT("정점 1~2개짜리 미완성 편집이어서 기존 메시 편집을 되돌렸습니다.")
                : TEXT("면이 형성되지 않았거나 위상이 올바르지 않아 기존 메시 편집을 되돌렸습니다.");
            CurrentEditableActor = nullptr;
        }
        else
        {
            AEditableMeshActor* ActorToDestroy = CurrentEditableActor.Get();
            CurrentEditableActor = nullptr;
            if (IsValid(ActorToDestroy))
            {
                ActorToDestroy->Destroy();
            }
            LastSaveMessage = bCanceledBecausePointOrLine
                ? TEXT("정점 1~2개만 있는 오브젝트는 생성하지 않고 취소했습니다.")
                : TEXT("면이 형성되지 않았거나 위상이 올바르지 않아 오브젝트 생성을 취소했습니다.");
        }

        CurrentMode = IsSelectedToolbarItemObjectCreation() ? EToolMode::PlaceEmptyObject : EToolMode::None;
        ClearEditableVertexMoveState();
        bCurrentEditableActorWasExisting = false;
        bHasOriginalEditableMeshRecord = false;
        NotifyStateChanged();
        return false;
    }

    CurrentEditableActor->ClearPreviewVertex();
    CurrentEditableActor->SetHighlightedVertexIndex(INDEX_NONE, false);
    CurrentEditableActor->FinalizeObject();
    CurrentEditableActor->SetActorHiddenInGame(false);
    SpawnedGeneratedMeshes.AddUnique(CurrentEditableActor);
    CurrentEditableActor = nullptr;
    CurrentMode = IsSelectedToolbarItemObjectCreation() ? EToolMode::PlaceEmptyObject : EToolMode::None;
    ClearEditableVertexMoveState();
    bCurrentEditableActorWasExisting = false;
    bHasOriginalEditableMeshRecord = false;
    LastSaveMessage = TEXT("메시 편집 완료: 완성된 메시가 월드에 노출됩니다. 오브젝트 만들기 아이템으로 다시 좌클릭하면 편집할 수 있습니다.");
    NotifyStateChanged();
    return true;
}

void UGameManagerSubSystem::CancelCurrentEditableMesh(bool bDestroyActor)
{
    DestroyPendingEmptyObjectPreview();
    if (IsValid(CurrentEditableActor))
    {
        if (bCurrentEditableActorWasExisting && bHasOriginalEditableMeshRecord)
        {
            CurrentEditableActor->LoadFromGeneratedMeshRecord(OriginalEditableMeshRecord);
            SpawnedGeneratedMeshes.AddUnique(CurrentEditableActor);
        }
        else if (bDestroyActor)
        {
            CurrentEditableActor->Destroy();
        }
        CurrentEditableActor = nullptr;
    }
    CurrentMode = IsSelectedToolbarItemObjectCreation() ? EToolMode::PlaceEmptyObject : EToolMode::None;
    ClearEditableVertexMoveState();
    bCurrentEditableActorWasExisting = false;
    bHasOriginalEditableMeshRecord = false;
    LastSaveMessage = bDestroyActor ? TEXT("편집 중 메시를 취소했습니다.") : TEXT("편집 중 메시를 종료했습니다.");
    NotifyStateChanged();
}

void UGameManagerSubSystem::PlaceVehicle(const FVector& Location, const FString& SourceFile)
{
    FActorSpawnParameters Params;
    Params.Owner = ConfigActor.Get();
    const FRotator SpawnRot = FRotator(0.0f, GetWorld()->GetFirstPlayerController() ? GetWorld()->GetFirstPlayerController()->GetControlRotation().Yaw : 0.0f, 0.0f);
    UClass* VehicleSpawnClass = VehiclePawnClass ? VehiclePawnClass.Get() : AVehiclePawn::StaticClass();
    const FVector SpawnLocation = Location + FVector(0.0f, 0.0f, 220.0f);
    AVehiclePawn* Vehicle = GetWorld()->SpawnActor<AVehiclePawn>(VehicleSpawnClass, FTransform(SpawnRot, SpawnLocation), Params);
    if (IsValid(Vehicle))
    {
        if (!SourceFile.IsEmpty())
        {
            Vehicle->LoadVehicleModel(SourceFile, FPaths::GetBaseFilename(SourceFile));
        }
        Vehicle->ResetVehiclePoseAboveGround();
        SpawnedVehicles.Add(Vehicle);
        LastSaveMessage = SourceFile.IsEmpty() ? TEXT("기본 자동차 설치됨. F키로 탑승하세요.") : TEXT("glTF 자동차 설치됨. F키로 탑승하세요.");
    }
}

void UGameManagerSubSystem::TryEnterOrExitVehicle()
{
    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (!IsValid(PC))
    {
        return;
    }

    if (AVehiclePawn* CurrentVehicle = Cast<AVehiclePawn>(PC->GetPawn()))
    {
        CurrentVehicle->ExitVehicle();
        return;
    }

    APawn* CurrentPawn = PC->GetPawn();
    if (ACharacterController* CharacterPawn = Cast<ACharacterController>(CurrentPawn))
    {
        if (UCharacterComponent* CharacterState = CharacterPawn->GetCharacterComponent())
        {
            if (CharacterState->IsRagdollActive() || CharacterState->IsGettingUp())
            {
                LastSaveMessage = TEXT("레그돌 상태에서는 차량에 탑승할 수 없습니다.");
                NotifyStateChanged();
                return;
            }
        }
    }

    const FVector Origin = IsValid(CurrentPawn) ? CurrentPawn->GetActorLocation() : PC->PlayerCameraManager->GetCameraLocation();
    AVehiclePawn* BestVehicle = nullptr;
    float BestDistSq = FMath::Square(VehicleEnterDistance);

    for (AVehiclePawn* Vehicle : SpawnedVehicles)
    {
        if (!IsValid(Vehicle) || Vehicle->IsOccupied())
        {
            continue;
        }
        const float DistSq = FVector::DistSquared(Origin, Vehicle->GetActorLocation());
        if (DistSq < BestDistSq)
        {
            BestDistSq = DistSq;
            BestVehicle = Vehicle;
        }
    }

    if (IsValid(BestVehicle) && !BestVehicle->EnterVehicle(PC, CurrentPawn))
    {
        LastSaveMessage = TEXT("현재 상태에서는 차량에 탑승할 수 없습니다.");
        NotifyStateChanged();
    }
}

void UGameManagerSubSystem::CollectSceneRecords(TArray<FPlacedObjectRecord>& OutPlaced, TArray<FGeneratedMeshRecord>& OutMeshes) const
{
    OutPlaced.Empty();
    OutMeshes.Empty();

    for (const APrefabActor* Prefab : SpawnedPrefabs)
    {
        if (IsValid(Prefab))
        {
            OutPlaced.Add(Prefab->ToPlacementRecord());
        }
    }

    int32 VehicleRecordIndex = 0;
    for (const AVehiclePawn* Vehicle : SpawnedVehicles)
    {
        if (IsValid(Vehicle))
        {
            OutPlaced.Add(Vehicle->ToPlacementRecord(VehicleRecordIndex));
            VehicleRecordIndex++;
        }
    }

    if (IsValid(CurrentEditableActor) && CurrentEditableActor->IsFinalized())
    {
        OutMeshes.Add(CurrentEditableActor->ToGeneratedMeshRecord());
    }
    for (const AEditableMeshActor* Mesh : SpawnedGeneratedMeshes)
    {
        if (IsValid(Mesh) && Mesh->IsFinalized())
        {
            OutMeshes.Add(Mesh->ToGeneratedMeshRecord());
        }
    }
}

bool UGameManagerSubSystem::SaveScene()
{
    if (bIsSavingScene)
    {
        return false;
    }
    bIsSavingScene = true;

    bool bSkippedIncompleteEditableMesh = false;
    if (IsValid(CurrentEditableActor) && !CurrentEditableActor->IsFinalized())
    {
        // Saving uses the same one/two-vertex cleanup rule as right-click completion.
        if (CurrentEditableActor->HasDanglingPointOrLineEdit() && CurrentEditableActor->HasAnyTriangle() && CurrentEditableActor->IsTopologyValid())
        {
            // Keep completed faces but drop an unfinished dangling point/line before finalizing for save.
            CurrentEditableActor->DiscardDanglingPointOrLineEdit();
        }

        // Only meshes that can become real world objects are finalized and exported.
        if (CurrentEditableActor->CanFinalizeAsObject())
        {
            // Remove edit-only preview geometry.
            CurrentEditableActor->ClearPreviewVertex();

            // Convert the procedural mesh into its finalized/collidable state.
            CurrentEditableActor->FinalizeObject();

            // Make sure the finalized mesh remains visible in the world.
            CurrentEditableActor->SetActorHiddenInGame(false);

            // Track the generated mesh for future save/export passes.
            SpawnedGeneratedMeshes.AddUnique(CurrentEditableActor);

            // Clear the active edit pointer because the mesh is now a finalized world object.
            CurrentEditableActor = nullptr;

            // Return to object-placement mode only when the object-creation item is still selected.
            CurrentMode = IsSelectedToolbarItemObjectCreation() ? EToolMode::PlaceEmptyObject : EToolMode::None;
        }
        else
        {
            // Leave the active actor out of the save when it is still just a point/line or invalid topology.
            bSkippedIncompleteEditableMesh = true;
        }
    }

    TArray<FPlacedObjectRecord> Placed;
    TArray<FGeneratedMeshRecord> Meshes;
    CollectSceneRecords(Placed, Meshes);
    const FString ManifestPath = GetManifestPath();
    const bool bSaved = UGLTFSaveLibrary::SaveScene(this, Placed, Meshes, ManifestPath, FString());
    if (bSaved)
    {
        UE_LOG(LogTemp, Verbose, TEXT("scene saved: %s%s"), *ManifestPath, bSkippedIncompleteEditableMesh ? TEXT(" (skipped incomplete editable mesh)") : TEXT(""));
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("scene save failed."));
    }
    NotifyStateChanged();
    bIsSavingScene = false;
    return bSaved;
}

bool UGameManagerSubSystem::LoadSavedScene()
{
    if (bSavedSceneLoaded)
    {
        return false;
    }
    bSavedSceneLoaded = true;

    if (const UWorld* World = GetWorld())
    {
        if (World->GetNetMode() == NM_Client)
        {
            // Clients receive gameplay actors from the authoritative server.
            // They still stream the selected GLB world render-only through StartMainWorldStreaming(),
            // but must not spawn a second local gameplay/collision scene from the manifest.
            return false;
        }
    }

    TArray<FPlacedObjectRecord> Placed;
    TArray<FGeneratedMeshRecord> Meshes;
    FString LoadedManifestPath = GetManifestPath();
    if (!UGLTFSaveLibrary::LoadScene(LoadedManifestPath, Placed, Meshes))
    {
        LoadedManifestPath = GetLegacyManifestPath();
        if (!UGLTFSaveLibrary::LoadScene(LoadedManifestPath, Placed, Meshes))
        {
            return false;
        }
    }

    FActorSpawnParameters Params;
    Params.Owner = ConfigActor.Get();
    for (const FGeneratedMeshRecord& MeshRecord : Meshes)
    {
        UClass* EditableSpawnClass = EditableMeshActorClass ? EditableMeshActorClass.Get() : AEditableMeshActor::StaticClass();
        AEditableMeshActor* MeshActor = GetWorld()->SpawnActor<AEditableMeshActor>(EditableSpawnClass, MeshRecord.Transform, Params);
        if (IsValid(MeshActor))
        {
            MeshActor->LoadFromGeneratedMeshRecord(MeshRecord);
            SpawnedGeneratedMeshes.Add(MeshActor);
        }
    }

    for (const FPlacedObjectRecord& Record : Placed)
    {
        if (Record.Kind == EPlacedObjectKind::Prefab)
        {
            UClass* PrefabSpawnClass = PrefabActorClass ? PrefabActorClass.Get() : APrefabActor::StaticClass();
            APrefabActor* Prefab = GetWorld()->SpawnActor<APrefabActor>(PrefabSpawnClass, Record.Transform, Params);
            if (IsValid(Prefab))
            {
                Prefab->SetRenderOnlyMode(UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this));
            }
            if (IsValid(Prefab) && Prefab->LoadPrefab(Record.SourceFile, Record.ObjectName))
            {
                SpawnedPrefabs.Add(Prefab);
            }
            else if (IsValid(Prefab))
            {
                Prefab->Destroy();
            }
        }
        else if (Record.Kind == EPlacedObjectKind::Vehicle)
        {
            UClass* VehicleSpawnClass = VehiclePawnClass ? VehiclePawnClass.Get() : AVehiclePawn::StaticClass();
            AVehiclePawn* Vehicle = GetWorld()->SpawnActor<AVehiclePawn>(VehicleSpawnClass, Record.Transform, Params);
            if (IsValid(Vehicle))
            {
                if (!Record.SourceFile.IsEmpty())
                {
                    Vehicle->LoadVehicleModel(Record.SourceFile, Record.ObjectName);
                }
                Vehicle->ResetVehiclePoseAboveGround();
                SpawnedVehicles.Add(Vehicle);
            }
        }
    }
    UE_LOG(LogTemp, Verbose, TEXT("Loaded runtime entity manifest: %s"), *LoadedManifestPath);
    NotifyStateChanged();
    return true;
}

void UGameManagerSubSystem::RefreshAssetLists()
{
    ScanAssetFolders();
    BuildAvailableItems();
    InitializeToolbarSlotsIfNeeded();
    LastSaveMessage = TEXT("Asset lists refreshed.");
    NotifyToolbarChanged();
    NotifyStateChanged();
}

void UGameManagerSubSystem::SetCurrentToolMode(EToolMode NewMode)
{
    switch (NewMode)
    {
    case EToolMode::PlacePrefab:
        SelectPrefabPlacementTool();
        break;
    case EToolMode::PlaceEmptyObject:
        SelectEmptyObjectTool();
        break;
    case EToolMode::EditVertices:
        SelectVertexTool();
        break;
    case EToolMode::PlaceVehicle:
        SelectVehicleTool();
        break;
    case EToolMode::Weapon:
        EquipCurrentWeapon();
        break;
    case EToolMode::None:
    default:
        CurrentMode = EToolMode::None;
        DestroyPendingEmptyObjectPreview();
        LastSaveMessage = TEXT("Tool cleared.");
        NotifyStateChanged();
        break;
    }
}

bool UGameManagerSubSystem::SetCurrentPrefabIndex(int32 NewIndex)
{
    if (PrefabFiles.Num() == 0)
    {
        ScanAssetFolders();
        BuildAvailableItems();
    }
    if (!PrefabFiles.IsValidIndex(NewIndex))
    {
        LastSaveMessage = TEXT("Invalid prefab index.");
        NotifyStateChanged();
        return false;
    }

    CurrentPrefabIndex = NewIndex;
    LastSaveMessage = FString::Printf(TEXT("Prefab 선택: %s"), *GetCurrentPrefabName());
    NotifyStateChanged();
    return true;
}

bool UGameManagerSubSystem::SetCurrentWeaponIndex(int32 NewIndex)
{
    if (WeaponFiles.Num() == 0)
    {
        ScanAssetFolders();
        BuildAvailableItems();
    }
    if (!WeaponFiles.IsValidIndex(NewIndex))
    {
        LastSaveMessage = TEXT("Invalid weapon index.");
        NotifyStateChanged();
        return false;
    }

    CurrentWeaponIndex = NewIndex;
    LastSaveMessage = FString::Printf(TEXT("무기 선택: %s"), *GetCurrentWeaponName());
    NotifyStateChanged();
    return true;
}

FString UGameManagerSubSystem::GetPrefabNameAtIndex(int32 Index) const
{
    return PrefabFiles.IsValidIndex(Index) ? FPaths::GetBaseFilename(PrefabFiles[Index]) : FString();
}

FString UGameManagerSubSystem::GetWeaponNameAtIndex(int32 Index) const
{
    return WeaponFiles.IsValidIndex(Index) ? FPaths::GetBaseFilename(WeaponFiles[Index]) : FString();
}

FString UGameManagerSubSystem::GetPrefabPathAtIndex(int32 Index) const
{
    return PrefabFiles.IsValidIndex(Index) ? PrefabFiles[Index] : FString();
}

FString UGameManagerSubSystem::GetWeaponPathAtIndex(int32 Index) const
{
    return WeaponFiles.IsValidIndex(Index) ? WeaponFiles[Index] : FString();
}

bool UGameManagerSubSystem::IsEditingGeneratedMesh() const
{
    return IsValid(CurrentEditableActor) && !CurrentEditableActor->IsFinalized();
}

int32 UGameManagerSubSystem::GetCurrentEditableMeshVertexCount() const
{
    return IsValid(CurrentEditableActor) ? CurrentEditableActor->GetVertices().Num() : 0;
}

int32 UGameManagerSubSystem::GetCurrentEditableMeshTriangleCount() const
{
    return IsValid(CurrentEditableActor) ? CurrentEditableActor->GetTriangles().Num() / 3 : 0;
}

bool UGameManagerSubSystem::IsCurrentEditableMeshTopologyValid() const
{
    return IsValid(CurrentEditableActor) && CurrentEditableActor->IsTopologyValid();
}

void UGameManagerSubSystem::GetSpawnedGeneratedMeshActors(TArray<AEditableMeshActor*>& OutActors) const
{
    OutActors.Empty();
    for (AEditableMeshActor* Actor : SpawnedGeneratedMeshes)
    {
        if (IsValid(Actor))
        {
            OutActors.Add(Actor);
        }
    }
}

void UGameManagerSubSystem::UpdatePendingEmptyObjectPreview(const FVector& Location)
{
    if (!IsValid(PendingEmptyObjectPreviewActor))
    {
        FActorSpawnParameters Params;
        Params.Owner = ConfigActor.Get();
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        UClass* EditableSpawnClass = EditableMeshActorClass ? EditableMeshActorClass.Get() : AEditableMeshActor::StaticClass();
        PendingEmptyObjectPreviewActor = GetWorld()->SpawnActor<AEditableMeshActor>(EditableSpawnClass, FTransform(Location), Params);
        if (IsValid(PendingEmptyObjectPreviewActor))
        {
            PendingEmptyObjectPreviewActor->BeginObject(TEXT("GeneratedMesh_Pending"));
            PendingEmptyObjectPreviewActor->SetActorEnableCollision(false);
        }
    }

    if (IsValid(PendingEmptyObjectPreviewActor))
    {
        PendingEmptyObjectPreviewActor->SetActorLocation(Location);
        PendingEmptyObjectPreviewActor->SetActorHiddenInGame(false);
        PendingEmptyObjectPreviewActor->SetActorEnableCollision(false);
    }
}

void UGameManagerSubSystem::DestroyPendingEmptyObjectPreview()
{
    if (IsValid(PendingEmptyObjectPreviewActor))
    {
        PendingEmptyObjectPreviewActor->Destroy();
    }
    PendingEmptyObjectPreviewActor = nullptr;
}

void UGameManagerSubSystem::UpdateObjectCreationPreview()
{
    if (IsEditingGeneratedMesh())
    {
        DestroyPendingEmptyObjectPreview();
        return;
    }

    if (IsSelectedToolbarItemObjectCreation() && CurrentMode == EToolMode::PlaceEmptyObject)
    {
        if (!bLastTraceHasPlacementLocation || GetEditableMeshFromHit(LastTraceHit))
        {
            DestroyPendingEmptyObjectPreview();
        }
        else
        {
            UpdatePendingEmptyObjectPreview(LastPreviewLocation);
        }
    }
    else
    {
        DestroyPendingEmptyObjectPreview();
    }
}

void UGameManagerSubSystem::ClearEditableVertexMoveState()
{
    HighlightedEditableVertexIndex = INDEX_NONE;
    bMovingHighlightedEditableVertex = false;
    bPrimaryVertexPressActive = false;
    bPrimaryVertexDragActive = false;
    PressedEditableVertexIndex = INDEX_NONE;
    ConnectedEditableVertexSourceIndex = INDEX_NONE;
    PrimaryVertexPressStartTime = 0.0;
    PrimaryVertexPressStartLocation = FVector::ZeroVector;
    LastVertexDistance = 0.0f;
    if (IsValid(CurrentEditableActor))
    {
        CurrentEditableActor->SetHighlightedVertexIndex(INDEX_NONE, false);
        CurrentEditableActor->ClearConnectedVertexSource();
    }
}

void UGameManagerSubSystem::UpdateEditableVertexPreviewAndSelection()
{
    LastVertexDistance = 0.0f;
    if (!IsValid(CurrentEditableActor) || CurrentEditableActor->IsFinalized() || CurrentMode != EToolMode::EditVertices)
    {
        HighlightedEditableVertexIndex = INDEX_NONE;
        bMovingHighlightedEditableVertex = false;
        bPrimaryVertexPressActive = false;
        bPrimaryVertexDragActive = false;
        PressedEditableVertexIndex = INDEX_NONE;
        return;
    }

    if (bPrimaryVertexPressActive)
    {
        UpdateEditableVertexPrimaryPressDrag();
        return;
    }

    int32 NearestIndex = INDEX_NONE;
    FVector NearestWorld = FVector::ZeroVector;
    float Distance = 0.0f;
    if (CurrentEditableActor->FindNearestVertexToRay(LastTraceStart, LastTraceDirection, VertexSelectionRayDistance, NearestIndex, NearestWorld, Distance))
    {
        HighlightedEditableVertexIndex = NearestIndex;
        LastVertexDistance = Distance;
        CurrentEditableActor->SetHighlightedVertexIndex(NearestIndex, false);
        CurrentEditableActor->ClearPreviewVertex();
    }
    else
    {
        HighlightedEditableVertexIndex = INDEX_NONE;
        CurrentEditableActor->SetHighlightedVertexIndex(INDEX_NONE, false);
        if (bLastTraceHasPlacementLocation)
        {
            CurrentEditableActor->SetPreviewVertexWorld(LastPreviewLocation);
            if (CurrentEditableActor->HasAnyVertex())
            {
                const int32 ConnectedSource = CurrentEditableActor->GetConnectedVertexSourceIndex();
                const FVector ReferenceVertex = ConnectedSource != INDEX_NONE ? CurrentEditableActor->GetVertexWorldLocation(ConnectedSource) : CurrentEditableActor->GetLastVertexWorld();
                LastVertexDistance = FVector::Dist(ReferenceVertex, LastPreviewLocation);
            }
        }
        else
        {
            CurrentEditableActor->ClearPreviewVertex();
        }
    }
}

FString UGameManagerSubSystem::BuildStatusText() const
{
    const FString ModeString = StaticEnum<EToolMode>()->GetDisplayNameTextByValue(static_cast<int64>(CurrentMode)).ToString();
    const FToolbarItem SelectedItem = GetSelectedToolbarItem();
    const FString SelectedItemName = SelectedItem.DisplayName.IsEmpty() ? TEXT("비어 있음") : SelectedItem.DisplayName;
    const FString ItemListText = bItemListWindowOpen ? TEXT("OPEN") : TEXT("CLOSED");
    const FString TopologyText = IsEditingGeneratedMesh()
        ? (IsCurrentEditableMeshTopologyValid() ? TEXT("VALID / GREEN") : TEXT("INVALID OR INCOMPLETE / RED"))
        : TEXT("NONE");
    const FString VertexSelectText = HighlightedEditableVertexIndex != INDEX_NONE
        ? FString::Printf(TEXT("Selected Vertex: %d %s"), HighlightedEditableVertexIndex, bMovingHighlightedEditableVertex ? TEXT("(DRAG MOVING)") : (bPrimaryVertexPressActive ? TEXT("(PRESSED)") : TEXT("")))
        : TEXT("Selected Vertex: none");
    const FString ConnectedSourceText = ConnectedEditableVertexSourceIndex != INDEX_NONE
        ? FString::Printf(TEXT("Connected Source: %d"), ConnectedEditableVertexSourceIndex)
        : TEXT("Connected Source: none");
    const FString CrosshairPlacementText = !bLastTraceHasPlacementLocation
        ? TEXT("NONE")
        : (bLastTraceBlockingHit ? TEXT("SURFACE") : TEXT("AIR / FREE-SPACE"));

    return FString::Printf(
        TEXT("[Creator Toolbar]\nMode: %s | PlayMode: %s\nToolbar Slot: %d / 7 | Item: %s (%s)\nInventory Window: %s | Available Items: %d\nSnap: %s / Grid %.0f cm\nCrosshair: X %.0f Y %.0f Z %.0f | Placement: %s | Collision %.0f cm / Max %.0f cm\nMesh Edit: V=%d T=%d | Topology: %s | %s | %s | RayDist %.1f cm\nControls: MouseWheel=toolbar slot, E=full item list, LMB empty=add vertex, LMB tap vertex=linked-vertex source, LMB hold+move vertex=move vertex, RMB=finish vertex editing, SnapAction/G=toggle snap\nFolder: %s\n%s"),
        *ModeString,
        PlayMode == EPlayMode::Creator ? TEXT("Creator") : TEXT("RealLife"),
        SelectedToolbarSlotIndex + 1,
        *SelectedItemName,
        *StaticEnum<EToolbarItemKind>()->GetDisplayNameTextByValue(static_cast<int64>(SelectedItem.Kind)).ToString(),
        *ItemListText,
        AvailableItems.Num(),
        bSnapToGrid ? TEXT("ON") : TEXT("OFF"),
        GridSize,
        LastPreviewLocation.X,
        LastPreviewLocation.Y,
        LastPreviewLocation.Z,
        *CrosshairPlacementText,
        CrosshairCollisionTraceDistance,
        FreeSpacePlacementDistance,
        GetCurrentEditableMeshVertexCount(),
        GetCurrentEditableMeshTriangleCount(),
        *TopologyText,
        *VertexSelectText,
        *ConnectedSourceText,
        LastVertexDistance,
        *GetWorldRootPath(),
        *LastSaveMessage);
}

FString UGameManagerSubSystem::BuildHUDText() const
{
    return BuildStatusText();
}
