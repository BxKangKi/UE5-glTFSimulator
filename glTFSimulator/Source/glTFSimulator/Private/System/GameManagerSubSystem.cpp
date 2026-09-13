// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

/**
 * @file GameManagerSubSystem.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "System/GameManagerSubSystem.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Editor/TransBuffer.h"
#endif
#include "GameMode/GameplayGameModeBase.h"
#include "System/GlTFSimulatorGameInstance.h"
#include "System/GlTFSimulatorAssetRegistry.h"
#include "System/ActorHelper.h"
#include "System/EntityArchive.h"
#include "System/SafeFileIO.h"
#include "Model/DynamicActor.h"
#include "Vehicle/VehiclePawn.h"
#include "Weapon/WeaponActor.h"
#include "Model/MaterialDefaultAsset.h"
#include "World/WorldEnvManager.h"
#include "World/WaterActor.h"
#include "World/PlayerData.h"
#include "ProceduralMeshComponent.h"
#include "System/MacroLibrary.h"
#include "System/MultiplayerWorldSubSystem.h"
#include "System/MultiplayerWorldStateActor.h"
#include "System/PhysicsHelper.h"
#include "Setting/GameSettings.h"
#include "World/WorldData.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/GameUserSettings.h"
#include "GameFramework/WorldSettings.h"
#include "System/SystemInfoFunctionLibrary.h"
#include "Components/PostProcessComponent.h"
#include "Model/WorldSceneStreamingSubsystem.h"
#include "Model/StaticActor.h"
#include "Model/InstancedEntitySubsystem.h"
#include "Character/CharacterController.h"
#include "Character/CharacterComponent.h"
#include "Character/PlayerCharacterController.h"
#include "System/FileFunctionLibrary.h"
#include "System/GlbValidation.h"
#include "System/glTFRuntimeSafety.h"
#include "Simulator/ModelDatabaseSubsystem.h"
#include "System/WorldObjectStreamingSubsystem.h"
#include "System/WorldSourceModelBuilder.h"
#include "Camera/CameraComponent.h"
#include "Weather/WeatherSubsystem.h"
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
#include "Misc/Paths.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/GarbageCollection.h"
#include "Engine/GameInstance.h"

static constexpr int32 ToolbarSlotCount = 7;

namespace
{
    constexpr int32 MaxSavedSceneReadinessAttempts = 120; // 30 seconds at 0.25 s intervals.
    constexpr int32 MaxSavedSceneDataAttempts = 20; // Allow transient model-registration failures to settle for up to five seconds.
    constexpr float SavedSceneLoadRetryDelaySeconds = 0.25f;

    // Global ocean is intentionally independent from Blueprint/GameMode transforms. Keeping one
    // native transform prevents an unset BP default (Identity) from spawning a 1 cm water volume.
    // 100000x on XY covers a very large world when the water mesh is authored at UE unit scale;
    // the smaller Z scale keeps the overlap/post-process volume bounded around sea level.
    const FTransform& GetHardcodedOceanTransform()
    {
        static const FTransform Transform(
            FRotator::ZeroRotator,
            FVector(0.0, 0.0, 0.0),
            FVector(100000.0, 100000.0, 10000.0));
        return Transform;
    }

    /**
     * Resolves the external world JSON play-mode key without letting a previous world's value leak
     * through the GameInstance subsystem. Empty/Default/SinglePlayer means use the map's directly
     * assigned GameplayGameMode default.
     */
    EPlayMode ResolveRuntimePlayModeKey(
        FString RuntimeModeKey,
        EPlayMode ConfiguredDefault,
        bool& bOutRecognized)
    {
        RuntimeModeKey.TrimStartAndEndInline();
        FString CanonicalKey = RuntimeModeKey;
        CanonicalKey.ReplaceInline(TEXT(" "), TEXT(""));
        CanonicalKey.ReplaceInline(TEXT("_"), TEXT(""));
        CanonicalKey.ReplaceInline(TEXT("-"), TEXT(""));
        bOutRecognized = true;

        if (CanonicalKey.IsEmpty()
            || CanonicalKey.Equals(TEXT("Default"), ESearchCase::IgnoreCase)
            || CanonicalKey.Equals(TEXT("SinglePlayer"), ESearchCase::IgnoreCase))
        {
            return ConfiguredDefault;
        }

        if (CanonicalKey.Equals(TEXT("Creator"), ESearchCase::IgnoreCase)
            || CanonicalKey.Equals(TEXT("CreatorMode"), ESearchCase::IgnoreCase))
        {
            return EPlayMode::Creator;
        }

        if (CanonicalKey.Equals(TEXT("RealLife"), ESearchCase::IgnoreCase)
            || CanonicalKey.Equals(TEXT("RealLifeMode"), ESearchCase::IgnoreCase))
        {
            return EPlayMode::RealLife;
        }

        bOutRecognized = false;
        return ConfiguredDefault;
    }

    bool IsFiniteWorldCoordinate(const double Value)
    {
        return FMath::IsFinite(Value)
            && FMath::Abs(Value) <= static_cast<double>(WORLD_MAX_SIZE);
    }

    bool IsFiniteWorldLocation(const FVector& Location)
    {
        return IsFiniteWorldCoordinate(Location.X)
            && IsFiniteWorldCoordinate(Location.Y)
            && IsFiniteWorldCoordinate(Location.Z);
    }

    bool IsFiniteRotation(const FRotator& Rotation)
    {
        return FMath::IsFinite(Rotation.Pitch)
            && FMath::IsFinite(Rotation.Yaw)
            && FMath::IsFinite(Rotation.Roll);
    }

}


UGameManagerSubSystem::UGameManagerSubSystem()
{
    bIsGamePaused = false;
    bIsWorldLoading = false;
    LoadingStatus = 0.0f;
    TotalSumFPS = 0;
    TotalCountFPS = 0;
    StaticActorClass = AStaticActor::StaticClass();
    DynamicActorClass = ADynamicActor::StaticClass();
    VehiclePawnClass = AVehiclePawn::StaticClass();
    WeaponActorClass = AWeaponActor::StaticClass();
    WorldEnvManagerClass = AWorldEnvManager::StaticClass();
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

    // Keep the installable external-asset directory present even before a world is opened so
    // users can drop Character/Dynamic .gasset packs into it from the Projects workflow.
    const FString ExternalResourcesRoot = FSafeFileIO::NormalizeFilePath(PATH_RESOURCES);
    if (!ExternalResourcesRoot.IsEmpty())
    {
        IFileManager::Get().MakeDirectory(*ExternalResourcesRoot, true);
    }

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
    StopGameplaySession(EEndPlayReason::Destroyed);

    if (PostLoadMapCleanupHandle.IsValid())
    {
        FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapCleanupHandle);
        PostLoadMapCleanupHandle.Reset();
    }

    ReleaseMaterialDefaultAsset();
    WorldSelectionTravelSourceWorld.Reset();
    WorldNameBeforeMenuTravel.Reset();
    bMenuTravelStatePrepared = false;
    bMenuTravelSaveCompleted = false;

    Super::Deinitialize();
}

const FglTFMaterialAssetReferences& UGameManagerSubSystem::GetMaterialDefaultReferences()
{
    if (UMaterialDefaultRuntimeCache* Guard = AcquireMaterialDefaultReferenceGuard())
    {
        return Guard->References;
    }

    static const FglTFMaterialAssetReferences EmptyReferences;
    return EmptyReferences;
}

UMaterialDefaultRuntimeCache* UGameManagerSubSystem::AcquireMaterialDefaultReferenceGuard()
{
    // UObject loads and mutation of the shared table are game-thread-owned. Returning null
    // off-thread prevents a data race and prevents an accidental synchronous asset load.
    if (!ensureMsgf(IsInGameThread(), TEXT("AcquireMaterialDefaultReferenceGuard must run on the game thread")))
    {
        return nullptr;
    }

    if (!bMaterialDefaultAssetResolved && !bMaterialDefaultAssetResolving)
    {
        ResolveMaterialDefaultAsset();
    }

    return IsValid(MaterialDefaultRuntimeCache) ? MaterialDefaultRuntimeCache.Get() : nullptr;
}

bool UGameManagerSubSystem::ResolveMaterialDefaultAsset()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("glTF material defaults must be resolved on the game thread")))
    {
        return false;
    }

    if (bMaterialDefaultAssetResolving)
    {
        return IsValid(MaterialDefaultRuntimeCache);
    }
    TGuardValue<bool> ResolvingGuard(bMaterialDefaultAssetResolving, true);
    bMaterialDefaultAssetResolved = false;

    // Never mutate an old guard: a cancelled native glTFRuntime callback may still hold it.
    MaterialDefaultRuntimeCache = nullptr;

    UMaterialDefaultRuntimeCache* NewCache = NewObject<UMaterialDefaultRuntimeCache>(this);
    if (!IsValid(NewCache))
    {
        bMaterialDefaultAssetResolved = true;
        UE_LOG(LogTemp, Error, TEXT("glTF material runtime reference guard could not be allocated."));
        return false;
    }
    MaterialDefaultRuntimeCache = NewCache;

    UGlTFSimulatorAssetRegistry* Registry =
        UGlTFSimulatorGameInstance::GetAssetRegistryFromContext(this);
    if (!IsValid(Registry))
    {
        bMaterialDefaultAssetResolved = true;
        UE_LOG(LogTemp, Warning,
            TEXT("No central AssetRegistry is available; glTFRuntime plugin material defaults will be used."));
        return true;
    }

    TArray<FString> Failures;
    const bool bResolveCallSucceeded = Registry->GlTFMaterials.Resolve(
        NewCache->References,
        Failures);
    bMaterialDefaultAssetResolved = true;

    for (const FString& Failure : Failures)
    {
        UE_LOG(LogTemp, Warning, TEXT("Central glTF material reference skipped: %s"), *Failure);
    }

    UE_LOG(LogTemp, Display,
        TEXT("Central glTF materials resolved. Configured=%d Named=%d Failures=%d"),
        Registry->GlTFMaterials.NumConfiguredReferences(),
        NewCache->References.ByMaterialName.Num(),
        Failures.Num());
    return bResolveCallSucceeded;
}

void UGameManagerSubSystem::ReleaseMaterialDefaultAsset()
{
    // Async requests can still retain the old cache object until their native callbacks finish.
    MaterialDefaultRuntimeCache = nullptr;
    bMaterialDefaultAssetResolved = false;
    bMaterialDefaultAssetResolving = false;
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

void UGameManagerSubSystem::SetCameraComponent(USceneComponent* InCamera)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("UGameManagerSubSystem::SetCameraComponent must run on the game thread")))
    {
        return;
    }

    CurrentCamera = InCamera;
    if (UGameInstance* GameInstance = GetGameInstance())
    {
        if (UWeatherSubsystem* Weather = GameInstance->GetSubsystem<UWeatherSubsystem>())
        {
            Weather->SetWeatherCamera(InCamera);
        }
    }
}

bool UGameManagerSubSystem::TryNormalizeWorldFolderName(
    const FString& Candidate,
    FString& OutNormalized,
    const bool bRequireExistingDirectory)
{
    // A world name is an opaque direct-child key, not a path. Run the same normalization at every
    // trust boundary so menu selections, URL options, PIE overrides, and replication cannot drift.
    OutNormalized = Candidate.TrimStartAndEnd();
    if (OutNormalized.IsEmpty()
        || OutNormalized == TEXT(".")
        || OutNormalized == TEXT("..")
        || FPaths::GetCleanFilename(OutNormalized) != OutNormalized
        || FPaths::MakeValidFileName(OutNormalized) != OutNormalized)
    {
        OutNormalized.Reset();
        return false;
    }

    if (bRequireExistingDirectory)
    {
        const FString VirtualRoot = FPaths::Combine(PATH_WORLDS, OutNormalized);
        if (!IFileManager::Get().FileExists(*FGWorldArchive::MakeArchivePath(VirtualRoot)))
        {
            OutNormalized.Reset();
            return false;
        }
    }
    return true;
}

void UGameManagerSubSystem::SetCurrentWorldName(FString Name)
{
    check(IsInGameThread());

    // Empty is the explicit menu/teardown reset. Invalid non-empty values also clear the old key:
    // retaining it could make a malformed network value reopen the previous world's data instead.
    if (Name.TrimStartAndEnd().IsEmpty())
    {
        CurrentWorldName.Reset();
        return;
    }

    FString Normalized;
    if (!TryNormalizeWorldFolderName(Name, Normalized, false))
    {
        UE_LOG(LogTemp, Error,
            TEXT("Rejected invalid external world folder key: %s"), *Name.Left(256));
        CurrentWorldName.Reset();
        return;
    }
    CurrentWorldName = MoveTemp(Normalized);
}

void UGameManagerSubSystem::SetPlayerActor(AActor* Actor)
{
    UWorld* World = GetWorld();
    if (!IsValid(Actor) || !World || Actor->GetWorld() != World)
    {
        return;
    }

    // The save record is intentionally single-player scoped. Ignore unpossessed, remote, and
    // split-screen secondary Pawns so they cannot consume the one-shot transform handshake.
    if (const APawn* Pawn = Cast<APawn>(Actor))
    {
        AController* OwnerController = Pawn->GetController();
        APlayerController* FirstPlayerController = World->GetFirstPlayerController();
        if (!IsValid(OwnerController)
            || (IsValid(FirstPlayerController) && OwnerController != FirstPlayerController)
            || (!IsValid(FirstPlayerController) && !OwnerController->IsLocalController()))
        {
            return;
        }
    }

    PlayerActor = Actor;
    if (World->GetNetMode() == NM_Client)
    {
        // Clients keep the local actor reference for camera/streaming helpers, but the authority
        // owns persistent spawn state and will replicate the resulting transform.
        bInitialPlayerTransformResolved = true;
        bPendingInitialControlRotation = false;
        return;
    }

    if (bInitialPlayerTransformResolved)
    {
        // Runtime character/GLB replacement must never replay the one-shot saved transform.
        return;
    }

    if (!bInitialPlayerDataLoadCompleted)
    {
        // BeginPlay ordering is not guaranteed. Preserve the PlayerStart transform provisionally
        // while the manager checks .dat; a valid saved transform may still supersede it.
        const FVector SpawnLocation = Actor->GetActorLocation();
        if (InitialPlayerLocationSource == EInitialPlayerLocationSource::None
            && IsFiniteWorldLocation(SpawnLocation))
        {
            PlayerLocation = SpawnLocation;
        }
        return;
    }

    TryResolveInitialPlayerTransform();
}

void UGameManagerSubSystem::SetPlayerLocation(
    const FVector& Location,
    const AActor* SourceActor)
{
    if (!IsFiniteWorldLocation(Location)
        || !IsValid(SourceActor)
        || SourceActor != PlayerActor.Get()
        || SourceActor->GetWorld() != GetWorld())
    {
        return;
    }

    PlayerLocation = Location;
}

void UGameManagerSubSystem::ApplyPendingInitialPlayerControlRotation(
    APlayerController* Controller)
{
    UWorld* World = GetWorld();
    if (!bPendingInitialControlRotation
        || !bInitialPlayerTransformResolved
        || !IsValid(Controller)
        || !World
        || World->GetNetMode() == NM_Client
        || Controller != World->GetFirstPlayerController()
        || Controller->GetPawn() != PlayerActor.Get()
        || !IsFiniteRotation(LoadedInitialPlayerRotation))
    {
        return;
    }

    // AGameModeBase::FinishRestartPlayer writes the PlayerStart rotation after OnPossess. This
    // method is called on the following tick so the validated save wins that final engine write.
    Controller->SetControlRotation(LoadedInitialPlayerRotation);
    Controller->ClientSetRotation(LoadedInitialPlayerRotation, true);
    bPendingInitialControlRotation = false;
}

void UGameManagerSubSystem::UpdateSettings()
{
    if (!IsValid(GameSettings))
    {
        return;
    }

    if (IsValid(PostProcess))
    {
        GameSettings->UpdateSettings(PostProcess);
    }
    else
    {
        // Streaming settings are still valid in menu/headless contexts even when no post process
        // component has been registered yet.
        GameSettings->UpdateSettings(nullptr);
    }

    if (UWorldObjectStreamingSubsystem* Chunks = GetWorld()
        ? GetWorld()->GetSubsystem<UWorldObjectStreamingSubsystem>() : nullptr)
    {
        if (Chunks->IsRunning())
        {
            Chunks->SetLoadRadiusMeters(GameSettings->GetEffectiveObjectStreamingRadiusMeters());
        }
    }

    // Height fog/cloud are ordinary settings.json toggles as well. Re-apply them to the active
    // environment immediately instead of requiring a world reload.
    if (IsValid(WorldEnvManagerActor))
    {
        WorldEnvManagerActor->RefreshRuntimeSettings();
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
    if (TotalSumFPS >= TNumericLimits<int32>::Max() - FPS)
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

void UGameManagerSubSystem::ApplyGameModeConfig(const AGlTFSimulatorGameplayGameModeBase* InConfigGameMode)
{
    if (!IsValid(InConfigGameMode))
    {
        return;
    }

    // Level actors own only numeric/session tuning. Asset/class references live in the GameInstance registry.
    PlacementGridSpacing = InConfigGameMode->PlacementGridSpacing;
    PlacementGridLineThickness = InConfigGameMode->PlacementGridLineThickness;
    PlacementGridMaxRadius = InConfigGameMode->PlacementGridMaxRadius;
    PlacementGridStrongRadius = InConfigGameMode->PlacementGridStrongRadius;
    PlacementGridFadeRadius = InConfigGameMode->PlacementGridFadeRadius;
    OceanTransform = GetHardcodedOceanTransform();
    PlacementTraceDistance = InConfigGameMode->PlacementTraceDistance;
    CrosshairCollisionTraceDistance = InConfigGameMode->CrosshairCollisionTraceDistance;
    FreeSpacePlacementDistance = InConfigGameMode->FreeSpacePlacementDistance;
    bAllowFreeSpacePlacement = InConfigGameMode->bAllowFreeSpacePlacement;
    GridSize = InConfigGameMode->GridSize;
    SurfacePlacementOffset = InConfigGameMode->SurfacePlacementOffset;
    VehicleEnterDistance = InConfigGameMode->VehicleEnterDistance;
    bAutoSaveScene = InConfigGameMode->bAutoSaveScene;
    SceneAutoSaveIntervalSeconds = InConfigGameMode->SceneAutoSaveIntervalSeconds;
    bSaveSceneOnEndPlay = InConfigGameMode->bSaveSceneOnEndPlay;
    PlayMode = InConfigGameMode->PlayMode;

    UGlTFSimulatorAssetRegistry* Registry =
        UGlTFSimulatorGameInstance::GetAssetRegistryFromContext(this);

    auto IsUsableClass = [](UClass* Class, UClass* RequiredBase)
    {
        return IsValid(Class) && Class->IsChildOf(RequiredBase)
            && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
    };

    UClass* ResolvedStatic = Registry ? Registry->StaticActorClass.LoadSynchronous() : nullptr;
    StaticActorClass = IsUsableClass(ResolvedStatic, AStaticActor::StaticClass())
        ? ResolvedStatic : AStaticActor::StaticClass();

    UClass* ResolvedDynamic = Registry ? Registry->DynamicActorClass.LoadSynchronous() : nullptr;
    DynamicActorClass = IsUsableClass(ResolvedDynamic, ADynamicActor::StaticClass())
        ? ResolvedDynamic : ADynamicActor::StaticClass();

    UClass* ResolvedVehicle = Registry ? Registry->VehiclePawnClass.LoadSynchronous() : nullptr;
    VehiclePawnClass = IsUsableClass(ResolvedVehicle, AVehiclePawn::StaticClass())
        ? ResolvedVehicle : AVehiclePawn::StaticClass();

    UClass* ResolvedWeapon = Registry ? Registry->WeaponActorClass.LoadSynchronous() : nullptr;
    WeaponActorClass = IsUsableClass(ResolvedWeapon, AWeaponActor::StaticClass())
        ? ResolvedWeapon : AWeaponActor::StaticClass();

    UClass* ResolvedEnv = Registry ? Registry->WorldEnvManagerClass.LoadSynchronous() : nullptr;
    WorldEnvManagerClass = IsUsableClass(ResolvedEnv, AWorldEnvManager::StaticClass())
        ? ResolvedEnv : AWorldEnvManager::StaticClass();

    UClass* ResolvedWater = Registry ? Registry->WaterActorClass.LoadSynchronous() : nullptr;
    WaterClass = IsUsableClass(ResolvedWater, AWaterActor::StaticClass())
        ? ResolvedWater : AWaterActor::StaticClass();

    UClass* ResolvedRain = Registry ? Registry->RainWeatherActorClass.LoadSynchronous() : nullptr;
    RainWeatherActorClass = IsUsableClass(ResolvedRain, AActor::StaticClass()) ? ResolvedRain : nullptr;

    PlacementGridMaterial = Registry ? Registry->PlacementGridMaterial.LoadSynchronous() : nullptr;

    if (!bMaterialDefaultAssetResolved)
    {
        ResolveMaterialDefaultAsset();
    }
}

FTimerManager& UGameManagerSubSystem::GetWorldTimerManager() const
{
    check(GetWorld());
    return GetWorld()->GetTimerManager();
}

FVector UGameManagerSubSystem::GetSessionOwnerLocation() const
{
    return SessionOwner.IsValid() ? SessionOwner->GetActorLocation() : FVector::ZeroVector;
}

void UGameManagerSubSystem::EnsureRuntimeComponents()
{
    AActor* OwnerActor = SessionOwner.Get();
    if (!IsValid(OwnerActor))
    {
        return;
    }

    Root = OwnerActor->GetRootComponent();
    if (IsValid(PlacementGridComponent))
    {
        return;
    }

    // The grid mesh is runtime state, so it is created by the subsystem on the config actor instead of being an editor property.
    PlacementGridComponent = NewObject<UProceduralMeshComponent>(OwnerActor, TEXT("PlacementGrid_Runtime"));
    if (IsValid(PlacementGridComponent))
    {
        OwnerActor->AddInstanceComponent(PlacementGridComponent);
        if (IsValid(Root))
        {
            PlacementGridComponent->SetupAttachment(Root);
        }
        PlacementGridComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        PlacementGridComponent->SetGenerateOverlapEvents(false);
        PlacementGridComponent->SetCastShadow(false);
        PlacementGridComponent->SetHiddenInGame(true);
        PlacementGridComponent->RegisterComponent();
    }
}


bool UGameManagerSubSystem::IsActiveGameMode(const AGlTFSimulatorGameplayGameModeBase* GameMode) const
{
    return ConfigGameMode.Get() == GameMode && SessionOwner.Get() == GameMode;
}

void UGameManagerSubSystem::StartGameplaySession(AGlTFSimulatorGameplayGameModeBase* InGameMode)
{
    if (!IsValid(InGameMode) || InGameMode->IsActorBeingDestroyed())
    {
        return;
    }
    StartGameplaySessionInternal(InGameMode, InGameMode);
}

void UGameManagerSubSystem::StartClientGameplaySession(APlayerController* InOwnerController)
{
    if (!IsValid(InOwnerController) || InOwnerController->IsActorBeingDestroyed())
    {
        return;
    }

    // GameMode exists only on authority, but AGameStateBase replicates the active GameModeClass.
    // Prefer that class's CDO so Blueprint defaults configured on MultiplayGameMode also reach
    // clients. Fall back to the native common CDO while GameState is not ready yet.
    const AGlTFSimulatorGameplayGameModeBase* Defaults =
        GetDefault<AGlTFSimulatorGameplayGameModeBase>();

    if (const UWorld* World = InOwnerController->GetWorld())
    {
        if (const AGameStateBase* GameState = World->GetGameState())
        {
            UClass* ReplicatedGameModeClass = GameState->GameModeClass.Get();
            if (IsValid(ReplicatedGameModeClass)
                && ReplicatedGameModeClass->IsChildOf(AGlTFSimulatorGameplayGameModeBase::StaticClass()))
            {
                if (const AGlTFSimulatorGameplayGameModeBase* ReplicatedDefaults =
                    Cast<AGlTFSimulatorGameplayGameModeBase>(ReplicatedGameModeClass->GetDefaultObject()))
                {
                    Defaults = ReplicatedDefaults;
                }
            }
        }
    }

    // Replication can call this again after the local render session was already started. Refresh
    // tunables from the now-known Blueprint GameMode CDO instead of rebuilding the whole session.
    if (bManagerStarted && SessionOwner.Get() == InOwnerController)
    {
        ApplyGameModeConfig(Defaults);
        return;
    }

    StartGameplaySessionInternal(InOwnerController, Defaults);
}

void UGameManagerSubSystem::StartGameplaySessionInternal(
    AActor* InOwnerActor,
    const AGlTFSimulatorGameplayGameModeBase* InConfigGameMode)
{
    if (!IsValid(InOwnerActor) || !IsValid(InConfigGameMode))
    {
        return;
    }

    check(IsInGameThread());
    UWorld* World = InOwnerActor->GetWorld();
    if (!IsValid(World) || World != GetWorld())
    {
        return;
    }

    if (bManagerStarted && SessionOwner.IsValid()
        && !SessionOwner->IsActorBeingDestroyed()
        && SessionOwner->GetWorld() == World)
    {
        SpawnWorldEnvManager();
        return;
    }

    if (bManagerStarted && SessionOwner.Get() != InOwnerActor)
    {
        StopGameplaySession(EEndPlayReason::Destroyed);
    }

    SessionOwner = InOwnerActor;
    ConfigGameMode = Cast<AGlTFSimulatorGameplayGameModeBase>(InOwnerActor);
    ApplyGameModeConfig(InConfigGameMode);

    // World environment is a rendering prerequisite and intentionally starts before .gwd I/O.
    SpawnWorldEnvManager();

    bMenuTravelStatePrepared = false;
    bMenuTravelSaveCompleted = false;
    WorldNameBeforeMenuTravel.Reset();

    UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this);
    FString ResolvedWorldName;
    FString ResolutionSource;
    const auto TryWorldCandidate =
        [&ResolvedWorldName, &ResolutionSource](const FString& Candidate, const TCHAR* Source)
        {
            if (!ResolvedWorldName.IsEmpty())
            {
                return true;
            }
            const FString TrimmedCandidate = Candidate.TrimStartAndEnd();
            if (TrimmedCandidate.IsEmpty())
            {
                return false;
            }
            FString Normalized;
            if (!UGameManagerSubSystem::TryNormalizeWorldFolderName(TrimmedCandidate, Normalized, true))
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("Ignored unavailable or invalid world hand-off. Source=%s Value=%s"),
                    Source, *TrimmedCandidate.Left(256));
                return false;
            }
            ResolvedWorldName = MoveTemp(Normalized);
            ResolutionSource = Source;
            return true;
        };

    const TCHAR* WorldOption = World->URL.GetOption(TEXT("World="), nullptr);
    if (WorldOption && FCString::Strlen(WorldOption) > 0)
    {
        TryWorldCandidate(FString(WorldOption), TEXT("URL option"));
    }
    if (IsValid(Multiplayer))
    {
        TryWorldCandidate(Multiplayer->GetSelectedWorldFolderName(), TEXT("world selection"));
    }
    TryWorldCandidate(CurrentWorldName, TEXT("pending selection"));
    TryWorldCandidate(InConfigGameMode->WorldFolderName, TEXT("game mode default"));

    if (ResolvedWorldName.IsEmpty())
    {
        const FString MapFolderName = UWorld::RemovePIEPrefix(
            FPaths::GetBaseFilename(World->GetMapName()), nullptr);
        TryWorldCandidate(MapFolderName, TEXT("PIE map folder"));
    }

    CurrentWorldName = MoveTemp(ResolvedWorldName);
    if (!CurrentWorldName.IsEmpty() && IsValid(Multiplayer))
    {
        Multiplayer->SetSelectedWorldFolderName(CurrentWorldName);
    }

    const FString ResolvedWorldRoot = GetWorldRootPath();
    UE_LOG(LogTemp, Display,
        TEXT("Resolved runtime world root. SelectedWorld=%s Root=%s Source=%s"),
        CurrentWorldName.IsEmpty() ? TEXT("<empty>") : *CurrentWorldName,
        *ResolvedWorldRoot,
        ResolutionSource.IsEmpty() ? TEXT("<none>") : *ResolutionSource);

    if (ResolvedWorldRoot.IsEmpty())
    {
        FailWorldStartup(TEXT("World startup failed: no explicit world folder could be resolved."));
        return;
    }

    if (World->GetNetMode() != NM_Standalone && World->GetNetMode() != NM_Client)
    {
        AMultiplayerWorldStateActor::SpawnOrUpdateForWorld(this, CurrentWorldName);
    }

    ValidateResolvedGameMode();
    EnsureRuntimeComponents();

    if (bManagerStarted)
    {
        return;
    }
    bManagerStarted = true;
    bWorldStartupContinued = false;
    bAutoBuildForStartup = false;

    SetWorldLoading(true);
    SetLoadingStatus(0.01f);
    ShowLoadingWidget();

    UModelDatabaseSubsystem* ModelDatabase = GetGameInstance()
        ? GetGameInstance()->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    if (ModelDatabase)
    {
        TWeakObjectPtr<UGameManagerSubSystem> WeakThis(this);
        const TWeakObjectPtr<AActor> StartupOwner = SessionOwner;
        ModelDatabase->InitializeForWorld(GetWorldRootPath(), FModelDatabaseReady::CreateLambda(
            [WeakThis, StartupOwner](const bool bSuccess, const FString& Error)
            {
                UGameManagerSubSystem* StrongThis = WeakThis.Get();
                if (!IsValid(StrongThis) || !StrongThis->bManagerStarted
                    || !StartupOwner.IsValid() || StrongThis->SessionOwner != StartupOwner)
                {
                    return;
                }
                if (!bSuccess)
                {
                    StrongThis->FailWorldStartup(FString::Printf(
                        TEXT("Failed to open the world model index: %s"), *Error));
                    return;
                }
                UModelDatabaseSubsystem* ReadyDatabase = StrongThis->GetGameInstance()
                    ? StrongThis->GetGameInstance()->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
                if (!ReadyDatabase || !ReadyDatabase->IsBuiltWorld())
                {
                    StrongThis->FailWorldStartup(
                        TEXT("World startup failed: runtime uses Worlds/*.gwd only. Build the project first from the Projects UI in MainWorld."));
                    return;
                }
                StrongThis->ContinueWorldStartupAfterDatabase();
            }));
    }
    else
    {
        FailWorldStartup(TEXT("World startup failed: the model database subsystem is unavailable."));
    }

    UE_LOG(LogTemp, Display, TEXT("[Gameplay] Session active. Owner=%s GameMode=%s"),
        *GetNameSafe(SessionOwner.Get()),
        ConfigGameMode.IsValid() ? *GetNameSafe(ConfigGameMode.Get()) : TEXT("<client>"));
    NotifyStateChanged();
}

void UGameManagerSubSystem::ContinueWorldStartupAfterDatabase()
{
    check(IsInGameThread());
    if (!bManagerStarted || bWorldStartupContinued) return;

    UModelDatabaseSubsystem* Database = GetGameInstance()
        ? GetGameInstance()->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    if (!Database || !Database->IsBuiltWorld())
    {
        FailWorldStartup(
            TEXT("World startup failed: no verified .gwd archive is open."));
        return;
    }

    bWorldStartupContinued = true;
    bAutoBuildForStartup = false;
    // The archive directory is now verified. Reserve 70..96% for initial range streaming and the
    // final 4% for restoring the first .dat area.
    SetLoadingStatus(FMath::Max(LoadingStatus, 0.70f));
    RefreshBuiltModelLists();
    BuildAvailableItems();
    InitializeToolbarSlotsIfNeeded();
    ApplySelectedToolbarItem(false);
    if (UWorldObjectStreamingSubsystem* Chunks = GetWorld()
        ? GetWorld()->GetSubsystem<UWorldObjectStreamingSubsystem>() : nullptr)
    {
        const float ObjectRadiusMeters = IsValid(GameSettings)
            ? GameSettings->GetEffectiveObjectStreamingRadiusMeters()
            : 2048.0f;
        Chunks->Start(GetWorldRootPath(), ObjectRadiusMeters);
    }
    InitializeRuntimeWorldState();
    NotifyToolbarChanged();

    if (UWorld* World = GetWorld())
    {
        TWeakObjectPtr<UGameManagerSubSystem> WeakThis(this);
        const TWeakObjectPtr<UWorld> StartupWorld(World);
        World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([WeakThis, StartupWorld]()
        {
            if (WeakThis.IsValid() && StartupWorld.IsValid()
                && WeakThis->GetWorld() == StartupWorld.Get() && WeakThis->bManagerStarted)
                WeakThis->LoadSavedScene();
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

void UGameManagerSubSystem::FailWorldStartup(const FString& Message)
{
    check(IsInGameThread());

    bAutoBuildForStartup = false;
    LastSaveMessage = Message;
    UE_LOG(LogTemp, Error, TEXT("%s"), *Message);

    // Never destroy WorldEnvManager here. Its dependency-free atmosphere is intentionally kept
    // visible so a source/config error produces a usable diagnostic scene rather than a black map.
    HideLoadingWidget();
    SetWorldLoading(false);
    NotifyStateChanged();
}

void UGameManagerSubSystem::StopGameplaySession(
    const EEndPlayReason::Type EndPlayReason,
    const AGlTFSimulatorGameplayGameModeBase* RequestingGameMode)
{
    // An old map actor can finish EndPlay after the destination map has already started its own
    // manager. Never let that stale callback tear down the destination world's streaming session.
    if (RequestingGameMode && ConfigGameMode.IsValid() && ConfigGameMode.Get() != RequestingGameMode)
    {
        UE_LOG(LogTemp, Display,
            TEXT("[Gameplay] Ignored stale GameplayGameMode EndPlay. Requester=%s Active=%s"),
            *GetNameSafe(RequestingGameMode),
            *GetNameSafe(ConfigGameMode.Get()));
        return;
    }

    CancelWorldBake();
    CompactTrackedEntityReferences();

    const bool bHadActiveRuntimeWorld = bManagerStarted
        || bRuntimeWorldStateInitialized
        || IsValid(StreamSubSystem)
        || IsValid(WorldEnvManagerActor)
        || IsValid(OceanActor)
        || IsValid(LoadingWidgetInstance.Get())
        || SpawnedStatics.Num() > 0
        || SpawnedVehicles.Num() > 0
        || IsValid(EquippedWeapon);

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(SceneAutoSaveTimerHandle);
        World->GetTimerManager().ClearTimer(WorldDataSaveTimerHandle);
        World->GetTimerManager().ClearAllTimersForObject(this);
    }

    if (bHadActiveRuntimeWorld && bSaveSceneOnEndPlay && !bMenuTravelSaveCompleted
        && EndPlayReason != EEndPlayReason::Destroyed)
    {
        // Fallback for editor shutdown or travel paths that did not run the pre-travel commit.
        SaveScene();
        SaveWorldData();
        SavePlayerData();
        bMenuTravelSaveCompleted = true;
    }

    if (bMenuTravelStatePrepared)
    {
        // The source folder remains valid through the pre-travel commit and teardown fallback.
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

    // Commit while every tracked actor is still alive. EndPlay ordering is not deterministic: a
    // Static object or vehicle can be destroyed before the manager receives its own EndPlay callback,
    // so every live object is snapshotted before its owning chunk is released.
    WorldNameBeforeMenuTravel = CurrentWorldName;
    const bool bEntitySaveCompleted = SaveScene();
    SaveWorldData();
    SavePlayerData();

    bMenuTravelStatePrepared = true;
    // A failed/blocked save intentionally leaves the last committed .dat generation untouched. Do not retry
    // from actor teardown, where the snapshot is less trustworthy than it is at this point.
    bMenuTravelSaveCompleted = true;
    UE_LOG(LogTemp, Display,
        TEXT("Pre-travel runtime save finished. World=%s EntitySave=%s"),
        *WorldNameBeforeMenuTravel,
        bEntitySaveCompleted ? TEXT("success") : TEXT("preserved-previous-generation"));
    RequestPostLoadRuntimeMemoryCleanup();
    SetGamePaused(false);
}

void UGameManagerSubSystem::PrepareForReturnToMenuLevel()
{
    // Keep the old Blueprint ABI non-destructive: it prepares the atomic save/cleanup state, while
    // the caller still owns the actual OpenLevel request and can recover if travel is rejected.
    PrepareForMenuLevelTravelRequest();
}

void UGameManagerSubSystem::PrepareForReturnToMainWorld()
{
    // The two legacy names historically had identical behavior; funnel both through one audited
    // implementation so save ordering and reference release cannot diverge again.
    PrepareForReturnToMenuLevel();
}

void UGameManagerSubSystem::RequestWorldSelectionMenuOnNextMainWorld()
{
    bOpenWorldSelectionMenuOnNextMainWorld = true;
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
    // gameplay-world actors, streamed built assets, and async-load state reachable.
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
    ReleaseMaterialDefaultAsset();

    ClearPlacementGridMesh();
    if (IsValid(PlacementGridComponent))
    {
        PlacementGridComponent->SetMaterial(0, nullptr);
        if (AActor* OwnerActor = PlacementGridComponent->GetOwner(); IsValid(OwnerActor))
        {
            OwnerActor->RemoveInstanceComponent(PlacementGridComponent);
        }
        PlacementGridComponent->UnregisterComponent();
        PlacementGridComponent->DestroyComponent();
        PlacementGridComponent = nullptr;
    }

    Root = nullptr;
    ConfigGameMode = nullptr;
    SessionOwner = nullptr;
    ClearTransientRuntimeReferences();
    ResetWorldRuntimeReferences();

    bManagerStarted = false;
    bRuntimeWorldStateInitialized = false;
    bWorldLoadCompleted = false;
    bSpawnedWorldEnvManager = false;
    bIsWorldLoading = false;
    bIsGamePaused = false;
    LoadingStatus = 0.0f;

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

    DestroyActorIfValid(EquippedWeapon.Get());

    for (const TWeakObjectPtr<AStaticActor>& StaticReference : SpawnedStatics)
    {
        DestroyActorIfValid(StaticReference.Get());
    }
    for (const TWeakObjectPtr<AVehiclePawn>& VehicleReference : SpawnedVehicles)
    {
        AVehiclePawn* Vehicle = VehicleReference.Get();
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

    EquippedWeapon = nullptr;
    SpawnedStatics.Empty();
    SpawnedVehicles.Empty();
}

void UGameManagerSubSystem::CompactTrackedEntityReferences()
{
    const int32 RemovedStatics = SpawnedStatics.RemoveAllSwap(
        [](const TWeakObjectPtr<AStaticActor>& Reference)
        {
            const AStaticActor* Actor = Reference.Get();
            return !IsValid(Actor) || Actor->IsActorBeingDestroyed();
        },
        EAllowShrinking::No);

    const int32 RemovedVehicles = SpawnedVehicles.RemoveAllSwap(
        [](const TWeakObjectPtr<AVehiclePawn>& Reference)
        {
            const AVehiclePawn* Actor = Reference.Get();
            return !IsValid(Actor) || Actor->IsActorBeingDestroyed();
        },
        EAllowShrinking::No);

    // Avoid reallocating during normal placement churn, but release clearly excessive slack.
    if (RemovedStatics > 0 && SpawnedStatics.Max() > FMath::Max(32, SpawnedStatics.Num() * 2))
    {
        SpawnedStatics.Shrink();
    }
    if (RemovedVehicles > 0 && SpawnedVehicles.Max() > FMath::Max(16, SpawnedVehicles.Num() * 2))
    {
        SpawnedVehicles.Shrink();
    }
}

void UGameManagerSubSystem::ResetWorldRuntimeReferences()
{
    StaticActorClass = nullptr;
    ConfigGameMode.Reset();
    SessionOwner.Reset();
    PlayerActor = nullptr;
    CurrentCamera = nullptr;
    PostProcess = nullptr;
    CurrentWorldData = nullptr;
    ActiveWorldData = nullptr;
    WorldEnvManagerActor = nullptr;
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

void UGameManagerSubSystem::RequestPostLoadRuntimeMemoryCleanup()
{
    bPendingMainWorldRuntimePurge = true;
}

void UGameManagerSubSystem::HandlePostLoadMapRuntimeCleanup(UWorld* LoadedWorld)
{
    if (LoadedWorld)
    {
        // Give placed actors and the configured PlayerController their normal BeginPlay/next-tick
        // opportunity first. The second tick performs cleanup/configuration validation and the
        // client-only render-session initialization. Authority startup is owned only by GameMode::BeginPlay.
        const TWeakObjectPtr<UWorld> WeakLoadedWorld(LoadedWorld);
        LoadedWorld->GetTimerManager().SetTimerForNextTick(
            FTimerDelegate::CreateWeakLambda(this, [this, WeakLoadedWorld]()
            {
                UWorld* StrongLoadedWorld = WeakLoadedWorld.Get();
                if (!IsValid(StrongLoadedWorld) || StrongLoadedWorld != GetWorld())
                {
                    return;
                }

                RunPostLoadRuntimeMemoryCleanup();
                StrongLoadedWorld->GetTimerManager().SetTimerForNextTick(
                    FTimerDelegate::CreateWeakLambda(this, [this, WeakLoadedWorld]()
                    {
                        if (UWorld* CurrentLoadedWorld = WeakLoadedWorld.Get();
                            IsValid(CurrentLoadedWorld) && CurrentLoadedWorld == GetWorld())
                        {
                            ValidatePostLoadGameplayLifecycle(CurrentLoadedWorld);
                        }
                    }));
            }));
        return;
    }

    RunPostLoadRuntimeMemoryCleanup();
}

void UGameManagerSubSystem::ValidatePostLoadGameplayLifecycle(UWorld* LoadedWorld)
{
    check(IsInGameThread());
    if (!IsValid(LoadedWorld) || LoadedWorld != GetWorld() || !LoadedWorld->IsGameWorld())
    {
        return;
    }

    // MainWorld is UI-only. A pending return-to-menu request must never be mistaken for gameplay.
    if (bMenuTravelStatePrepared || bWorldSelectionMenuTravelInProgress
        || bOpenWorldSelectionMenuOnNextMainWorld)
    {
        return;
    }

    if (bManagerStarted && SessionOwner.IsValid() && SessionOwner->GetWorld() == LoadedWorld)
    {
        SpawnWorldEnvManager();
        return;
    }

    // Authority/standalone initialization belongs exclusively to the configured gameplay
    // GameMode's BeginPlay. PostLoadMap must never become a second lifecycle owner.
    if (Cast<AGlTFSimulatorGameplayGameModeBase>(LoadedWorld->GetAuthGameMode()))
    {
        return;
    }

    // Clients never own GameMode. They start their render/session side from the local controller;
    // the authoritative world key is subsequently reinforced by AMultiplayerWorldStateActor.
    if (LoadedWorld->GetNetMode() == NM_Client)
    {
        if (APlayerController* PC = LoadedWorld->GetFirstPlayerController())
        {
            StartClientGameplaySession(PC);
        }
        return;
    }

    // A world that resolves to a real .gwd but does not use the gameplay GameMode classes is a
    // configuration error. Do not spawn a substitute manager actor; GameMode is now the lifecycle owner.
    FString Candidate;
    const TCHAR* WorldOption = LoadedWorld->URL.GetOption(TEXT("World="), nullptr);
    if (WorldOption && FCString::Strlen(WorldOption) > 0)
    {
        Candidate = FString(WorldOption);
    }
    if (Candidate.IsEmpty())
    {
        Candidate = CurrentWorldName;
    }
    FString Normalized;
    if (TryNormalizeWorldFolderName(Candidate, Normalized, true))
    {
        UE_LOG(LogTemp, Error,
            TEXT("Gameplay world '%s' resolved .gwd '%s' but its active GameMode is not derived from AGlTFSimulatorGameplayGameModeBase. Assign SingleplayGameMode or MultiplayGameMode in World Settings / travel override."),
            *GetNameSafe(LoadedWorld), *Normalized);
    }
}

void UGameManagerSubSystem::RunPostLoadRuntimeMemoryCleanup()
{
    if (!bPendingMainWorldRuntimePurge)
    {
        return;
    }

    UWorld* CurrentWorld = GetWorld();
    const bool bNewWorldEnvManagerIsActive = bManagerStarted && SessionOwner.IsValid() &&
        SessionOwner->GetWorld() == CurrentWorld;
    if (bNewWorldEnvManagerIsActive)
    {
        // PostLoadMap runs before/around BeginPlay depending on the travel path. A deferred cleanup
        // from the previous map must never stop the streaming session just started by this world.
        bPendingMainWorldRuntimePurge = false;
        UE_LOG(LogTemp, Display,
            TEXT("[Gameplay] Deferred post-load purge skipped because the destination world manager is active: %s"),
            *GetNameSafe(CurrentWorld));
        return;
    }

    bPendingMainWorldRuntimePurge = false;

    // GameInstance subsystems survive OpenLevel. Clear only stale streaming state. If another startup
    // path already attached the subsystem to the destination world, preserving it is safer than
    // treating a persistent subsystem as old-world state.
    UWorldSceneStreamingSubsystem* GlobalStreamSubSystem = nullptr;
    if (UGameInstance* GameInstance = GetGameInstance())
    {
        GlobalStreamSubSystem = GameInstance->GetSubsystem<UWorldSceneStreamingSubsystem>();
    }
    if (GlobalStreamSubSystem && !GlobalStreamSubSystem->IsActiveForWorld(CurrentWorld))
    {
        GlobalStreamSubSystem->StopWorldStreaming();
    }

    HideLoadingWidget();
    ClearPlacementGridMesh();
    ReleaseMaterialDefaultAsset();
    ClearTransientRuntimeReferences();
    ResetWorldRuntimeReferences();

    RequestRuntimeGarbageCollection(TEXT("PostLoadMapRuntimeCleanup"));
}


void UGameManagerSubSystem::InitializeWorldSystems(UWorldData* InWorldData, const FString& InWorldRoot, const FString& InInitialPlayerName)
{
    ActiveWorldData = InWorldData;
    ApplyLevelSettings();

    // Keep gameplay-owned world actors centralized here: water and streamed world actors are not rendering concerns.
    SpawnOcean();
    StartGameplayWorldStreaming(InWorldRoot, InInitialPlayerName);
}

void UGameManagerSubSystem::StopWorldSystems()
{
    if (UWorldObjectStreamingSubsystem* Chunks = GetWorld()
        ? GetWorld()->GetSubsystem<UWorldObjectStreamingSubsystem>() : nullptr)
    {
        Chunks->Stop();
    }
    HideLoadingWidget();

    // Stop the streaming subsystem before destroying this manager so spawned stream actors release their assets cleanly.
    if (IsValid(StreamSubSystem))
    {
        StreamSubSystem->StopWorldStreaming();
        StreamSubSystem = nullptr;
    }
    else if (UWorldSceneStreamingSubsystem* GlobalStreamSubSystem = UWorldSceneStreamingSubsystem::Get(this))
    {
        // GameInstance subsystems persist after map travel. If our cached pointer was already cleared,
        // still force-stop the global streaming subsystem so built assets cannot stay resident.
        GlobalStreamSubSystem->StopWorldStreaming();
    }

    if (IsValid(OceanActor))
    {
        OceanActor->Destroy();
        OceanActor = nullptr;
    }

    if (IsValid(WorldEnvManagerActor))
    {
        WorldEnvManagerActor->StopRendering();
        if (bSpawnedWorldEnvManager)
        {
            WorldEnvManagerActor->Destroy();
        }
        WorldEnvManagerActor = nullptr;
    }

    bSpawnedWorldEnvManager = false;
    ActivePlayerData = nullptr;
    bCurrentLevelCheatsEnabled = false;
    ActiveWorldData = nullptr;
    SetWorldData(nullptr);
}

bool UGameManagerSubSystem::AreWorldSystemsReady() const
{
    const bool bStreamingReady = !IsValid(StreamSubSystem) || StreamSubSystem->IsInitialWorldReady();
    const bool bEntityRestoreResolved = bSavedSceneLoaded || bSavedSceneLoadFailed;
    return bStreamingReady && bEntityRestoreResolved;
}

float UGameManagerSubSystem::GetWorldSystemsLoadingStatus() const
{
    // Expose the manager-composed value rather than the raw model-stream value. The latter reaches
    // 100% before the reserved initial chunk restoration node and caused Blueprint loading bars to
    // display completion while saved actors were still pending.
    return bWorldLoadCompleted ? 1.0f : FMath::Clamp(LoadingStatus, 0.0f, 0.99f);
}

void UGameManagerSubSystem::InitializeRuntimeWorldState()
{
    if (bRuntimeWorldStateInitialized)
    {
        return;
    }
    // The subsystem survives OpenLevel, while the initial player transform is scoped to one world.
    // Reset its source/applied markers before reading the current .dat player data.
    ResetInitialPlayerTransformState();
    bRuntimeWorldStateInitialized = true;
    bWorldLoadCompleted = false;

    SetWorldLoading(true);
    SetLoadingStatus(FMath::Max(LoadingStatus, 0.70f));

    // Loading UI, world data, world rendering, water, and .gwd streaming now start from one owner.
    ShowLoadingWidget();
    LoadWorldData();
}

void UGameManagerSubSystem::SpawnWorldEnvManager()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }
    if (IsValid(WorldEnvManagerActor))
    {
        if (WorldEnvManagerActor->GetWorld() == World)
        {
            WorldEnvManagerActor->PrepareForWorldLoading();
            return;
        }

        // GameInstance subsystems survive map travel. Never mistake an actor that is still
        // finishing EndPlay in the source map for the destination world's environment manager.
        WorldEnvManagerActor = nullptr;
        bSpawnedWorldEnvManager = false;
    }

    UClass* EffectiveWorldEnvManagerClass = WorldEnvManagerClass.Get();
    if (!IsValid(EffectiveWorldEnvManagerClass)
        || !EffectiveWorldEnvManagerClass->IsChildOf(AWorldEnvManager::StaticClass())
        || EffectiveWorldEnvManagerClass->HasAnyClassFlags(
            CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
    {
        EffectiveWorldEnvManagerClass = AWorldEnvManager::StaticClass();
    }

    AWorldEnvManager* AnyExistingManager = nullptr;
    for (TActorIterator<AWorldEnvManager> It(World); It; ++It)
    {
        AWorldEnvManager* ExistingWorldEnvManager = *It;
        if (!IsValid(ExistingWorldEnvManager))
        {
            continue;
        }
        if (ExistingWorldEnvManager->IsA(EffectiveWorldEnvManagerClass))
        {
            WorldEnvManagerActor = ExistingWorldEnvManager;
            bSpawnedWorldEnvManager = false;
            ExistingWorldEnvManager->PrepareForWorldLoading();
            UE_LOG(LogTemp, Display,
                TEXT("Using placed WorldEnvManager before world-data I/O. Actor=%s Class=%s"),
                *GetNameSafe(ExistingWorldEnvManager),
                *GetNameSafe(ExistingWorldEnvManager->GetClass()));
            return;
        }
        if (!IsValid(AnyExistingManager))
        {
            AnyExistingManager = ExistingWorldEnvManager;
        }
    }

    // A placed native/Blueprint environment is preferable to creating a duplicate merely because
    // the manager's configured subclass changed. Its component contract is defined by the native
    // base class and remains sufficient for the procedural fallback sky.
    if (IsValid(AnyExistingManager))
    {
        WorldEnvManagerActor = AnyExistingManager;
        bSpawnedWorldEnvManager = false;
        AnyExistingManager->PrepareForWorldLoading();
        UE_LOG(LogTemp, Warning,
            TEXT("Using placed WorldEnvManager '%s' because no placed actor matched configured class '%s'."),
            *GetNameSafe(AnyExistingManager),
            *GetNameSafe(EffectiveWorldEnvManagerClass));
        return;
    }

    FActorSpawnParameters Params;
    Params.Owner = SessionOwner.Get();
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    WorldEnvManagerActor = World->SpawnActor<AWorldEnvManager>(EffectiveWorldEnvManagerClass, FTransform::Identity, Params);

    // A stale/missing Blueprint generated class must not remove the entire sky. Retry with the
    // native class, which owns all required components and needs no content asset to render.
    if (!IsValid(WorldEnvManagerActor)
        && EffectiveWorldEnvManagerClass != AWorldEnvManager::StaticClass())
    {
        UE_LOG(LogTemp, Error,
            TEXT("Configured WorldEnvManager class failed to spawn; retrying native fallback. Class=%s"),
            *GetNameSafe(EffectiveWorldEnvManagerClass));
        WorldEnvManagerActor = World->SpawnActor<AWorldEnvManager>(
            AWorldEnvManager::StaticClass(), FTransform::Identity, Params);
    }
    bSpawnedWorldEnvManager = IsValid(WorldEnvManagerActor);
    if (bSpawnedWorldEnvManager)
    {
        WorldEnvManagerActor->PrepareForWorldLoading();
        UE_LOG(LogTemp, Display,
            TEXT("WorldEnvManager spawned before world-data I/O. Actor=%s Class=%s"),
            *GetNameSafe(WorldEnvManagerActor),
            *GetNameSafe(WorldEnvManagerActor->GetClass()));
    }
    else
    {
        UE_LOG(LogTemp, Error,
            TEXT("WorldEnvManager could not be spawned; procedural sky is unavailable for this world."));
    }
}

bool UGameManagerSubSystem::CheckWorldSystemsLoaded()
{
    // Reserve the final progress node for initial chunk validation and actor restoration. Without
    // this reservation the streaming subsystem could publish 100% and hide the loading screen
    // before saved Static objects/vehicles had actually been recreated in the destination world.
    constexpr float EntityRestoreProgressStart = 0.96f;

    if (!IsValid(StreamSubSystem))
    {
        SetLoadingStatus(EntityRestoreProgressStart);
        return true;
    }

    // Readiness can start the selected-player load, so evaluate it before sampling progress. The
    // streaming subsystem counts cached, calculated, duplicate, invalid, and distance-skipped model
    // nodes. Map that complete node graph into 0..96%, leaving one truthful final restoration node.
    const bool bSystemsReady = StreamSubSystem->IsInitialWorldReady();
    const float Percent = StreamSubSystem->GetLoadingStatus();
    const bool bVisibleProgressComplete = Percent >= 1.0f - KINDA_SMALL_NUMBER;
    constexpr float RuntimeProgressStart = 0.70f;
    SetLoadingStatus(FMath::Lerp(
        RuntimeProgressStart,
        EntityRestoreProgressStart,
        FMath::Clamp(Percent, 0.0f, 1.0f)));
    return bSystemsReady && bVisibleProgressComplete;
}

void UGameManagerSubSystem::LoadWorldData()
{
    check(IsInGameThread());
    const FString RequestedRoot = GetWorldRootPath();
    const TWeakObjectPtr<AActor> StartupOwner = SessionOwner;
    UModelDatabaseSubsystem* Database = GetGameInstance()
        ? GetGameInstance()->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> ArchiveReader =
        Database ? Database->GetArchiveReader() : nullptr;
    if (!ArchiveReader.IsValid())
    {
        FailWorldStartup(TEXT("World startup failed: no .gwd reader is available for config.json."));
        return;
    }
    TWeakObjectPtr<UGameManagerSubSystem> WeakThis(this);
    const bool bQueued = FSafeFileIO::RunTrackedWorker([WeakThis, ArchiveReader, RequestedRoot, StartupOwner]()
    {
        FSafeJsonLimits Limits;
        Limits.MaxFileBytes = 64ll * 1024ll * 1024ll;
        Limits.bAllowBackupRecovery = false;
        FString ConfigText;
        FString ConfigError;
        FSafeJsonLoadResult Config;
        if (ArchiveReader->ReadWorldConfig(ConfigText, ConfigError))
        {
            Config = FSafeFileIO::ParseJsonText(ConfigText, TEXT(".gwd/config.json"), Limits);
        }
        else
        {
            Config.Status = ESafeFileIOStatus::ReadFailed;
            Config.Path = ArchiveReader->GetArchivePath() + TEXT("#config.json");
            Config.Error = MoveTemp(ConfigError);
        }
        FSafeFileIO::DispatchTrackedGameThread(
            [WeakThis, RequestedRoot, StartupOwner, Config = MoveTemp(Config)]() mutable
            {
                UGameManagerSubSystem* StrongThis = WeakThis.Get();
                if (!IsValid(StrongThis) || !StrongThis->bManagerStarted
                    || !StartupOwner.IsValid() || StrongThis->SessionOwner != StartupOwner
                    || StrongThis->GetWorldRootPath() != RequestedRoot) return;

                // config.json is immutable author/world configuration. Mutable time/player state is
                // range-read from the state record in the single .dat commit log.
                TFunction<void(bool, bool, FWorldRuntimeState, FString)> ApplyLoadedState =
                    [WeakThis, RequestedRoot, StartupOwner, Config = MoveTemp(Config)](
                        const bool bStateLoaded,
                        const bool bStateMissing,
                        FWorldRuntimeState State,
                        FString StateError) mutable
                {
                    UGameManagerSubSystem* Manager = WeakThis.Get();
                    if (!IsValid(Manager) || !Manager->bManagerStarted
                        || !StartupOwner.IsValid() || Manager->SessionOwner != StartupOwner
                        || Manager->GetWorldRootPath() != RequestedRoot) return;

                    Manager->ActiveWorldData = NewObject<UWorldData>(Manager);
                    Manager->ActivePlayerData = NewObject<UPlayerData>(Manager);
                    if (!IsValid(Manager->ActiveWorldData)
                        || !IsValid(Manager->ActivePlayerData))
                    {
                        Manager->FailWorldStartup(
                            TEXT("World startup failed: runtime world state could not be allocated."));
                        return;
                    }

                    if (!Config.IsSuccess()
                        || !UWorldData::DeserializeData(
                            Manager->ActiveWorldData, Config.JsonObject))
                    {
                        UE_LOG(LogTemp, Warning,
                            TEXT("config.json invalid or missing; defaults used. Path=%s Reason=%s"),
                            *Config.Path, *Config.Error);
                    }
                    if (bStateLoaded && !bStateMissing)
                    {
                        Manager->ActiveWorldData->WorldTime = State.WorldTime;
                        Manager->ActiveWorldData->Player = State.SelectedPlayer;
                        Manager->ActivePlayerData->Players = MoveTemp(State.Players);
                    }
                    else
                    {
                        Manager->bPendingInitialWorldDataSave = true;
                        Manager->bPendingInitialPlayerDataSave = true;
                        if (!StateError.IsEmpty())
                        {
                            UE_LOG(LogTemp, Warning,
                                TEXT(".dat runtime state unavailable; defaults will be saved. Reason=%s"),
                                *StateError);
                        }
                    }

                    Manager->SetWorldData(Manager->ActiveWorldData);
                    Manager->LoadPlayerData();
                    Manager->ApplyLevelSettings();
                    Manager->SpawnWorldEnvManager();
                    if (IsValid(Manager->WorldEnvManagerActor))
                    {
                        Manager->WorldEnvManagerActor->InitializeRendering(
                            Manager->ActiveWorldData);
                    }
                    Manager->InitializeWorldSystems(
                        Manager->ActiveWorldData,
                        Manager->GetWorldRootPath(),
                        Manager->ActivePlayerId);

                    // StartGameplayWorldStreaming reports terminal startup rejection through
                    // FailWorldStartup. Do not start the next-tick poll after that failure or the
                    // loading transaction would live forever even though its overlay is hidden.
                    if (Manager->bIsWorldLoading)
                    {
                        Manager->LoadWorldAsync();
                    }
                };

                UWorldObjectStreamingSubsystem* Chunks = StrongThis->GetWorld()
                    ? StrongThis->GetWorld()->GetSubsystem<UWorldObjectStreamingSubsystem>() : nullptr;
                if (Chunks && Chunks->IsRunning())
                {
                    Chunks->LoadRuntimeStateAsync(MoveTemp(ApplyLoadedState));
                }
                else
                {
                    ApplyLoadedState(false, true, FWorldRuntimeState(),
                        TEXT("the .dat streamer is unavailable"));
                }
            });
    });
    if (!bQueued)
    {
        FailWorldStartup(
            TEXT("World startup failed: config.json cannot be read because asynchronous file I/O is shutting down."));
    }
}

void UGameManagerSubSystem::LoadPlayerData()
{
    check(IsInGameThread());
    bInitialPlayerDataLoadCompleted = false;
    if (!IsValid(ActivePlayerData)) ActivePlayerData = NewObject<UPlayerData>(this);
    if (!IsValid(ActivePlayerData))
    {
        bInitialPlayerDataLoadCompleted = true;
        return;
    }
    ActivePlayerId = IsValid(ActiveWorldData) && !ActiveWorldData->Player.IsEmpty()
        ? FPaths::GetCleanFilename(ActiveWorldData->Player) : TEXT("Player");
    const FWorldPlayerRecord* Existing = ActivePlayerData->FindPlayer(ActivePlayerId);
    if (Existing && IsFiniteWorldLocation(Existing->Location))
    {
        PlayerLocation = Existing->Location;
        InitialPlayerLocationSource = EInitialPlayerLocationSource::EntityArchive;
        LoadedInitialPlayerRotation = Existing->Rotation.GetNormalized();
        bHasLoadedInitialPlayerRotation = IsFiniteRotation(LoadedInitialPlayerRotation);
    }
    else
    {
        FWorldPlayerRecord& Created = ActivePlayerData->FindOrAddPlayer(ActivePlayerId);
        Created.DisplayName = ActivePlayerId;
        bPendingInitialPlayerDataSave = true;
    }
    bInitialPlayerDataLoadCompleted = true;
    TryResolveInitialPlayerTransform();
}

void UGameManagerSubSystem::ResetInitialPlayerTransformState()
{
    InitialPlayerLocationSource = EInitialPlayerLocationSource::None;
    LoadedInitialPlayerRotation = FRotator::ZeroRotator;
    bHasLoadedInitialPlayerRotation = false;
    bInitialPlayerDataLoadCompleted = false;
    bInitialPlayerTransformResolved = false;
    bPendingInitialControlRotation = false;
    bPendingInitialWorldDataSave = false;
    bPendingInitialPlayerDataSave = false;

    // A Character may already have begun play. Keep its PlayerStart location as a provisional
    // seed, but do not finalize it until LoadPlayerData proves that no valid save supersedes it.
    AActor* RegisteredPlayer = PlayerActor.Get();
    if (IsValid(RegisteredPlayer) && RegisteredPlayer->GetWorld() != GetWorld())
    {
        PlayerActor = nullptr;
        RegisteredPlayer = nullptr;
    }
    const FVector RegisteredLocation = IsValid(RegisteredPlayer)
        ? RegisteredPlayer->GetActorLocation()
        : FVector::ZeroVector;
    PlayerLocation = IsFiniteWorldLocation(RegisteredLocation)
        ? RegisteredLocation
        : FVector::ZeroVector;
}

void UGameManagerSubSystem::TryResolveInitialPlayerTransform()
{
    AActor* RegisteredPlayer = PlayerActor.Get();
    UWorld* World = GetWorld();
    if (bInitialPlayerTransformResolved
        || !bInitialPlayerDataLoadCompleted
        || !IsValid(RegisteredPlayer))
    {
        return;
    }

    if (!World || RegisteredPlayer->GetWorld() != World)
    {
        PlayerActor = nullptr;
        return;
    }

    if (World->GetNetMode() == NM_Client)
    {
        bInitialPlayerTransformResolved = true;
        bPendingInitialControlRotation = false;
        return;
    }

    FVector ResolvedLocation = RegisteredPlayer->GetActorLocation();
    FRotator ResolvedRotation = RegisteredPlayer->GetActorRotation();
    bool bShouldRestoreSavedControlRotation = false;
    if (!IsFiniteWorldLocation(ResolvedLocation))
    {
        UE_LOG(LogTemp, Error,
            TEXT("[PlayerSpawn] Cannot seed the initial player transform from invalid Pawn location %s."),
            *ResolvedLocation.ToString());
        return;
    }
    if (!IsFiniteRotation(ResolvedRotation))
    {
        ResolvedRotation = FRotator::ZeroRotator;
    }

    if (InitialPlayerLocationSource != EInitialPlayerLocationSource::None)
    {
        ResolvedLocation = PlayerLocation;
        if (bHasLoadedInitialPlayerRotation)
        {
            ResolvedRotation = LoadedInitialPlayerRotation;
            bShouldRestoreSavedControlRotation = true;
        }

        // This is the sole initial teleport. The resolved marker is set immediately afterward so
        // SetPlayerActor calls from later runtime GLB/Pawn replacement cannot replay the save.
        const bool bApplied = RegisteredPlayer->SetActorLocationAndRotation(
            ResolvedLocation,
            ResolvedRotation,
            false,
            nullptr,
            ETeleportType::TeleportPhysics);
        if (!bApplied)
        {
            UE_LOG(LogTemp, Error,
                TEXT("[PlayerSpawn] Failed to apply the validated saved transform to %s; "
                     "preserving its actual transform instead."),
                *GetNameSafe(RegisteredPlayer));

            const FVector ActualLocation = RegisteredPlayer->GetActorLocation();
            const FRotator ActualRotation = RegisteredPlayer->GetActorRotation();
            if (!IsFiniteWorldLocation(ActualLocation))
            {
                // Leave the handshake unresolved so a later valid registration can retry.
                return;
            }

            ResolvedLocation = ActualLocation;
            ResolvedRotation = IsFiniteRotation(ActualRotation)
                ? ActualRotation.GetNormalized()
                : FRotator::ZeroRotator;
            bShouldRestoreSavedControlRotation = false;
        }

        if (bShouldRestoreSavedControlRotation)
        {
            // Keep this pending even when the controller already exists: this function can run
            // from OnPossess, before FinishRestartPlayer performs its final PlayerStart override.
            bPendingInitialControlRotation = true;
            if (const APawn* Pawn = Cast<APawn>(RegisteredPlayer))
            {
                if (AController* Controller = Pawn->GetController())
                {
                    // CharacterMovement/camera code may immediately consume control rotation, so
                    // restore it with the actor rotation instead of letting it overwrite the save.
                    Controller->SetControlRotation(ResolvedRotation);
                }
            }
        }
    }

    PlayerLocation = ResolvedLocation;
    bInitialPlayerTransformResolved = true;

    if (bPendingInitialControlRotation)
    {
        // The controller's OnPossess callback also consumes this flag. Scheduling here covers the
        // inverse initialization order where the manager resolves after OnPossess has already run.
        World->GetTimerManager().SetTimerForNextTick(
            FTimerDelegate::CreateWeakLambda(this, [this]()
            {
                if (UWorld* CurrentWorld = GetWorld())
                {
                    ApplyPendingInitialPlayerControlRotation(
                        CurrentWorld->GetFirstPlayerController());
                }
            }));
    }

    if (IsValid(ActiveWorldData))
    {
        ActiveWorldData->PlayerLocation = ResolvedLocation;
    }
    if (IsValid(ActivePlayerData))
    {
        FWorldPlayerRecord& PlayerRecord = ActivePlayerData->FindOrAddPlayer(ActivePlayerId);
        PlayerRecord.Location = ResolvedLocation;
        PlayerRecord.Rotation = ResolvedRotation;
    }

    FlushPendingInitialTransformSaves();
}

void UGameManagerSubSystem::FlushPendingInitialTransformSaves()
{
    // Save functions clear their own pending bit only after a validated transform is serializable.
    if (bPendingInitialWorldDataSave)
    {
        SaveWorldData();
    }
    if (bPendingInitialPlayerDataSave)
    {
        SavePlayerData();
    }
}

void UGameManagerSubSystem::SaveWorldData()
{
    check(IsInGameThread());
    if (const UWorld* World = GetWorld(); World && World->GetNetMode() == NM_Client) return;
    if (!IsValid(ActiveWorldData) || !IsValid(ActivePlayerData) || !FMath::IsFinite(ActiveWorldData->WorldTime)) return;
    FWorldRuntimeState Snapshot;
    Snapshot.WorldTime = ActiveWorldData->WorldTime;
    Snapshot.SelectedPlayer = FPaths::GetCleanFilename(ActiveWorldData->Player);
    Snapshot.Players = ActivePlayerData->Players;
    UWorldObjectStreamingSubsystem* Chunks = GetWorld()
        ? GetWorld()->GetSubsystem<UWorldObjectStreamingSubsystem>() : nullptr;
    if (!Chunks || !Chunks->IsRunning()) return;
    Chunks->SaveRuntimeStateAsync(Snapshot, [](FSafeFileWriteResult Result)
    {
        if (!Result.IsSuccess() && Result.Status != ESafeFileIOStatus::ShuttingDown
            && Result.Status != ESafeFileIOStatus::Superseded)
            UE_LOG(LogTemp, Error, TEXT(".dat transactional save failed. Path=%s Reason=%s"), *Result.Path, *Result.Error);
    });
    bPendingInitialWorldDataSave = false;
}

void UGameManagerSubSystem::SavePlayerData()
{
    check(IsInGameThread());
    if (const UWorld* World = GetWorld(); World && World->GetNetMode() == NM_Client) return;
    if (!IsValid(ActivePlayerData) || !IsFiniteWorldLocation(PlayerLocation)) return;
    FWorldPlayerRecord& PlayerRecord = ActivePlayerData->FindOrAddPlayer(ActivePlayerId);
    PlayerRecord.Location = PlayerLocation;
    if (const AActor* Player = PlayerActor.Get(); IsValid(Player) && IsFiniteRotation(Player->GetActorRotation()))
        PlayerRecord.Rotation = Player->GetActorRotation().GetNormalized();
    SaveWorldData();
    bPendingInitialPlayerDataSave = false;
}

void UGameManagerSubSystem::SetSelectedPlayerForRuntime(const FString& PlayerId)
{
    if (!IsValid(ActiveWorldData))
    {
        return;
    }

    FString SafeName = FPaths::GetCleanFilename(PlayerId);
    SafeName.TrimStartAndEndInline();
    if (SafeName.Contains(TEXT("..")) || SafeName.Contains(TEXT("/")) || SafeName.Contains(TEXT("\\")))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Rejected unsafe selected-player name: %s"),
            *PlayerId);
        return;
    }

    ActiveWorldData->Player = SafeName;
    ActivePlayerId = SafeName.IsEmpty() ? FString(TEXT("Player")) : SafeName;
    if (IsValid(ActivePlayerData))
    {
        ActivePlayerData->FindOrAddPlayer(ActivePlayerId);
    }
    SaveWorldData();
}

void UGameManagerSubSystem::ValidateResolvedGameMode() const
{
    const UWorld* World = GetWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Warning, TEXT("[GameModeTravel] Cannot validate GameMode because no UWorld is available."));
        return;
    }

    const AWorldSettings* WorldSettings = World->GetWorldSettings();
    UClass* MapGameModeClass = WorldSettings ? WorldSettings->DefaultGameMode.Get() : nullptr;
    const FString MapGameModePath = IsValid(MapGameModeClass)
        ? MapGameModeClass->GetPathName()
        : FString();

    const UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this);
    const TSoftClassPtr<AGameModeBase> RequestedOverride = IsValid(Multiplayer)
        ? Multiplayer->GetRequestedGameModeOverride()
        : TSoftClassPtr<AGameModeBase>();
    const FString RequestedOverridePath = RequestedOverride.IsNull()
        ? FString()
        : RequestedOverride.ToSoftObjectPath().ToString();
    const FString RequestedFolder = IsValid(Multiplayer)
        ? Multiplayer->GetRequestedGameModeWorldFolder()
        : FString();

    const TCHAR* UrlGameModeOption = World->URL.GetOption(TEXT("game="), nullptr);
    const FString UrlGameMode = UrlGameModeOption ? FString(UrlGameModeOption) : FString();

    // GameMode exists only on the authority. Network clients receive GameState/PlayerState instead.
    if (World->GetNetMode() == NM_Client)
    {
        UE_LOG(LogTemp, Display,
            TEXT("[GameModeTravel] Client world loaded. Map=%s RequestedFolder=%s; authoritative GameMode is owned by the server."),
            *World->GetMapName(),
            RequestedFolder.IsEmpty() ? TEXT("<none>") : *RequestedFolder);
        return;
    }

    const AGameModeBase* ActiveGameMode = World->GetAuthGameMode();
    if (!IsValid(ActiveGameMode))
    {
        UE_LOG(LogTemp, Error,
            TEXT("[GameModeTravel] No authoritative GameMode exists after map initialization. Map=%s MapOverride=%s URLGame=%s"),
            *World->GetMapName(),
            MapGameModePath.IsEmpty() ? TEXT("<project default>") : *MapGameModePath,
            UrlGameMode.IsEmpty() ? TEXT("<none>") : *UrlGameMode);
        return;
    }

    UClass* ActiveGameModeClass = ActiveGameMode->GetClass();
    const FString ActiveGameModePath = IsValid(ActiveGameModeClass)
        ? ActiveGameModeClass->GetPathName()
        : FString(TEXT("<invalid>"));

    FString ExpectedGameModePath;
    FString ResolutionSource;
    if (!RequestedOverridePath.IsEmpty())
    {
        ExpectedGameModePath = RequestedOverridePath;
        ResolutionSource = TEXT("folder launch profile");
    }
    else if (!MapGameModePath.IsEmpty())
    {
        ExpectedGameModePath = MapGameModePath;
        ResolutionSource = TEXT("map World Settings");
    }
    else
    {
        ResolutionSource = TEXT("project default");
    }

    const bool bExpectedClassMismatch = !ExpectedGameModePath.IsEmpty()
        && !ActiveGameModePath.Equals(ExpectedGameModePath, ESearchCase::CaseSensitive);
    if (bExpectedClassMismatch)
    {
        UE_LOG(LogTemp, Error,
            TEXT("[GameModeTravel] GameMode mismatch. Map=%s Folder=%s Expected=%s Active=%s Source=%s URLGame=%s"),
            *World->GetMapName(),
            RequestedFolder.IsEmpty() ? TEXT("<none>") : *RequestedFolder,
            *ExpectedGameModePath,
            *ActiveGameModePath,
            *ResolutionSource,
            UrlGameMode.IsEmpty() ? TEXT("<none>") : *UrlGameMode);
        return;
    }

    if (ExpectedGameModePath.IsEmpty() && ActiveGameModeClass == AGameModeBase::StaticClass())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[GameModeTravel] Map=%s has no map/profile GameMode override and resolved to bare GameModeBase. "
                 "Assign a valid World Settings override, launch-profile override, or project default."),
            *World->GetMapName());
    }

    UE_LOG(LogTemp, Display,
        TEXT("[GameModeTravel] Active GameMode confirmed. Map=%s Folder=%s Active=%s Source=%s MapOverride=%s URLGame=%s"),
        *World->GetMapName(),
        RequestedFolder.IsEmpty() ? TEXT("<none>") : *RequestedFolder,
        *ActiveGameModePath,
        *ResolutionSource,
        MapGameModePath.IsEmpty() ? TEXT("<none>") : *MapGameModePath,
        UrlGameMode.IsEmpty() ? TEXT("<none>") : *UrlGameMode);
}

void UGameManagerSubSystem::ApplyLevelSettings()
{
    ApplyGameplaySettings();

    if (UGameInstance* GameInstance = GetGameInstance())
    {
        if (UWeatherSubsystem* Weather = GameInstance->GetSubsystem<UWeatherSubsystem>())
        {
            Weather->ConfigureWeatherActorClass(RainWeatherActorClass);
            Weather->SetWeatherCamera(CurrentCamera.Get());
            Weather->ConfigureFromWorldData(ActiveWorldData);
        }
    }
}

void UGameManagerSubSystem::ApplyGameplaySettings()
{
    bCurrentLevelCheatsEnabled = IsValid(ActiveWorldData) && ActiveWorldData->Gameplay.bCheatsEnabled;

    // UGameManagerSubSystem survives OpenLevel. Resolve the mode from a clean per-map baseline on
    // every world load; otherwise an unrecognized key can leave the previous world's mode active.
    const EPlayMode ConfiguredDefault = PlayMode;
    const FString RuntimeModeKey = IsValid(ActiveWorldData)
        ? ActiveWorldData->Gameplay.WorldGameMode
        : FString();
    bool bRecognizedModeKey = false;
    PlayMode = ResolveRuntimePlayModeKey(RuntimeModeKey, ConfiguredDefault, bRecognizedModeKey);

    if (!bRecognizedModeKey)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[RuntimePlayMode] Unknown Runtime Play Mode Key '%s' in config.json. Using GameplayGameMode default '%s'."),
            *RuntimeModeKey,
            ConfiguredDefault == EPlayMode::Creator ? TEXT("Creator") : TEXT("RealLife"));
    }

    UE_LOG(LogTemp, Display,
        TEXT("[RuntimePlayMode] Runtime play mode resolved. Key=%s Active=%s"),
        RuntimeModeKey.IsEmpty() ? TEXT("<default>") : *RuntimeModeKey,
        PlayMode == EPlayMode::Creator ? TEXT("Creator") : TEXT("RealLife"));

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
        // Re-run the idempotent .dat restore when world streaming is ready.
        // The earlier next-tick attempt can legitimately occur before the persistent GameInstance
        // streaming subsystem has attached itself to this destination UWorld.
        if (!bSavedSceneLoaded && !bSavedSceneLoadInProgress && !bSavedSceneLoadFailed)
        {
            LoadSavedScene();
        }

        // The initial chunk set is a real loading node, not a post-load side effect. Wait while validation or
        // a bounded retry is pending. A terminal/partial failure is allowed to enter the world, but
        // SaveScene remains locked so the unread generation cannot be overwritten by an empty one.
        if (!bSavedSceneLoaded && !bSavedSceneLoadFailed)
        {
            SetLoadingStatus(0.96f);
            if (UWorld* World = GetWorld())
            {
                World->GetTimerManager().SetTimerForNextTick(this, &UGameManagerSubSystem::LoadWorldAsync);
            }
            return;
        }

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

bool UGameManagerSubSystem::SetWorldTimeSeconds(const double Seconds)
{
    if (!IsInGameThread() || !IsValid(ActiveWorldData) || !FMath::IsFinite(Seconds))
    {
        return false;
    }
    if (const UWorld* World = GetWorld(); World && World->GetNetMode() == NM_Client)
    {
        return false;
    }

    // Keep astronomical/sky calculations in a numerically useful range even if a malformed console
    // or future chat command supplies an extreme double. WorldTime is stored as float.
    constexpr double MaxSafeWorldTimeSeconds = 1.0e12;
    ActiveWorldData->WorldTime = static_cast<float>(FMath::Clamp(Seconds, 0.0, MaxSafeWorldTimeSeconds));
    return true;
}

bool UGameManagerSubSystem::AddWorldTimeSeconds(const double DeltaSeconds)
{
    if (!IsValid(ActiveWorldData) || !FMath::IsFinite(DeltaSeconds))
    {
        return false;
    }
    return SetWorldTimeSeconds(static_cast<double>(ActiveWorldData->WorldTime) + DeltaSeconds);
}

bool UGameManagerSubSystem::SetWorldDay(const double DayNumber)
{
    if (!IsValid(ActiveWorldData) || !FMath::IsFinite(DayNumber) || DayNumber < 0.0)
    {
        return false;
    }

    const double SecondsPerDay = FMath::Max(1.0, static_cast<double>(ActiveWorldData->OneDayTime));
    const double CurrentSeconds = FMath::Max(0.0, static_cast<double>(ActiveWorldData->WorldTime));
    const double TimeOfDay = FMath::Fmod(CurrentSeconds, SecondsPerDay);
    return SetWorldTimeSeconds(DayNumber * SecondsPerDay + TimeOfDay);
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

    if (!FMath::IsFinite(DeltaSeconds) || !FMath::IsFinite(ActiveWorldData->TimeSpeed))
    {
        return;
    }

    constexpr double MaxSafeWorldTimeSeconds = 1.0e12;
    const double AdvancedTime = static_cast<double>(ActiveWorldData->WorldTime) +
        static_cast<double>(DeltaSeconds) * static_cast<double>(ActiveWorldData->TimeSpeed);
    ActiveWorldData->WorldTime = static_cast<float>(FMath::Clamp(AdvancedTime, 0.0, MaxSafeWorldTimeSeconds));

    // Player transforms live only in WorldName.dat. Keep the in-memory mirrors current so the
    // next periodic binary save has an immutable validated snapshot.
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
    const FString WorldRootPath = GetWorldRootPath();
    return WorldRootPath.IsEmpty()
        ? FString()
        : FPaths::Combine(WorldRootPath, FPaths::GetCleanFilename(FileName));
}

bool UGameManagerSubSystem::HasLoadingWidget() const
{
    return IsValid(LoadingWidgetInstance.Get());
}

void UGameManagerSubSystem::SetLoadingWidget(UUserWidget* InWidget)
{
    if (LoadingWidgetInstance == InWidget)
    {
        return;
    }
    if (IsValid(LoadingWidgetInstance.Get()))
    {
        LoadingWidgetInstance->SetVisibility(ESlateVisibility::Collapsed);
    }
    LoadingWidgetInstance = InWidget;
    if (IsValid(LoadingWidgetInstance.Get()))
    {
        LoadingWidgetInstance->SetVisibility(bIsWorldLoading
            ? ESlateVisibility::Visible
            : ESlateVisibility::Collapsed);
    }
}

void UGameManagerSubSystem::ShowLoadingWidget()
{
    if (!IsValid(LoadingWidgetInstance.Get()))
    {
        UGlTFSimulatorAssetRegistry* Registry = UGlTFSimulatorGameInstance::GetAssetRegistryFromContext(this);
        APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
        if (IsValid(Registry) && IsValid(PlayerController) && PlayerController->IsLocalController()
            && !Registry->LoadingWidgetClass.IsNull())
        {
            if (UClass* WidgetClass = Registry->LoadingWidgetClass.LoadSynchronous())
            {
                UUserWidget* Widget = CreateWidget<UUserWidget>(PlayerController, WidgetClass);
                if (IsValid(Widget))
                {
                    Widget->AddToPlayerScreen(100);
                    SetLoadingWidget(Widget);
                }
            }
        }
    }

    if (IsValid(LoadingWidgetInstance.Get()))
    {
        LoadingWidgetInstance->SetVisibility(ESlateVisibility::Visible);
        if (!LoadingWidgetInstance->IsInViewport())
        {
            LoadingWidgetInstance->AddToPlayerScreen(100);
        }
    }

    if (APlayerCharacterController* PlayerController =
            Cast<APlayerCharacterController>(UGameplayStatics::GetPlayerController(this, 0)))
    {
        PlayerController->ApplyLoadingInputMode(LoadingWidgetInstance.Get());
    }
}

void UGameManagerSubSystem::HideLoadingWidget()
{
    if (IsValid(LoadingWidgetInstance.Get()))
    {
        LoadingWidgetInstance->SetVisibility(ESlateVisibility::Collapsed);
    }
}

bool UGameManagerSubSystem::ShouldSpawnOcean() const
{
    return IsValid(ActiveWorldData) && ActiveWorldData->bOcean;
}

void UGameManagerSubSystem::SpawnOcean()
{
    if (IsValid(OceanActor))
    {
        return;
    }
    if (!ShouldSpawnOcean())
    {
        UE_LOG(LogTemp, Display,
            TEXT("Global ocean disabled by world config (bOcean=false)."));
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = SessionOwner.Get();
    // Water is an overlap volume. Never let existing world collision suppress the global ocean.
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    UClass* OceanClass = WaterClass ? WaterClass.Get() : AWaterActor::StaticClass();
    if (!IsValid(OceanClass) || !OceanClass->IsChildOf(AWaterActor::StaticClass()))
    {
        OceanClass = AWaterActor::StaticClass();
    }
    const FTransform& HardcodedOceanTransform = GetHardcodedOceanTransform();
    OceanTransform = HardcodedOceanTransform;
    OceanActor = World->SpawnActor<AActor>(OceanClass, HardcodedOceanTransform, SpawnParams);
    if (!IsValid(OceanActor) && OceanClass != AWaterActor::StaticClass())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Configured water actor class failed to spawn ocean; retrying native AWaterActor. Class=%s"),
            *GetNameSafe(OceanClass));
        OceanActor = World->SpawnActor<AActor>(AWaterActor::StaticClass(), HardcodedOceanTransform, SpawnParams);
    }

    if (IsValid(OceanActor))
    {
        OceanActor->SetActorTransform(HardcodedOceanTransform, false, nullptr, ETeleportType::TeleportPhysics);
        UE_LOG(LogTemp, Display,
            TEXT("Global ocean spawned with native hardcoded transform. Class=%s Location=%s Scale=%s"),
            *GetNameSafe(OceanActor->GetClass()),
            *HardcodedOceanTransform.GetLocation().ToCompactString(),
            *HardcodedOceanTransform.GetScale3D().ToCompactString());
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Global ocean spawn failed even after native AWaterActor fallback."));
    }
}

void UGameManagerSubSystem::StartGameplayWorldStreaming(const FString& InWorldRoot, const FString& InInitialPlayerName)
{
    UWorld* World = GetWorld();
    if (!World)
    {
        FailWorldStartup(TEXT("World startup failed: no UWorld is available for streaming."));
        return;
    }

    StreamSubSystem = UWorldSceneStreamingSubsystem::Get(this);
    if (!IsValid(StreamSubSystem))
    {
        FailWorldStartup(TEXT("World startup failed: the world streaming subsystem could not be created."));
        return;
    }

    const bool bRenderOnlyStreaming = UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this);
    StreamSubSystem->StartWorldStreaming(
        SessionOwner.Get(),
        InWorldRoot,
        InInitialPlayerName,
        bRenderOnlyStreaming);

    // StartWorldStreaming deliberately refuses every non-.gwd source. Convert that
    // refusal into a terminal runtime-start failure here so LoadWorldAsync cannot spin forever.
    if (StreamSubSystem->HasStartupFailed()
        || !StreamSubSystem->IsActiveForWorld(World))
    {
        StreamSubSystem = nullptr;
        FailWorldStartup(
            TEXT("World startup failed: the verified .gwd streaming session could not be started."));
    }
}

void UGameManagerSubSystem::UpdateGameManager(float DeltaSeconds)
{
    UpdateWorldTime(DeltaSeconds);

    if (PlayMode != EPlayMode::Creator || CurrentMode == EToolMode::None)
    {
        ClearPlacementGridMesh();
        return;
    }

    FHitResult Hit;
    FVector Preview;
    if (TracePlacementLocation(Preview, Hit))
    {
        LastPreviewLocation = CurrentMode == EToolMode::PlaceVehicle
            ? Preview
            : ApplyGridSnap(Preview);
    }

    UpdatePlacementGrid();
}

void UGameManagerSubSystem::ClearTransientRuntimeReferences()
{
    // Weather is a GameInstance subsystem, so explicitly release its world actor/timer before
    // dropping camera/world references during travel.
    if (UGameInstance* GameInstance = GetGameInstance())
    {
        if (UWeatherSubsystem* Weather = GameInstance->GetSubsystem<UWeatherSubsystem>())
        {
            Weather->StopWeather();
        }
    }

    // GameInstanceSubsystems persist across level travel. Drop every strong reference to
    // gameplay actors, components, streamed assets, UI, and world data so GC can reclaim them.
    PlayerActor = nullptr;
    CurrentCamera = nullptr;
    PostProcess = nullptr;
    CurrentWorldData = nullptr;
    ActiveWorldData = nullptr;
    LoadingWidgetInstance = nullptr;
    StreamSubSystem = nullptr;
    OceanActor = nullptr;
    WorldEnvManagerActor = nullptr;
    Root = nullptr;
    PlacementGridComponent = nullptr;
    PlacementGridMaterial = nullptr;

    // These class references may point at Blueprint packages with large dependency graphs. Revert to
    // lightweight native defaults (or null for optional systems) once the world session is gone.
    StaticActorClass = AStaticActor::StaticClass();
    DynamicActorClass = ADynamicActor::StaticClass();
    VehiclePawnClass = AVehiclePawn::StaticClass();
    WeaponActorClass = AWeaponActor::StaticClass();
    WorldEnvManagerClass = AWorldEnvManager::StaticClass();
    WaterClass = nullptr;
    RainWeatherActorClass = nullptr;
    OceanTransform = FTransform::Identity;

    EquippedWeapon = nullptr;
    SpawnedStatics.Empty();
    SpawnedVehicles.Empty();

    // This method is used only at world/session teardown. Empty releases the allocator capacity
    // retained by large worlds instead of carrying it into menus and subsequent level loads.
    StaticReferences.Empty();
    VehicleReferences.Empty();
    WeaponReferences.Empty();
    AvailableItems.Empty();
    ToolbarSlots.Empty();
    bToolbarInitialized = false;

    // Keep CurrentWorldName across ordinary level travel. A confirmed menu return clears it only
    // after StopGameplaySession has finished saving the source gameplay world.
    ResetInitialPlayerTransformState();
    SelectedToolbarSlotIndex = 0;
    CurrentStaticIndex = 0;
    CurrentWeaponIndex = 0;
    CurrentMode = EToolMode::None;
    bSnapToGrid = false;
    bFirstPerson = false;
    bItemListWindowOpen = false;

    LastPreviewLocation = FVector::ZeroVector;
    LastTraceHit = FHitResult();
    bLastTraceBlockingHit = false;
    bLastTraceHasPlacementLocation = false;
    bLastTraceUsedFreeSpace = false;
    LastTraceStart = FVector::ZeroVector;
    LastTraceDirection = FVector::ForwardVector;
    LastSaveMessage.Empty();
    bSavedSceneLoaded = false;
    bSavedSceneLoadInProgress = false;
    bSavedSceneLoadFailed = false;
    SavedSceneReadinessAttemptCount = 0;
    SavedSceneDataAttemptCount = 0;
    bIsSavingScene = false;
    PendingWorldBakeModels.Empty();
    CompletedWorldBuildModels.Empty();
    ActiveWorldBuildTask = nullptr;
    bWorldBakeInProgress = false;
    bWorldArchiveCommitInFlight = false;
    bAutoBuildForStartup = false;
    bWorldStartupContinued = false;
    WorldBakeTotalModels = 0;
    WorldBakeCompletedModels = 0;
    WorldBakeFailedModels = 0;
    WorldBakeNextModelIndex = 0;

    CachedPlacementGridCenter = FVector::ZeroVector;
    CachedPlacementGridRadius = 0.0f;
    bPlacementGridBuilt = false;
}

FString UGameManagerSubSystem::GetWorldRootPath() const
{
    // Revalidate on use as a final defense against stale serialized state or future native callers
    // that bypass SetCurrentWorldName. Directory existence is checked during startup resolution.
    FString WorldName;
    if (!TryNormalizeWorldFolderName(CurrentWorldName, WorldName, false))
    {
        return FString();
    }
    return FPaths::Combine(PATH_WORLDS, WorldName);
}
void UGameManagerSubSystem::ScheduleSavedSceneLoadRetry(
    const FString& Reason,
    const bool bWaitingForWorldReadiness)
{
    bSavedSceneLoadInProgress = false;
    if (bSavedSceneLoaded)
    {
        return;
    }

    const int32 AttemptCount = bWaitingForWorldReadiness
        ? SavedSceneReadinessAttemptCount
        : SavedSceneDataAttemptCount;
    const int32 MaximumAttempts = bWaitingForWorldReadiness
        ? MaxSavedSceneReadinessAttempts
        : MaxSavedSceneDataAttempts;

    if (AttemptCount >= MaximumAttempts)
    {
        // Never pretend that restoration completed. In particular, this keeps SaveScene() from
        // replacing an unreadable/non-applied chunk with an empty generation during shutdown.
        bSavedSceneLoadFailed = true;
        UE_LOG(LogTemp, Error,
            TEXT("World-object .dat restore failed after %d attempts. World=%s Phase=%s Reason=%s"),
            AttemptCount,
            *GetWorldRootPath(),
            bWaitingForWorldReadiness ? TEXT("world-readiness") : TEXT("entity-validation/apply"),
            *Reason);
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        bSavedSceneLoadFailed = true;
        UE_LOG(LogTemp, Error,
            TEXT("World-object .dat load could not be retried because the world is unavailable: %s"),
            *Reason);
        return;
    }

    World->GetTimerManager().ClearTimer(SavedSceneLoadRetryTimerHandle);
    TWeakObjectPtr<UGameManagerSubSystem> WeakThis(this);
    World->GetTimerManager().SetTimer(
        SavedSceneLoadRetryTimerHandle,
        FTimerDelegate::CreateLambda([WeakThis]()
        {
            if (WeakThis.IsValid())
            {
                WeakThis->LoadSavedScene();
            }
        }),
        SavedSceneLoadRetryDelaySeconds,
        false);
    UE_LOG(LogTemp, Warning,
        TEXT("World-object .dat load will retry. Phase=%s Attempt=%d/%d Reason=%s"),
        bWaitingForWorldReadiness ? TEXT("world-readiness") : TEXT("entity-validation/apply"),
        AttemptCount,
        MaximumAttempts,
        *Reason);
}

void UGameManagerSubSystem::RefreshBuiltModelLists()
{
    StaticReferences.Empty();
    VehicleReferences.Empty();
    WeaponReferences.Empty();

    const UModelDatabaseSubsystem* Database = GetGameInstance()
        ? GetGameInstance()->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    if (!Database || !Database->IsReady())
    {
        UE_LOG(LogTemp, Error,
            TEXT("Built model list refresh skipped because the world model index is unavailable."));
        return;
    }

    TArray<FModelDefinition> Definitions;
    Database->GetDefinitions(Definitions);
    for (const FModelDefinition& Definition : Definitions)
    {
        if (Definition.ModelType == EModelDefinitionType::Static)
        {
            StaticReferences.Add(FGWorldArchive::MakeModelReference(Definition.UUID));
            continue;
        }
        if (Definition.ModelType != EModelDefinitionType::Dynamic)
        {
            continue;
        }
        if (Definition.EntityType == EModelEntityType::Vehicle)
        {
            VehicleReferences.Add(FGWorldArchive::MakeModelReference(Definition.UUID));
        }
        else if (Definition.ItemType == EModelItemType::Weapon)
        {
            WeaponReferences.Add(FGWorldArchive::MakeModelReference(Definition.UUID));
        }
    }

    StaticReferences.Sort();
    VehicleReferences.Sort();
    WeaponReferences.Sort();

    CurrentStaticIndex = StaticReferences.Num() > 0
        ? FMath::Clamp(CurrentStaticIndex, 0, StaticReferences.Num() - 1) : 0;
    CurrentWeaponIndex = WeaponReferences.Num() > 0
        ? FMath::Clamp(CurrentWeaponIndex, 0, WeaponReferences.Num() - 1) : 0;
}

FString UGameManagerSubSystem::GetAssetDisplayName(const FString& ModelReference) const
{
    const UModelDatabaseSubsystem* Database = GetGameInstance()
        ? GetGameInstance()->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    FGuid UUID;
    FModelDefinition Definition;
    FString RuntimeReference;
    if (Database && Database->FindUUIDForReference(ModelReference, UUID)
        && Database->Resolve(UUID, Definition, RuntimeReference))
    {
        return Definition.DisplayName.IsEmpty() ? Definition.Name : Definition.DisplayName;
    }

    // Invalid references are displayed as opaque identifiers. Runtime UI never probes resources/.
    return FPaths::GetBaseFilename(ModelReference);
}

FToolbarItem UGameManagerSubSystem::MakeToolbarItem(EToolbarItemKind Kind, const FString& DisplayName, const FString& ModelReference, int32 ModelIndex) const
{
    FToolbarItem Item;
    Item.Kind = Kind;
    Item.DisplayName = DisplayName;
    Item.ModelReference = ModelReference;
    Item.ModelIndex = ModelIndex;
    Item.bAvailable = Kind != EToolbarItemKind::None;
    return Item;
}

void UGameManagerSubSystem::BuildAvailableItems()
{
    AvailableItems.Empty();

    if (PlayMode == EPlayMode::Creator)
    {
        for (int32 Index = 0; Index < VehicleReferences.Num(); ++Index)
        {
            AvailableItems.Add(MakeToolbarItem(
                EToolbarItemKind::Vehicle,
                GetAssetDisplayName(VehicleReferences[Index]),
                VehicleReferences[Index],
                Index));
        }

        for (int32 Index = 0; Index < StaticReferences.Num(); ++Index)
        {
            AvailableItems.Add(MakeToolbarItem(
                EToolbarItemKind::Static,
                GetAssetDisplayName(StaticReferences[Index]),
                StaticReferences[Index],
                Index));
        }

        for (int32 Index = 0; Index < WeaponReferences.Num(); ++Index)
        {
            AvailableItems.Add(MakeToolbarItem(
                EToolbarItemKind::Weapon,
                GetAssetDisplayName(WeaponReferences[Index]),
                WeaponReferences[Index],
                Index));
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

        if (!Item.ModelReference.IsEmpty()
            && Candidate.ModelReference.Equals(Item.ModelReference, ESearchCase::IgnoreCase))
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
        LastSaveMessage = TEXT("The item index for the toolbar is invalid.");
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
    LastSaveMessage = bItemListWindowOpen ? TEXT("Item list opened") : TEXT("Item list closed");

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
    RefreshBuiltModelLists();
    BuildAvailableItems();
    InitializeToolbarSlotsIfNeeded();
    ApplySelectedToolbarItem(false);
    LastSaveMessage = PlayMode == EPlayMode::Creator ? TEXT("Creator Mode") : TEXT("Real Life Mode");
    NotifyToolbarChanged();
    NotifyStateChanged();
}

void UGameManagerSubSystem::ApplySelectedToolbarItem(bool bBroadcastChange)
{
    const FToolbarItem Item = GetSelectedToolbarItem();

    switch (Item.Kind)
    {
    case EToolbarItemKind::Static:
        if (Item.bAvailable && StaticReferences.IsValidIndex(Item.ModelIndex))
        {
            CurrentStaticIndex = Item.ModelIndex;
        }
        CurrentMode = EToolMode::PlaceStatic;
        LastSaveMessage = FString::Printf(TEXT("Static selected: %s"), *GetCurrentStaticName());
        break;
    case EToolbarItemKind::Vehicle:
        CurrentMode = EToolMode::PlaceVehicle;
        LastSaveMessage = TEXT("Vehicle tool: left-click to place a vehicle at the center crosshair.");
        break;
    case EToolbarItemKind::Weapon:
        if (Item.bAvailable && WeaponReferences.IsValidIndex(Item.ModelIndex))
        {
            CurrentWeaponIndex = Item.ModelIndex;
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
        LastSaveMessage = TEXT("The toolbar slot is empty.");
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
    TSoftObjectPtr<UWorld> MainWorld)
{
    TryOpenWorldSelectionScreen(WorldContextObject, MainWorld);
}

bool UGameManagerSubSystem::TryOpenWorldSelectionScreen(
    const UObject* WorldContextObject,
    TSoftObjectPtr<UWorld> MainWorld)
{
    UWorld* SourceWorld = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    if (!WorldContextObject || !SourceWorld || MainWorld.IsNull())
    {
        UE_LOG(LogTemp, Error,
            TEXT("[MenuTravel] Cannot open world selection because the context or directly referenced world is invalid."));
        return false;
    }

    if (MainWorld.Get() == SourceWorld)
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

    // MainGameMode clears the pending request only after the destination UI is input-safe.
    ResetEditorTransactionBufferForWorldTravel(WorldContextObject, TEXT("Open world-selection screen"));
    UE_LOG(LogTemp, Display, TEXT("[MenuTravel] Calling OpenLevelBySoftObjectPtr for world selection."));
    UGameplayStatics::OpenLevelBySoftObjectPtr(WorldContextObject, MainWorld, true, FString());
    return true;
}



AActor* UGameManagerSubSystem::GetCrosshairHitActor() const
{
    return bLastTraceBlockingHit ? LastTraceHit.GetActor() : nullptr;
}

FString UGameManagerSubSystem::GetCurrentStaticName() const
{
    return StaticReferences.IsValidIndex(CurrentStaticIndex)
        ? GetAssetDisplayName(StaticReferences[CurrentStaticIndex]) : TEXT("None");
}

FString UGameManagerSubSystem::GetCurrentWeaponName() const
{
    return WeaponReferences.IsValidIndex(CurrentWeaponIndex)
        ? GetAssetDisplayName(WeaponReferences[CurrentWeaponIndex]) : TEXT("None");
}

void UGameManagerSubSystem::SelectPreviousStatic()
{
    RefreshBuiltModelLists();
    BuildAvailableItems();
    if (StaticReferences.Num() > 0)
    {
        CurrentStaticIndex = (CurrentStaticIndex - 1 + StaticReferences.Num()) % StaticReferences.Num();
        LastSaveMessage = FString::Printf(TEXT("Static selected: %s"), *GetCurrentStaticName());
    }
    else
    {
        LastSaveMessage = TEXT("No built Static models are available.");
    }
    NotifyStateChanged();
}

void UGameManagerSubSystem::SelectNextStatic()
{
    RefreshBuiltModelLists();
    BuildAvailableItems();
    if (StaticReferences.Num() > 0)
    {
        CurrentStaticIndex = (CurrentStaticIndex + 1) % StaticReferences.Num();
        LastSaveMessage = FString::Printf(TEXT("Static selected: %s"), *GetCurrentStaticName());
    }
    else
    {
        LastSaveMessage = TEXT("No built Static models are available.");
    }
    NotifyStateChanged();
}

void UGameManagerSubSystem::SelectStaticPlacementTool()
{
    RefreshBuiltModelLists();
    BuildAvailableItems();
    CurrentMode = EToolMode::PlaceStatic;
    LastSaveMessage = TEXT("Static tool: left-click to place the current Static at the center crosshair.");
    NotifyStateChanged();
}

void UGameManagerSubSystem::SelectVehicleTool()
{
    RefreshBuiltModelLists();
    BuildAvailableItems();
    CurrentMode = EToolMode::PlaceVehicle;
    LastSaveMessage = TEXT("Vehicle tool: left-click to place a vehicle at the center crosshair.");
    NotifyStateChanged();
}

void UGameManagerSubSystem::SelectPreviousWeapon()
{
    RefreshBuiltModelLists();
    BuildAvailableItems();
    if (WeaponReferences.Num() > 0)
    {
        CurrentWeaponIndex = (CurrentWeaponIndex - 1 + WeaponReferences.Num()) % WeaponReferences.Num();
        LastSaveMessage = FString::Printf(TEXT("Weapon selected: %s"), *GetCurrentWeaponName());
    }
    else
    {
        LastSaveMessage = TEXT("No built Weapon models are available.");
    }
    NotifyStateChanged();
}

void UGameManagerSubSystem::SelectNextWeapon()
{
    RefreshBuiltModelLists();
    BuildAvailableItems();
    if (WeaponReferences.Num() > 0)
    {
        CurrentWeaponIndex = (CurrentWeaponIndex + 1) % WeaponReferences.Num();
        LastSaveMessage = FString::Printf(TEXT("Weapon selected: %s"), *GetCurrentWeaponName());
    }
    else
    {
        LastSaveMessage = TEXT("No built Weapon models are available.");
    }
    NotifyStateChanged();
}

void UGameManagerSubSystem::EquipCurrentWeapon()
{
    RefreshBuiltModelLists();

    UWorld* const World = GetWorld();
    APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
    if (!IsValid(PC))
    {
        LastSaveMessage = TEXT("PlayerController could not be found.");
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
        LastSaveMessage = TEXT("No character mesh or camera is available for weapon attachment.");
        NotifyStateChanged();
        return;
    }

    const bool bHasConfiguredWeaponReference = WeaponReferences.IsValidIndex(CurrentWeaponIndex);
    const FString SelectedWeaponReference = bHasConfiguredWeaponReference ? WeaponReferences[CurrentWeaponIndex] : FString();

    FActorSpawnParameters Params;
    Params.Owner = PC->GetPawn() ? Cast<AActor>(PC->GetPawn()) : SessionOwner.Get();
    Params.Instigator = PC->GetPawn();
    UClass* WeaponSpawnClass = WeaponActorClass ? WeaponActorClass.Get() : AWeaponActor::StaticClass();
    // Retain the validated world across this game-thread operation; the GameInstance subsystem can
    // otherwise be called from menu Blueprint code after its gameplay UWorld has been released.
    AWeaponActor* Weapon = World->SpawnActor<AWeaponActor>(
        WeaponSpawnClass, FTransform::Identity, Params);
    if (!IsValid(Weapon) && WeaponSpawnClass != AWeaponActor::StaticClass())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Configured weapon actor class failed to spawn; retrying native AWeaponActor. Class=%s"),
            *GetNameSafe(WeaponSpawnClass));
        Weapon = World->SpawnActor<AWeaponActor>(
            AWeaponActor::StaticClass(), FTransform::Identity, Params);
    }

    const bool bEquipped = IsValid(Weapon) && (bHasConfiguredWeaponReference
        ? Weapon->EquipFromModel(SelectedWeaponReference, AttachTarget)
        : Weapon->EquipDefault(AttachTarget));

    if (bEquipped)
    {
        EquippedWeapon = Weapon;
        CurrentMode = EToolMode::Weapon;
        LastSaveMessage = bHasConfiguredWeaponReference
            ? FString::Printf(TEXT("Weapon equipped: %s"), *GetCurrentWeaponName())
            : TEXT("Default test weapon equipped");
    }
    else if (IsValid(Weapon))
    {
        Weapon->Destroy();
        LastSaveMessage = TEXT("Weapon load failed");
    }
    NotifyStateChanged();
}

void UGameManagerSubSystem::ToggleSnap()
{
    bSnapToGrid = !bSnapToGrid;
    LastSaveMessage = bSnapToGrid ? TEXT("Grid Snap enabled") : TEXT("Grid Snap disabled");
    NotifyStateChanged();
}

void UGameManagerSubSystem::SetSnapEnabled(bool bEnabled)
{
    bSnapToGrid = bEnabled;
    LastSaveMessage = bSnapToGrid ? TEXT("Grid Snap enabled") : TEXT("Grid Snap disabled");
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
    LastSaveMessage = bFirstPerson ? TEXT("First-person mode enabled") : TEXT("First-person mode disabled");
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
        OutLocation = GetSessionOwnerLocation();
        // Reset the cached ray to a safe default when there is no player camera.
        LastTraceStart = GetSessionOwnerLocation();
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
    if (AActor* OwnerActor = SessionOwner.Get())
    {
        Params.AddIgnoredActor(OwnerActor);
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
    return PlayMode == EPlayMode::Creator
        && bLastTraceHasPlacementLocation
        && CurrentMode == EToolMode::PlaceStatic;
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
    if (Kind == EPlacedObjectKind::Static)
    {
        for (const TWeakObjectPtr<AStaticActor>& StaticReference : SpawnedStatics)
        {
            const AStaticActor* Static = StaticReference.Get();
            if (IsValid(Static) && Static->GetBaseName().Equals(BaseName, ESearchCase::IgnoreCase))
            {
                ++Count;
            }
        }
    }
    else if (Kind == EPlacedObjectKind::Vehicle)
    {
        for (const TWeakObjectPtr<AVehiclePawn>& VehicleReference : SpawnedVehicles)
        {
            const AVehiclePawn* Vehicle = VehicleReference.Get();
            if (IsValid(Vehicle))
            {
                ++Count;
            }
        }
    }
    return Count;
}

FString UGameManagerSubSystem::MakeObjectName(const FString& BaseName, EPlacedObjectKind Kind) const
{
    const FString SafeBaseName = BaseName.IsEmpty() ? TEXT("GeneratedEntity") : BaseName;
    const int32 ExistingCount = CountExistingBaseName(SafeBaseName, Kind);
    return ExistingCount <= 0
        ? SafeBaseName
        : FString::Printf(TEXT("%s.%03d"), *SafeBaseName, ExistingCount);
}

void UGameManagerSubSystem::InputPrimaryAction()
{
    // Backward-compatible Blueprint endpoint. Release was always a no-op after placement became
    // edge-triggered, so forwarding once preserves behavior without manufacturing duplicate work.
    InputPrimaryPressed();
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
    const bool bVehiclePlacement = CurrentMode == EToolMode::PlaceVehicle
        || Item.Kind == EToolbarItemKind::Vehicle;
    if (!bVehiclePlacement)
    {
        Location = ApplyGridSnap(Location);
    }

    switch (Item.Kind)
    {
    case EToolbarItemKind::Static:
        if (bHasPlacementLocation)
        {
            PlaceCurrentStatic(Location);
        }
        else
        {
            LastSaveMessage = TEXT("Could not resolve the center-crosshair location for Static placement.");
        }
        break;
    case EToolbarItemKind::Vehicle:
        if (bHasPlacementLocation)
        {
            PlaceVehicle(Location, Item.ModelReference);
        }
        else
        {
            LastSaveMessage = TEXT("Could not resolve the center-crosshair location for vehicle placement.");
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
        if (CurrentMode == EToolMode::PlaceStatic && bHasPlacementLocation)
        {
            PlaceCurrentStatic(Location);
        }
        else if (CurrentMode == EToolMode::PlaceVehicle && bHasPlacementLocation)
        {
            PlaceVehicle(Location, Item.ModelReference);
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
    // Intentionally empty. Retaining the reflected symbol prevents old Blueprint graphs from
    // failing to load while the current input model performs all placement work on button press.
}

void UGameManagerSubSystem::InputSecondaryAction()
{
    // Stable endpoint retained for existing input mappings.
    if (!bItemListWindowOpen)
    {
        NotifyStateChanged();
    }
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
    InputPrimaryPressed();
}

void UGameManagerSubSystem::ConfirmCurrentPendingLocation()
{
    InputPrimaryPressed();
}

void UGameManagerSubSystem::PlaceCurrentStatic(const FVector& Location)
{
    UWorld* const World = GetWorld();
    if (!IsValid(World) || !World->IsGameWorld())
    {
        LastSaveMessage = TEXT("Static placement was skipped because no active game world is available.");
        return;
    }

    RefreshBuiltModelLists();
    if (!StaticReferences.IsValidIndex(CurrentStaticIndex))
    {
        LastSaveMessage = TEXT("No built Static models are available.");
        return;
    }

    UModelDatabaseSubsystem* Database = GetGameInstance()
        ? GetGameInstance()->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    FGuid UUID;
    FModelDefinition Definition;
    FString ModelReference;
    if (!Database || !Database->FindUUIDForReference(StaticReferences[CurrentStaticIndex], UUID)
        || !Database->ResolveLoadable(UUID, Definition, ModelReference)
        || Definition.ModelType != EModelDefinitionType::Static)
    {
        LastSaveMessage = TEXT("The built Static model reference is invalid.");
        return;
    }

    const FString BaseName = Definition.Name;
    const FString ObjectName = MakeObjectName(BaseName, EPlacedObjectKind::Static);

    FActorSpawnParameters Params;
    Params.Owner = SessionOwner.Get();
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    APlayerController* const PlayerController = World->GetFirstPlayerController();
    const FRotator SpawnRot(0.0f,
        PlayerController ? PlayerController->GetControlRotation().Yaw : 0.0f,
        0.0f);
    UClass* StaticSpawnClass = StaticActorClass ? StaticActorClass.Get() : AStaticActor::StaticClass();
    AStaticActor* Actor = World->SpawnActor<AStaticActor>(
        StaticSpawnClass, FTransform(SpawnRot, Location), Params);
    if (!IsValid(Actor) && StaticSpawnClass != AStaticActor::StaticClass())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Configured Static actor class failed to spawn; retrying with native AStaticActor. Class=%s"),
            *GetNameSafe(StaticSpawnClass));
        Actor = World->SpawnActor<AStaticActor>(
            AStaticActor::StaticClass(), FTransform(SpawnRot, Location), Params);
    }
    if (IsValid(Actor))
    {
        Actor->SetRenderOnlyStreaming(UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this));
    }
    if (IsValid(Actor) && Actor->LoadStatic(ModelReference, ObjectName))
    {
        UWorldObjectStreamingSubsystem* Chunks =
            World->GetSubsystem<UWorldObjectStreamingSubsystem>();
        if (!Chunks || !Chunks->RegisterPlacedObject(Actor, UUID))
        {
            Actor->Destroy();
            LastSaveMessage = TEXT("Placement was deferred because the target chunk or model UUID is not ready yet.");
            return;
        }
        SpawnedStatics.Add(TWeakObjectPtr<AStaticActor>(Actor));
        LastSaveMessage = FString::Printf(TEXT("Placed: %s"), *ObjectName);
    }
    else if (IsValid(Actor))
    {
        Actor->Destroy();
        LastSaveMessage = TEXT("Static load failed");
    }
}

void UGameManagerSubSystem::PlaceVehicle(const FVector& Location, const FString& ModelReference)
{
    UWorld* const World = GetWorld();
    if (!IsValid(World) || !World->IsGameWorld())
    {
        LastSaveMessage = TEXT("Vehicle placement was skipped because no active game world is available.");
        return;
    }

    UModelDatabaseSubsystem* Database = GetGameInstance()
        ? GetGameInstance()->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    FGuid UUID;
    FModelDefinition Definition;
    FString RuntimeReference;
    if (!Database || !Database->FindUUIDForReference(ModelReference, UUID)
        || !Database->ResolveLoadable(UUID, Definition, RuntimeReference)
        || Definition.ModelType != EModelDefinitionType::Dynamic
        || Definition.EntityType != EModelEntityType::Vehicle)
    {
        LastSaveMessage = TEXT("The built vehicle model reference is invalid.");
        return;
    }

    FActorSpawnParameters Params;
    Params.Owner = SessionOwner.Get();
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
    APlayerController* const PlayerController = World->GetFirstPlayerController();
    const FRotator SpawnRot = FRotator(
        0.0f,
        PlayerController ? PlayerController->GetControlRotation().Yaw : 0.0f,
        0.0f);
    UClass* VehicleSpawnClass = VehiclePawnClass ? VehiclePawnClass.Get() : AVehiclePawn::StaticClass();
    const FVector SpawnLocation = Location + FVector(0.0f, 0.0f, 220.0f);
    AVehiclePawn* Vehicle = World->SpawnActor<AVehiclePawn>(
        VehicleSpawnClass,
        FTransform(SpawnRot, SpawnLocation),
        Params);
    if (!IsValid(Vehicle) && VehicleSpawnClass != AVehiclePawn::StaticClass())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Configured vehicle actor class failed to spawn; retrying with native AVehiclePawn. Class=%s"),
            *GetNameSafe(VehicleSpawnClass));
        Vehicle = World->SpawnActor<AVehiclePawn>(
            AVehiclePawn::StaticClass(),
            FTransform(SpawnRot, SpawnLocation),
            Params);
    }
    if (!IsValid(Vehicle))
    {
        LastSaveMessage = TEXT("Failed to spawn the vehicle actor.");
        return;
    }

    const FString VehicleBaseName = Definition.Name;
    const FString VehicleObjectName = MakeObjectName(VehicleBaseName, EPlacedObjectKind::Vehicle);
    if (!Vehicle->LoadVehicleModel(RuntimeReference, VehicleObjectName))
    {
        Vehicle->Destroy();
        LastSaveMessage = TEXT("The vehicle actor was removed because its model could not be loaded.");
        return;
    }

    SpawnedVehicles.Add(TWeakObjectPtr<AVehiclePawn>(Vehicle));
    UWorldObjectStreamingSubsystem* Chunks =
        World->GetSubsystem<UWorldObjectStreamingSubsystem>();
    if (!Chunks || !Chunks->RegisterPlacedObject(Vehicle, UUID))
    {
        SpawnedVehicles.Pop(EAllowShrinking::No);
        Vehicle->Destroy();
        LastSaveMessage = TEXT("Vehicle placement was deferred because the target chunk or model UUID is not ready yet.");
        return;
    }
    LastSaveMessage = TEXT("glTF vehicle placed. Press F to enter.");
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
                LastSaveMessage = TEXT("Vehicles cannot be entered while ragdolled.");
                NotifyStateChanged();
                return;
            }
        }
    }

    const FVector Origin = IsValid(CurrentPawn) ? CurrentPawn->GetActorLocation() : PC->PlayerCameraManager->GetCameraLocation();
    AVehiclePawn* BestVehicle = nullptr;
    float BestDistSq = FMath::Square(VehicleEnterDistance);

    CompactTrackedEntityReferences();
    for (const TWeakObjectPtr<AVehiclePawn>& VehicleReference : SpawnedVehicles)
    {
        AVehiclePawn* Vehicle = VehicleReference.Get();
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
        LastSaveMessage = TEXT("The vehicle cannot be entered in the current state.");
        NotifyStateChanged();
    }
}
bool UGameManagerSubSystem::SaveScene()
{
    check(IsInGameThread());
    if (const UWorld* World = GetWorld(); World && World->GetNetMode() == NM_Client) return false;
    UWorldObjectStreamingSubsystem* Chunks = GetWorld() ? GetWorld()->GetSubsystem<UWorldObjectStreamingSubsystem>() : nullptr;
    if (!Chunks || !Chunks->IsRunning())
    {
        LastSaveMessage = TEXT("Save request skipped because chunk streaming is not ready.");
        return false;
    }
    // Mark tracked actors for the next coalesced .dat checkpoint.
    for (const TWeakObjectPtr<AStaticActor>& Object : SpawnedStatics)
        if (Object.IsValid()) Chunks->MarkObjectChanged(Object.Get());
    for (const TWeakObjectPtr<AVehiclePawn>& Object : SpawnedVehicles)
        if (Object.IsValid()) Chunks->MarkObjectChanged(Object.Get());
    LastSaveMessage = TEXT("The modified object was queued for the next .dat chunk checkpoint.");
    NotifyStateChanged();
    return true;
}

FString UGameManagerSubSystem::GetProjectsRootPath()
{
    return FSafeFileIO::NormalizeFilePath(PATH_PROJECTS);
}

FString UGameManagerSubSystem::GetExternalResourcesRootPath()
{
    const FString ResourcesRoot = FSafeFileIO::NormalizeFilePath(PATH_RESOURCES);
    if (!ResourcesRoot.IsEmpty())
    {
        IFileManager::Get().MakeDirectory(*ResourcesRoot, true);
    }
    return ResourcesRoot;
}

bool UGameManagerSubSystem::GetProjectConfigurationByName(
    const FString& ProjectName,
    EGlTFSimulatorProjectType& OutProjectType,
    bool& bOutAllowExternalAssets,
    FString& OutDisplayName) const
{
    OutProjectType = EGlTFSimulatorProjectType::World;
    bOutAllowExternalAssets = false;
    OutDisplayName.Reset();
    FString SafeName;
    if (!TryNormalizeWorldFolderName(ProjectName, SafeName, false)) return false;
    FGlTFSimulatorProjectConfig Config;
    FString Error;
    if (!GlTFSimulatorProjectConfig::Load(
            FPaths::Combine(PATH_PROJECTS, SafeName, LEVEL_FILE_NAME),
            SafeName, Config, nullptr, Error))
    {
        return false;
    }
    OutProjectType = Config.ProjectType;
    bOutAllowExternalAssets = Config.bAllowExternalAssets;
    OutDisplayName = Config.GetDisplayName(SafeName);
    return true;
}

bool UGameManagerSubSystem::SetProjectTypeByName(
    const FString& ProjectName,
    const EGlTFSimulatorProjectType ProjectType)
{
    check(IsInGameThread());
    if (IsProjectBuildInProgress()) return false;
    FString SafeName;
    if (!TryNormalizeWorldFolderName(ProjectName, SafeName, false)) return false;
    const FString ConfigPath = FPaths::Combine(PATH_PROJECTS, SafeName, LEVEL_FILE_NAME);
    const FSafeJsonLoadResult Loaded = FSafeFileIO::LoadJsonBlocking(ConfigPath);
    TSharedPtr<FJsonObject> Json = Loaded.JsonObject;
    if (!Loaded.IsSuccess() || !Json.IsValid())
    {
        LastSaveMessage = FString::Printf(
            TEXT("Project configuration could not be updated: %s"),
            Loaded.Error.IsEmpty() ? TEXT("invalid config.json") : *Loaded.Error);
        NotifyStateChanged();
        return false;
    }
    Json->SetStringField(PROJECT_TYPE_FIELD, GlTFSimulatorProjectConfig::ToString(ProjectType));
    if (ProjectType == EGlTFSimulatorProjectType::World)
    {
        FString WorldName;
        if (!Json->TryGetStringField(CONFIG_WORLD_NAME_FIELD, WorldName)
            || WorldName.TrimStartAndEnd().IsEmpty())
        {
            Json->SetStringField(CONFIG_WORLD_NAME_FIELD, SafeName);
        }
    }
    else
    {
        Json->SetBoolField(ALLOW_EXTERNAL_ASSETS_FIELD, false);
        FString ProjectDisplayName;
        if (!Json->TryGetStringField(PROJECT_NAME_FIELD, ProjectDisplayName)
            || ProjectDisplayName.TrimStartAndEnd().IsEmpty())
        {
            Json->SetStringField(PROJECT_NAME_FIELD, SafeName);
        }
    }
    const FSafeFileWriteResult Saved = FSafeFileIO::SaveJsonBlocking(Json.ToSharedRef(), ConfigPath);
    LastSaveMessage = Saved.IsSuccess()
        ? FString::Printf(TEXT("Project type changed to %s."), *GlTFSimulatorProjectConfig::ToString(ProjectType))
        : FString::Printf(TEXT("Project configuration save failed: %s"), *Saved.Error);
    NotifyStateChanged();
    return Saved.IsSuccess();
}

bool UGameManagerSubSystem::SetWorldExternalAssetsAllowedByName(
    const FString& ProjectName,
    const bool bAllowed)
{
    check(IsInGameThread());
    if (IsProjectBuildInProgress()) return false;
    FString SafeName;
    if (!TryNormalizeWorldFolderName(ProjectName, SafeName, false)) return false;
    const FString ConfigPath = FPaths::Combine(PATH_PROJECTS, SafeName, LEVEL_FILE_NAME);
    FGlTFSimulatorProjectConfig Existing;
    TSharedPtr<FJsonObject> Json;
    FString Error;
    if (!GlTFSimulatorProjectConfig::Load(ConfigPath, SafeName, Existing, &Json, Error)
        || !Json.IsValid() || Existing.ProjectType != EGlTFSimulatorProjectType::World)
    {
        LastSaveMessage = Existing.ProjectType == EGlTFSimulatorProjectType::World
            ? FString::Printf(TEXT("Project configuration could not be updated: %s"), *Error)
            : TEXT("Only World projects can allow external asset packs.");
        NotifyStateChanged();
        return false;
    }
    Json->SetBoolField(ALLOW_EXTERNAL_ASSETS_FIELD, bAllowed);
    const FSafeFileWriteResult Saved = FSafeFileIO::SaveJsonBlocking(Json.ToSharedRef(), ConfigPath);
    LastSaveMessage = Saved.IsSuccess()
        ? FString::Printf(TEXT("External assets are now %s for this world."), bAllowed ? TEXT("enabled") : TEXT("disabled"))
        : FString::Printf(TEXT("Project configuration save failed: %s"), *Saved.Error);
    NotifyStateChanged();
    return Saved.IsSuccess();
}

bool UGameManagerSubSystem::BuildProjectByName(const FString& ProjectName)
{
    check(IsInGameThread());
    FString SafeName;
    if (!TryNormalizeWorldFolderName(ProjectName, SafeName, false)
        || bWorldBakeInProgress || !ActiveBuildProjectRoot.IsEmpty())
    {
        LastSaveMessage = (bWorldBakeInProgress || !ActiveBuildProjectRoot.IsEmpty())
            ? TEXT("Another project build is already in progress.")
            : TEXT("The project name is invalid.");
        NotifyStateChanged();
        return false;
    }

    const FString ProjectRoot = FSafeFileIO::NormalizeFilePath(FPaths::Combine(PATH_PROJECTS, SafeName));
    const FString ResourcesRoot = FPaths::Combine(ProjectRoot, TEXT("resources"));
    const FString ConfigPath = FPaths::Combine(ProjectRoot, LEVEL_FILE_NAME);
    if (!IFileManager::Get().DirectoryExists(*ProjectRoot)
        || !IFileManager::Get().DirectoryExists(*ResourcesRoot)
        || !IFileManager::Get().FileExists(*ConfigPath))
    {
        LastSaveMessage = TEXT("The project requires both config.json and a resources directory.");
        NotifyStateChanged();
        return false;
    }

    if (!GetGameInstance() || !GetGameInstance()->GetSubsystem<UModelDatabaseSubsystem>())
    {
        LastSaveMessage = TEXT("The project model database subsystem is unavailable.");
        NotifyStateChanged();
        return false;
    }

    ActiveBuildProjectRoot = ProjectRoot;
    ActiveBuildProjectName = SafeName;
    ActiveBuildProjectType = EGlTFSimulatorProjectType::World;
    bActiveBuildAllowExternalAssets = false;
    WorldBakeProgressValue = 0.01f;
    LastSaveMessage = FString::Printf(TEXT("Validating project configuration: %s"), *SafeName);
    OnWorldBakeProgress.Broadcast(WorldBakeProgressValue);
    NotifyStateChanged();

    TWeakObjectPtr<UGameManagerSubSystem> WeakThis(this);
    const bool bQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, ProjectRoot, SafeName, ConfigPath]()
    {
        FString ConfigJson;
        FString ValidationError;
        FGlTFSimulatorProjectConfig ProjectConfig;
        const FSafeBinaryLoadResult ConfigBytes =
            FSafeFileIO::LoadBinaryBlocking(ConfigPath, 64ll * 1024ll * 1024ll);
        if (!ConfigBytes.IsSuccess())
        {
            ValidationError = FString::Printf(
                TEXT("Failed to read project config.json: %s"), *ConfigBytes.Error);
        }
        else
        {
            const FSafeJsonLoadResult ParsedConfig =
                FSafeFileIO::ParseJsonUtf8Bytes(ConfigBytes.Data, ConfigPath);
            if (!ParsedConfig.IsSuccess() || !ParsedConfig.JsonObject.IsValid()
                || !GlTFSimulatorProjectConfig::Parse(
                    ParsedConfig.JsonObject, SafeName, ProjectConfig, ValidationError))
            {
                if (ValidationError.IsEmpty())
                {
                    ValidationError = ParsedConfig.Error.IsEmpty()
                        ? TEXT("Project config.json is invalid.")
                        : ParsedConfig.Error;
                }
            }
            else
            {
                FUTF8ToTCHAR Utf8(
                    reinterpret_cast<const ANSICHAR*>(ConfigBytes.Data.GetData()),
                    ConfigBytes.Data.Num());
                ConfigJson = FString(Utf8.Length(), Utf8.Get());
            }
        }

        FSafeFileIO::DispatchTrackedGameThread(
            [WeakThis, ProjectRoot, SafeName, ProjectConfig,
                ConfigJson = MoveTemp(ConfigJson), ValidationError = MoveTemp(ValidationError)]() mutable
        {
            UGameManagerSubSystem* StrongThis = WeakThis.Get();
            if (!IsValid(StrongThis) || StrongThis->ActiveBuildProjectRoot != ProjectRoot
                || StrongThis->ActiveBuildProjectName != SafeName)
            {
                return;
            }
            if (!ValidationError.IsEmpty())
            {
                StrongThis->PendingWorldConfigJson.Reset();
                StrongThis->ActiveBuildProjectRoot.Reset();
                StrongThis->ActiveBuildProjectName.Reset();
                StrongThis->LastSaveMessage = ValidationError;
                StrongThis->OnWorldBakeCompleted.Broadcast(false, StrongThis->LastSaveMessage);
                StrongThis->NotifyStateChanged();
                return;
            }

            UModelDatabaseSubsystem* Database = StrongThis->GetGameInstance()
                ? StrongThis->GetGameInstance()->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
            if (!Database)
            {
                StrongThis->PendingWorldConfigJson.Reset();
                StrongThis->ActiveBuildProjectRoot.Reset();
                StrongThis->ActiveBuildProjectName.Reset();
                StrongThis->LastSaveMessage = TEXT("The project model database subsystem is unavailable.");
                StrongThis->OnWorldBakeCompleted.Broadcast(false, StrongThis->LastSaveMessage);
                StrongThis->NotifyStateChanged();
                return;
            }

            StrongThis->ActiveBuildProjectType = ProjectConfig.ProjectType;
            StrongThis->bActiveBuildAllowExternalAssets = ProjectConfig.bAllowExternalAssets;
            StrongThis->PendingWorldConfigJson = MoveTemp(ConfigJson);
            StrongThis->WorldBakeProgressValue = FMath::Max(StrongThis->WorldBakeProgressValue, 0.03f);
            StrongThis->OnWorldBakeProgress.Broadcast(StrongThis->WorldBakeProgressValue);
            FglTFRuntimeSafety::ResetRecoverableFailures();
            StrongThis->LastSaveMessage = FString::Printf(
                TEXT("Validating %s project models: %s"),
                *GlTFSimulatorProjectConfig::ToString(ProjectConfig.ProjectType), *SafeName);
            StrongThis->NotifyStateChanged();
            Database->InitializeForAuthoringProject(
                ProjectRoot, FModelDatabaseReady::CreateLambda(
                    [WeakThis, ProjectRoot, SafeName](const bool bReady, const FString& Error)
            {
                UGameManagerSubSystem* InnerThis = WeakThis.Get();
                if (!IsValid(InnerThis) || InnerThis->ActiveBuildProjectRoot != ProjectRoot
                    || InnerThis->ActiveBuildProjectName != SafeName)
                {
                    return;
                }
                if (!bReady)
                {
                    InnerThis->PendingWorldConfigJson.Reset();
                    InnerThis->ActiveBuildProjectRoot.Reset();
                    InnerThis->ActiveBuildProjectName.Reset();
                    InnerThis->LastSaveMessage = FString::Printf(TEXT("Project validation failed: %s"), *Error);
                    InnerThis->OnWorldBakeCompleted.Broadcast(false, InnerThis->LastSaveMessage);
                    InnerThis->NotifyStateChanged();
                    return;
                }
                InnerThis->WorldBakeProgressValue = FMath::Max(InnerThis->WorldBakeProgressValue, 0.05f);
                InnerThis->OnWorldBakeProgress.Broadcast(InnerThis->WorldBakeProgressValue);
                InnerThis->BakeWorldData();
            }));
        });
    });

    if (!bQueued)
    {
        ActiveBuildProjectRoot.Reset();
        ActiveBuildProjectName.Reset();
        PendingWorldConfigJson.Reset();
        LastSaveMessage = TEXT("The project build worker queue is shutting down.");
        NotifyStateChanged();
        return false;
    }
    return true;
}

void UGameManagerSubSystem::BakeWorldData()
{
    UWorld* World = GetWorld();
    if (!World || World->GetNetMode() == NM_Client)
    {
        const FString Message = TEXT("Project build can run only in an authoritative server or single-player world.");
        LastSaveMessage = Message;
        OnWorldBakeCompleted.Broadcast(false, Message);
        if (bAutoBuildForStartup)
        {
            FailWorldStartup(Message);
        }
        else
        {
            PendingWorldConfigJson.Reset();
            ActiveBuildProjectRoot.Reset();
            ActiveBuildProjectName.Reset();
            ActiveBuildProjectType = EGlTFSimulatorProjectType::World;
            bActiveBuildAllowExternalAssets = false;
            NotifyStateChanged();
        }
        return;
    }

    if (bWorldBakeInProgress)
    {
        const FString Message = TEXT("A project archive build is already in progress.");
        LastSaveMessage = Message;
        if (bAutoBuildForStartup)
        {
            FailWorldStartup(Message);
        }
        else
        {
            NotifyStateChanged();
        }
        return;
    }

    UModelDatabaseSubsystem* ModelDatabase = GetGameInstance()
        ? GetGameInstance()->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    if (!ModelDatabase || !ModelDatabase->IsReady())
    {
        const FString Message = TEXT("The project archive cannot be built because the source model index is not ready.");
        LastSaveMessage = Message;
        OnWorldBakeCompleted.Broadcast(false, Message);
        if (bAutoBuildForStartup)
        {
            FailWorldStartup(Message);
        }
        else
        {
            PendingWorldConfigJson.Reset();
            ActiveBuildProjectRoot.Reset();
            ActiveBuildProjectName.Reset();
            ActiveBuildProjectType = EGlTFSimulatorProjectType::World;
            bActiveBuildAllowExternalAssets = false;
            NotifyStateChanged();
        }
        return;
    }
    if (ModelDatabase->IsBuiltWorld())
    {
        const FString Message = TEXT("Source rebuild was refused because a verified runtime archive is already open.");
        LastSaveMessage = Message;
        OnWorldBakeCompleted.Broadcast(false, Message);
        if (bAutoBuildForStartup)
        {
            FailWorldStartup(Message);
        }
        else
        {
            PendingWorldConfigJson.Reset();
            ActiveBuildProjectRoot.Reset();
            ActiveBuildProjectName.Reset();
            ActiveBuildProjectType = EGlTFSimulatorProjectType::World;
            bActiveBuildAllowExternalAssets = false;
            NotifyStateChanged();
        }
        return;
    }

    bWorldBakeInProgress = true;
    ++WorldBakeGeneration;
    WorldBakeCancellation = MakeShared<TAtomic<bool>, ESPMode::ThreadSafe>(false);
    bWorldArchiveCommitInFlight = false;
    WorldBakeProgressValue = FMath::Max(WorldBakeProgressValue, 0.05f);
    WorldBakeTotalModels = 0;
    WorldBakeCompletedModels = 0;
    WorldBakeFailedModels = 0;
    WorldBakeNextModelIndex = 0;
    PendingWorldBakeModels.Empty();
    CompletedWorldBuildModels.Empty();
    ActiveWorldBuildTask = nullptr;
    OnWorldBakeProgress.Broadcast(WorldBakeProgressValue);

    TArray<FModelDefinition> Definitions;
    ModelDatabase->GetDefinitions(Definitions);
    PendingWorldBakeModels.Reserve(Definitions.Num());
    FString TypeMismatchError;
    for (const FModelDefinition& Definition : Definitions)
    {
        const FString ModelPath = GlbValidation::NormalizePath(Definition.GlbPath);
        if (ModelPath.IsEmpty()) continue;

        bool bAllowedForProject = ActiveBuildProjectType == EGlTFSimulatorProjectType::World;
        if (ActiveBuildProjectType == EGlTFSimulatorProjectType::Character)
        {
            bAllowedForProject = Definition.ModelType == EModelDefinitionType::Character;
        }
        else if (ActiveBuildProjectType == EGlTFSimulatorProjectType::Dynamic)
        {
            bAllowedForProject = Definition.ModelType == EModelDefinitionType::Dynamic;
        }

        if (!bAllowedForProject)
        {
            TypeMismatchError = FString::Printf(
                TEXT("%s projects may contain only %s models, but '%s' is %s."),
                *GlTFSimulatorProjectConfig::ToString(ActiveBuildProjectType),
                *GlTFSimulatorProjectConfig::ToString(ActiveBuildProjectType),
                *Definition.DisplayName,
                *ModelDefinitionJson::ModelTypeToString(Definition.ModelType));
            break;
        }
        PendingWorldBakeModels.Add(Definition);
    }

    if (!TypeMismatchError.IsEmpty())
    {
        CompleteWorldArchiveBuild(false, FString(), TypeMismatchError);
        return;
    }

    WorldBakeTotalModels = PendingWorldBakeModels.Num();

    // Reserve five percent for setup and the last five percent for archive verification/publish.
    WorldBakeProgressValue = FMath::Max(WorldBakeProgressValue, 0.05f);
    OnWorldBakeProgress.Broadcast(WorldBakeProgressValue);

    LastSaveMessage = FString::Printf(
        TEXT("%s project build started: %d model(s)"),
        *GlTFSimulatorProjectConfig::ToString(ActiveBuildProjectType),
        WorldBakeTotalModels);
    NotifyStateChanged();

    if (WorldBakeTotalModels > 0)
    {
        World->GetTimerManager().SetTimer(
            WorldBakeProgressTimerHandle,
            this,
            &UGameManagerSubSystem::RefreshWorldBakeProgress,
            0.05f,
            true);
    }
    else
    {
        // Never publish an empty archive. A zero count means the resources root was wrong or source
        // discovery failed, and treating that as a build would suppress every later GLB scan.
        CompleteWorldArchiveBuild(false, FString(),
            TEXT("recursive resources scan produced zero buildable models for this project type"));
        return;
    }
    StartNextWorldBakeModel();
}

void UGameManagerSubSystem::StartNextWorldBakeModel()
{
    if (!bWorldBakeInProgress)
    {
        return;
    }

    if (!GetWorld())
    {
        CompleteWorldArchiveBuild(false, FString(),
            TEXT("the gameplay world was destroyed during source decoding"));
        return;
    }

    while (WorldBakeNextModelIndex < PendingWorldBakeModels.Num())
    {
        const FModelDefinition Definition = PendingWorldBakeModels[WorldBakeNextModelIndex++];
        const FString ModelPath = GlbValidation::NormalizePath(Definition.GlbPath);
        if (ModelPath.IsEmpty() || !IFileManager::Get().FileExists(*ModelPath))
        {
            ++WorldBakeCompletedModels;
            ++WorldBakeFailedModels;
            RefreshWorldBakeProgress();
            continue;
        }

        UModelDatabaseSubsystem* Database = GetGameInstance()
            ? GetGameInstance()->GetSubsystem<UModelDatabaseSubsystem>()
            : nullptr;
        FString DefinitionJson;
        FString DefinitionError;
        if (!Database
            || !Database->GetDefinitionJson(
                Definition.UUID, DefinitionJson, &DefinitionError))
        {
            UE_LOG(LogTemp, Error,
                TEXT("World source JSON unavailable. GLB=%s Reason=%s"),
                *ModelPath, *DefinitionError);
            ++WorldBakeCompletedModels;
            ++WorldBakeFailedModels;
            RefreshWorldBakeProgress();
            continue;
        }

        UWorldSourceModelBuilder* BuildTask =
            NewObject<UWorldSourceModelBuilder>(this, NAME_None, RF_Transient);
        if (!IsValid(BuildTask))
        {
            ++WorldBakeCompletedModels;
            ++WorldBakeFailedModels;
            RefreshWorldBakeProgress();
            continue;
        }

        ActiveWorldBuildTask = BuildTask;
        LastSaveMessage = FString::Printf(
            TEXT("Building model %d/%d: %s"),
            WorldBakeCompletedModels + 1,
            WorldBakeTotalModels,
            *FPaths::GetCleanFilename(ModelPath));
        NotifyStateChanged();
        UE_LOG(LogTemp, Display,
            TEXT("Building source GLB %d/%d: %s"),
            WorldBakeCompletedModels + 1, WorldBakeTotalModels, *ModelPath);
        BuildTask->OnFinished.AddUObject(
            this,
            &UGameManagerSubSystem::HandleWorldBuildModelFinished);
        BuildTask->Start(Definition, DefinitionJson);
        return;
    }

    FinishWorldBake();
}

void UGameManagerSubSystem::RefreshWorldBakeProgress()
{
    if (!bWorldBakeInProgress)
    {
        return;
    }

    const float ActiveModelProgress = IsValid(ActiveWorldBuildTask)
        ? FMath::Clamp(ActiveWorldBuildTask->GetProgress(), 0.0f, 1.0f)
        : 0.0f;
    const float ModelProgress = WorldBakeTotalModels > 0
        ? FMath::Clamp(
            (static_cast<float>(WorldBakeCompletedModels) + ActiveModelProgress) /
                static_cast<float>(WorldBakeTotalModels),
            0.0f,
            1.0f)
        : 1.0f;

    // The final five percent belongs to directory serialization, fsync, re-open and checksum verification.
    const float CalculatedProgress = FMath::Min(0.05f + ModelProgress * 0.90f, 0.95f);
    const float SafeProgress = FMath::Max(WorldBakeProgressValue, CalculatedProgress);
    if (!FMath::IsNearlyEqual(SafeProgress, WorldBakeProgressValue, KINDA_SMALL_NUMBER))
    {
        WorldBakeProgressValue = SafeProgress;
        // Automatic source builds are part of startup. Keep the always-available loading overlay
        // moving even before config.json and scene streaming have begun.
        if (bAutoBuildForStartup)
        {
            SetLoadingStatus(FMath::Clamp(WorldBakeProgressValue * 0.65f, 0.0f, 0.65f));
        }
        OnWorldBakeProgress.Broadcast(WorldBakeProgressValue);
    }
}

void UGameManagerSubSystem::HandleWorldBuildModelFinished(
    UWorldSourceModelBuilder* BuildTask,
    bool bSuccess)
{
    if (!bWorldBakeInProgress
        || !IsValid(BuildTask)
        || BuildTask != ActiveWorldBuildTask.Get())
    {
        return;
    }

    BuildTask->OnFinished.RemoveAll(this);
    if (bSuccess)
    {
        FGWorldBuildModel Built = BuildTask->TakeBuildModel();
        FString ValidationError;
        if (!Built.Definition.UUID.IsValid()
            || Built.DefinitionJson.IsEmpty()
            || !Built.Metadata.IsSane(&ValidationError)
            || !Built.BakedData.IsSane(&ValidationError)
            || Built.Metadata.SourceMeshCount != Built.BakedData.Meshes.Num()
            || Built.Metadata.SourceMaterialCount != Built.BakedData.Materials.Num()
            || Built.Metadata.SourceTextureCount != Built.BakedData.Textures.Num()
            || Built.SourceFileSize <= 0)
        {
            bSuccess = false;
            UE_LOG(LogTemp, Error,
                TEXT("World source build result rejected. Reason=%s"),
                ValidationError.IsEmpty()
                    ? TEXT("incomplete build payload") : *ValidationError);
        }
        else
        {
            CompletedWorldBuildModels.Add(MoveTemp(Built));
        }
    }
    else
    {
        UE_LOG(LogTemp, Error,
            TEXT("World source model failed. GLB=%s Reason=%s"),
            *BuildTask->GetSourcePath(), *BuildTask->GetError());
    }
    ActiveWorldBuildTask = nullptr;

    ++WorldBakeCompletedModels;
    if (!bSuccess)
    {
        ++WorldBakeFailedModels;
    }

    RefreshWorldBakeProgress();
    // Leave the completion stack before allocating the next parser-backed task. This also gives
    // the released source asset one frame boundary before the next large model is decoded.
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().SetTimerForNextTick(
            this, &UGameManagerSubSystem::StartNextWorldBakeModel);
    }
    else
    {
        StartNextWorldBakeModel();
    }
}

void UGameManagerSubSystem::FinishWorldBake()
{
    if (!bWorldBakeInProgress || bWorldArchiveCommitInFlight)
    {
        return;
    }

    if (WorldBakeFailedModels > 0
        || CompletedWorldBuildModels.Num() != WorldBakeTotalModels)
    {
        CompleteWorldArchiveBuild(false, FString(),
            FString::Printf(TEXT("model build failed for %d/%d models"),
                WorldBakeFailedModels, WorldBakeTotalModels));
        return;
    }

    const FString BuildProjectName = !ActiveBuildProjectName.IsEmpty()
        ? ActiveBuildProjectName : CurrentWorldName;
    if (BuildProjectName.IsEmpty())
    {
        CompleteWorldArchiveBuild(false, FString(), TEXT("project build name is empty"));
        return;
    }

    const EGlTFSimulatorProjectType BuildProjectType = ActiveBuildProjectType;
    const bool bWorldProject = BuildProjectType == EGlTFSimulatorProjectType::World;
    FString OutputRootOrPath;
    if (bWorldProject)
    {
        OutputRootOrPath = FSafeFileIO::NormalizeFilePath(FPaths::Combine(PATH_WORLDS, BuildProjectName));
    }
    else
    {
        const FString ExternalRoot = GetExternalResourcesRootPath();
        if (ExternalRoot.IsEmpty())
        {
            CompleteWorldArchiveBuild(false, FString(), TEXT("external Resources directory is unavailable"));
            return;
        }
        OutputRootOrPath = FSafeFileIO::NormalizeFilePath(
            FPaths::Combine(ExternalRoot, BuildProjectName + TEXT(".gasset")));
    }

    bWorldArchiveCommitInFlight = true;
    WorldBakeProgressValue = 0.96f;
    LastSaveMessage = bWorldProject
        ? TEXT("Writing and verifying the world .gwd archive...")
        : TEXT("Writing and verifying the external .gasset archive...");
    OnWorldBakeProgress.Broadcast(WorldBakeProgressValue);
    NotifyStateChanged();
    const FString ArchiveConfigJson = PendingWorldConfigJson;
    TArray<FGWorldBuildModel> BuildModels = MoveTemp(CompletedWorldBuildModels);
    TWeakObjectPtr<UGameManagerSubSystem> WeakThis(this);
    const uint64 BuildGeneration = WorldBakeGeneration;
    const TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> Cancellation = WorldBakeCancellation;
    const bool bQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, OutputRootOrPath, ArchiveConfigJson, BuildGeneration, Cancellation,
            bWorldProject, BuildModels = MoveTemp(BuildModels)]() mutable
    {
        FString ArchivePath;
        FString Error;
        const TFunction<bool()> ShouldCancel = [Cancellation]()
        {
            return Cancellation.IsValid() && Cancellation->Load();
        };
        const bool bSuccess = bWorldProject
            ? FGWorldArchive::BuildBlocking(
                OutputRootOrPath, BuildModels, ArchivePath, Error, ShouldCancel, ArchiveConfigJson)
            : FGWorldArchive::BuildBlockingToArchivePath(
                OutputRootOrPath, BuildModels, ArchivePath, Error, ShouldCancel, ArchiveConfigJson);
        FSafeFileIO::DispatchTrackedGameThread(
            [WeakThis, BuildGeneration, bSuccess,
                ArchivePath = MoveTemp(ArchivePath), Error = MoveTemp(Error)]()
        {
            if (UGameManagerSubSystem* StrongThis = WeakThis.Get())
            {
                if (StrongThis->WorldBakeGeneration == BuildGeneration)
                {
                    StrongThis->CompleteWorldArchiveBuild(bSuccess, ArchivePath, Error);
                }
            }
        });
    });
    if (!bQueued)
    {
        bWorldArchiveCommitInFlight = false;
        CompleteWorldArchiveBuild(false, FString(),
            TEXT("project archive worker queue is shutting down"));
    }
}

void UGameManagerSubSystem::CompleteWorldArchiveBuild(
    const bool bArchiveSuccess,
    const FString& ArchivePath,
    const FString& Error)
{
    check(IsInGameThread());
    if (!bWorldBakeInProgress) return;
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(WorldBakeProgressTimerHandle);
    }

    const bool bSuccess = bArchiveSuccess;
    const bool bWasAutomaticStartupBuild = bAutoBuildForStartup;
    const EGlTFSimulatorProjectType CompletedProjectType = ActiveBuildProjectType;
    const bool bResumeStartup = bWasAutomaticStartupBuild && bArchiveSuccess
        && CompletedProjectType == EGlTFSimulatorProjectType::World;
    const TCHAR* ArchiveKind = CompletedProjectType == EGlTFSimulatorProjectType::World
        ? TEXT(".gwd") : TEXT(".gasset");
    const FString Message = bSuccess
        ? FString::Printf(TEXT("%s build completed: %d model(s) (%s)"),
            ArchiveKind, WorldBakeTotalModels, *ArchivePath)
        : FString::Printf(TEXT("%s build failed: %s"),
            ArchiveKind, Error.IsEmpty() ? TEXT("archive build failed") : *Error);

    bWorldBakeInProgress = false;
    bWorldArchiveCommitInFlight = false;
    WorldBakeCancellation.Reset();
    bAutoBuildForStartup = false;
    PendingWorldBakeModels.Empty();
    CompletedWorldBuildModels.Empty();
    ActiveWorldBuildTask = nullptr;
    PendingWorldConfigJson.Reset();
    ActiveBuildProjectRoot.Reset();
    ActiveBuildProjectName.Reset();
    ActiveBuildProjectType = EGlTFSimulatorProjectType::World;
    bActiveBuildAllowExternalAssets = false;
    WorldBakeNextModelIndex = 0;
    WorldBakeProgressValue = 1.0f;
    LastSaveMessage = Message;
    OnWorldBakeProgress.Broadcast(1.0f);
    OnWorldBakeCompleted.Broadcast(bSuccess, Message);
    NotifyStateChanged();

    if (bWasAutomaticStartupBuild && !bSuccess)
    {
        HideLoadingWidget();
        SetWorldLoading(false);
    }

    if (bResumeStartup)
    {
        UModelDatabaseSubsystem* Database = GetGameInstance()
            ? GetGameInstance()->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
        if (!Database)
        {
            FailWorldStartup(
                TEXT("World startup failed: no model database is available to reopen the archive after the build."));
            return;
        }
        SetLoadingStatus(0.67f);
        const FString WorldRoot = GetWorldRootPath();
        TWeakObjectPtr<UGameManagerSubSystem> WeakThis(this);
        Database->InitializeForWorld(WorldRoot, FModelDatabaseReady::CreateLambda(
            [WeakThis](const bool bOpened, const FString& OpenError)
        {
            UGameManagerSubSystem* StrongThis = WeakThis.Get();
            if (!IsValid(StrongThis) || !StrongThis->bManagerStarted) return;
            UModelDatabaseSubsystem* OpenDatabase = StrongThis->GetGameInstance()
                ? StrongThis->GetGameInstance()->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
            if (!bOpened || !OpenDatabase || !OpenDatabase->IsBuiltWorld())
            {
                StrongThis->FailWorldStartup(FString::Printf(
                    TEXT("Built archive revalidation failed: %s"), *OpenError));
                return;
            }
            StrongThis->ContinueWorldStartupAfterDatabase();
        }));
    }
}

void UGameManagerSubSystem::CancelWorldBake()
{
    if (WorldBakeCancellation.IsValid())
    {
        WorldBakeCancellation->Store(true);
    }
    ++WorldBakeGeneration;
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(WorldBakeProgressTimerHandle);
    }

    if (IsValid(ActiveWorldBuildTask))
    {
        ActiveWorldBuildTask->OnFinished.RemoveAll(this);
        ActiveWorldBuildTask->Cancel();
    }

    ActiveWorldBuildTask = nullptr;
    PendingWorldBakeModels.Empty();
    CompletedWorldBuildModels.Empty();
    PendingWorldConfigJson.Reset();
    ActiveBuildProjectRoot.Reset();
    ActiveBuildProjectName.Reset();
    ActiveBuildProjectType = EGlTFSimulatorProjectType::World;
    bActiveBuildAllowExternalAssets = false;
    bWorldBakeInProgress = false;
    bWorldArchiveCommitInFlight = false;
    WorldBakeCancellation.Reset();
    bAutoBuildForStartup = false;
    WorldBakeTotalModels = 0;
    WorldBakeCompletedModels = 0;
    WorldBakeFailedModels = 0;
    WorldBakeNextModelIndex = 0;
    WorldBakeProgressValue = 0.0f;
}

bool UGameManagerSubSystem::LoadSavedScene()
{
    check(IsInGameThread());
    UWorldObjectStreamingSubsystem* Chunks = GetWorld() ? GetWorld()->GetSubsystem<UWorldObjectStreamingSubsystem>() : nullptr;
    if (!Chunks || !Chunks->IsRunning())
    {
        ++SavedSceneReadinessAttemptCount;
        ScheduleSavedSceneLoadRetry(TEXT("world-object chunk streamer is not ready"), true);
        return false;
    }
    if (!Chunks->IsInitialAreaReady()) return false;
    // Missing model UUIDs remain in their original .dat chunks and are intentionally ignored.
    bSavedSceneLoadInProgress = false;
    bSavedSceneLoaded = true;
    bSavedSceneLoadFailed = false;
    SavedSceneReadinessAttemptCount = 0;
    SavedSceneDataAttemptCount = 0;
    LastSaveMessage = TEXT("Initial-radius world object chunks finished loading.");
    NotifyStateChanged();
    return true;
}

void UGameManagerSubSystem::TrackStreamedWorldObject(AActor* Actor)
{
    check(IsInGameThread());

    if (AStaticActor* Static = Cast<AStaticActor>(Actor))
    {
        SpawnedStatics.AddUnique(TWeakObjectPtr<AStaticActor>(Static));
    }
    else if (AVehiclePawn* Vehicle = Cast<AVehiclePawn>(Actor))
    {
        SpawnedVehicles.AddUnique(TWeakObjectPtr<AVehiclePawn>(Vehicle));
    }
}

void UGameManagerSubSystem::RefreshAssetLists()
{
    RefreshBuiltModelLists();
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
    case EToolMode::PlaceStatic:
        SelectStaticPlacementTool();
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
        LastSaveMessage = TEXT("Tool cleared.");
        ClearPlacementGridMesh();
        NotifyStateChanged();
        break;
    }
}

bool UGameManagerSubSystem::SetCurrentStaticIndex(int32 NewIndex)
{
    if (StaticReferences.Num() == 0)
    {
        RefreshBuiltModelLists();
        BuildAvailableItems();
    }
    if (!StaticReferences.IsValidIndex(NewIndex))
    {
        LastSaveMessage = TEXT("Invalid Static index.");
        NotifyStateChanged();
        return false;
    }

    CurrentStaticIndex = NewIndex;
    LastSaveMessage = FString::Printf(TEXT("Static selected: %s"), *GetCurrentStaticName());
    NotifyStateChanged();
    return true;
}

bool UGameManagerSubSystem::SetCurrentWeaponIndex(int32 NewIndex)
{
    if (WeaponReferences.Num() == 0)
    {
        RefreshBuiltModelLists();
        BuildAvailableItems();
    }
    if (!WeaponReferences.IsValidIndex(NewIndex))
    {
        LastSaveMessage = TEXT("Invalid weapon index.");
        NotifyStateChanged();
        return false;
    }

    CurrentWeaponIndex = NewIndex;
    LastSaveMessage = FString::Printf(TEXT("Weapon selected: %s"), *GetCurrentWeaponName());
    NotifyStateChanged();
    return true;
}

FString UGameManagerSubSystem::GetStaticNameAtIndex(int32 Index) const
{
    return StaticReferences.IsValidIndex(Index) ? GetAssetDisplayName(StaticReferences[Index]) : FString();
}

FString UGameManagerSubSystem::GetWeaponNameAtIndex(int32 Index) const
{
    return WeaponReferences.IsValidIndex(Index) ? GetAssetDisplayName(WeaponReferences[Index]) : FString();
}

FString UGameManagerSubSystem::GetStaticPathAtIndex(const int32 Index) const
{
    // Preserve old Blueprint pins without restoring source-file identity at runtime.
    return GetStaticReferenceAtIndex(Index);
}

FString UGameManagerSubSystem::GetWeaponPathAtIndex(const int32 Index) const
{
    // Preserve old Blueprint pins without restoring source-file identity at runtime.
    return GetWeaponReferenceAtIndex(Index);
}

FString UGameManagerSubSystem::GetStaticReferenceAtIndex(int32 Index) const
{
    return StaticReferences.IsValidIndex(Index) ? StaticReferences[Index] : FString();
}

FString UGameManagerSubSystem::GetWeaponReferenceAtIndex(int32 Index) const
{
    return WeaponReferences.IsValidIndex(Index) ? WeaponReferences[Index] : FString();
}

FString UGameManagerSubSystem::BuildStatusText() const
{
    const FString ModeString = StaticEnum<EToolMode>()->GetDisplayNameTextByValue(
        static_cast<int64>(CurrentMode)).ToString();
    const FToolbarItem SelectedItem = GetSelectedToolbarItem();
    const FString SelectedItemName = SelectedItem.DisplayName.IsEmpty()
        ? TEXT("Empty")
        : SelectedItem.DisplayName;
    const FString CrosshairPlacementText = !bLastTraceHasPlacementLocation
        ? TEXT("NONE")
        : (bLastTraceBlockingHit ? TEXT("SURFACE") : TEXT("AIR / FREE-SPACE"));

    return FString::Printf(
        TEXT("[Creator Toolbar]\nMode: %s | PlayMode: %s\nToolbar Slot: %d / 7 | Item: %s (%s)\nInventory Window: %s | Available Items: %d\nSnap: %s / Grid %.0f cm\nCrosshair: X %.0f Y %.0f Z %.0f | Placement: %s | Collision %.0f cm / Max %.0f cm\nControls: MouseWheel=toolbar slot, E=item list, LMB=place/fire, F=enter/exit vehicle, SnapAction/G=toggle snap\nWorld: %s\nEntity: %s\n%s"),
        *ModeString,
        PlayMode == EPlayMode::Creator ? TEXT("Creator") : TEXT("RealLife"),
        SelectedToolbarSlotIndex + 1,
        *SelectedItemName,
        *StaticEnum<EToolbarItemKind>()->GetDisplayNameTextByValue(static_cast<int64>(SelectedItem.Kind)).ToString(),
        bItemListWindowOpen ? TEXT("OPEN") : TEXT("CLOSED"),
        AvailableItems.Num(),
        bSnapToGrid ? TEXT("ON") : TEXT("OFF"),
        GridSize,
        LastPreviewLocation.X,
        LastPreviewLocation.Y,
        LastPreviewLocation.Z,
        *CrosshairPlacementText,
        CrosshairCollisionTraceDistance,
        FreeSpacePlacementDistance,
        *GetWorldRootPath(),
        *FEntityArchiveStore::MakeArchivePath(GetWorldRootPath()),
        *LastSaveMessage);
}

FString UGameManagerSubSystem::BuildHUDText() const
{
    return BuildStatusText();
}
