// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "System/GameManagerSubSystem.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Editor/TransBuffer.h"
#endif
#include "System/GameManagerActor.h"
#include "System/BinaryDataStore.h"
#include "System/ActorHelper.h"
#include "System/SafeFileIO.h"
#include "World/PrefabActor.h"
#include "Vehicle/VehiclePawn.h"
#include "Weapon/WeaponActor.h"
#include "Model/glTFStreamActor.h"
#include "Model/MaterialDefaultAsset.h"
#include "World/WorldEnvManager.h"
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
#include "GameFramework/GameUserSettings.h"
#include "GameFramework/WorldSettings.h"
#include "System/SystemInfoFunctionLibrary.h"
#include "Components/PostProcessComponent.h"
#include "Model/glTFStreamSubSystem.h"
#include "Model/InstancedEntitySubsystem.h"
#include "Character/CharacterController.h"
#include "Character/CharacterComponent.h"
#include "Character/PlayerCharacterController.h"
#include "System/FileFunctionLibrary.h"
#include "System/GlbValidation.h"
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
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "RenderingThread.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/GarbageCollection.h"
#include "Engine/GameInstance.h"

static constexpr int32 ToolbarSlotCount = 7;
#define MODEL_DIRECTORY TEXT("/model/")
#define PLAYER_DIRECTORY TEXT("/player/")

namespace
{
    constexpr int32 MaxSavedSceneReadinessAttempts = 120; // 30 seconds at 0.25 s intervals.
    constexpr int32 MaxSavedSceneDataAttempts = 20; // Allow transient model-registration failures to settle for up to five seconds.
    constexpr float SavedSceneLoadRetryDelaySeconds = 0.25f;

    FString NormalizeFullPathForWorldData(const FString& Path)
    {
        FString Normalized = Path.TrimStartAndEnd();
        if (Normalized.IsEmpty())
        {
            return FString();
        }

        Normalized = FPaths::ConvertRelativePathToFull(Normalized);
        FPaths::NormalizeFilename(Normalized);
        if (!FPaths::CollapseRelativeDirectories(Normalized))
        {
            return FString();
        }
        return Normalized;
    }

    bool IsPathInsideDirectory(const FString& CandidatePath, const FString& DirectoryPath)
    {
        const FString Candidate = NormalizeFullPathForWorldData(CandidatePath);
        FString Directory = NormalizeFullPathForWorldData(DirectoryPath);
        if (Candidate.IsEmpty() || Directory.IsEmpty())
        {
            return false;
        }

        while (Directory.EndsWith(TEXT("/")))
        {
            Directory.LeftChopInline(1, EAllowShrinking::No);
        }
        const FString Prefix = Directory + TEXT("/");
        return Candidate.Equals(Directory, ESearchCase::IgnoreCase) ||
            Candidate.StartsWith(Prefix, ESearchCase::IgnoreCase);
    }

    bool ArePlacedObjectRecordsEquivalent(
        const TArray<FPlacedObjectRecord>& A,
        const TArray<FPlacedObjectRecord>& B)
    {
        if (A.Num() != B.Num())
        {
            return false;
        }

        for (int32 Index = 0; Index < A.Num(); ++Index)
        {
            const FPlacedObjectRecord& Left = A[Index];
            const FPlacedObjectRecord& Right = B[Index];
            if (Left.Kind != Right.Kind
                || !Left.ObjectName.Equals(Right.ObjectName, ESearchCase::CaseSensitive)
                || !Left.BaseName.Equals(Right.BaseName, ESearchCase::CaseSensitive)
                || !Left.SourceFile.Equals(Right.SourceFile, ESearchCase::CaseSensitive)
                || !Left.Transform.Equals(Right.Transform, 0.001f))
            {
                return false;
            }
        }
        return true;
    }

    enum class EAuthoredAssetType : uint8
    {
        Auto,
        Entity,
        Vehicle,
        Weapon,
        Prefab
    };

    EAuthoredAssetType ReadAuthoredAssetType(const FString& ModelPath, bool& bOutExplicit)
    {
        bOutExplicit = false;
        const FString JsonPath = FPaths::ChangeExtension(ModelPath, TEXT("json"));
        if (!IFileManager::Get().FileExists(*JsonPath))
        {
            return EAuthoredAssetType::Auto;
        }

        FSafeJsonLimits Limits;
        Limits.MaxFileBytes = 64ll * 1024ll * 1024ll;
        Limits.MaxDepth = 32;
        Limits.MaxValues = 100000;
        Limits.MaxContainerEntries = 100000;
        Limits.MaxStringCharacters = 32768;
        Limits.MaxPrimitiveCharacters = 1024;
        Limits.bAllowBackupRecovery = false;
        const FSafeJsonLoadResult LoadResult = FSafeFileIO::LoadJsonBlocking(JsonPath, Limits);
        if (!LoadResult.IsSuccess() || !LoadResult.JsonObject.IsValid())
        {
            UE_LOG(LogTemp, Warning,
                TEXT("AssetType was ignored because the sibling JSON is invalid. Path=%s Reason=%s"),
                *JsonPath,
                *LoadResult.Error);
            return EAuthoredAssetType::Auto;
        }

        FString Value;
        if (!LoadResult.JsonObject->TryGetStringField(TEXT("AssetType"), Value))
        {
            return EAuthoredAssetType::Auto;
        }

        bOutExplicit = true;
        Value.TrimStartAndEndInline();
        FString Canonical = Value;
        Canonical.ReplaceInline(TEXT(" "), TEXT(""));
        Canonical.ReplaceInline(TEXT("_"), TEXT(""));
        Canonical.ReplaceInline(TEXT("-"), TEXT(""));
        if (Canonical.Equals(TEXT("Entity"), ESearchCase::IgnoreCase))
        {
            return EAuthoredAssetType::Entity;
        }
        if (Canonical.Equals(TEXT("Prefab"), ESearchCase::IgnoreCase))
        {
            return EAuthoredAssetType::Prefab;
        }
        if (Canonical.Equals(TEXT("Vehicle"), ESearchCase::IgnoreCase) ||
            Canonical.Equals(TEXT("Car"), ESearchCase::IgnoreCase))
        {
            return EAuthoredAssetType::Vehicle;
        }
        if (Canonical.Equals(TEXT("Weapon"), ESearchCase::IgnoreCase) ||
            Canonical.Equals(TEXT("Item"), ESearchCase::IgnoreCase))
        {
            return EAuthoredAssetType::Weapon;
        }
        if (Canonical.Equals(TEXT("Auto"), ESearchCase::IgnoreCase))
        {
            return EAuthoredAssetType::Auto;
        }

        UE_LOG(LogTemp, Warning,
            TEXT("Unknown AssetType '%s'. Falling back to folder/wheel-tag classification. Path=%s"),
            *Value,
            *JsonPath);
        bOutExplicit = false;
        return EAuthoredAssetType::Auto;
    }

    /**
     * Resolves the external world JSON play-mode key without letting a previous world's value leak
     * through the GameInstance subsystem. Empty/Default/SinglePlayer means use the map's directly
     * assigned GameManagerActor default.
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

    /**
     * All three fields are required. In particular, a zero vector is accepted while a missing,
     * non-numeric, non-finite, or out-of-world component is rejected.
     */
    bool TryReadSavedLocation(
        const TSharedPtr<FJsonObject>& Json,
        const FString& Prefix,
        FVector& OutLocation)
    {
        if (!Json.IsValid())
        {
            return false;
        }

        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        if (!Json->TryGetNumberField(Prefix + TEXT("X"), X)
            || !Json->TryGetNumberField(Prefix + TEXT("Y"), Y)
            || !Json->TryGetNumberField(Prefix + TEXT("Z"), Z)
            || !IsFiniteWorldCoordinate(X)
            || !IsFiniteWorldCoordinate(Y)
            || !IsFiniteWorldCoordinate(Z))
        {
            return false;
        }

        OutLocation = FVector(X, Y, Z);
        return true;
    }

    bool TryReadSavedRotation(
        const TSharedPtr<FJsonObject>& Json,
        const FString& Prefix,
        FRotator& OutRotation)
    {
        if (!Json.IsValid())
        {
            return false;
        }

        double Pitch = 0.0;
        double Yaw = 0.0;
        double Roll = 0.0;
        if (!Json->TryGetNumberField(Prefix + TEXT("Pitch"), Pitch)
            || !Json->TryGetNumberField(Prefix + TEXT("Yaw"), Yaw)
            || !Json->TryGetNumberField(Prefix + TEXT("Roll"), Roll)
            || !FMath::IsFinite(Pitch)
            || !FMath::IsFinite(Yaw)
            || !FMath::IsFinite(Roll))
        {
            return false;
        }

        OutRotation = FRotator(Pitch, Yaw, Roll).GetNormalized();
        return IsFiniteRotation(OutRotation);
    }

    TSharedPtr<FJsonObject> FindSerializedPlayerRecord(
        const TSharedPtr<FJsonObject>& RootJson,
        const FString& PlayerId)
    {
        if (!RootJson.IsValid())
        {
            return nullptr;
        }

        const TArray<TSharedPtr<FJsonValue>>* PlayerValues = nullptr;
        if (!RootJson->TryGetArrayField(TEXT("Players"), PlayerValues) || !PlayerValues)
        {
            return nullptr;
        }

        for (const TSharedPtr<FJsonValue>& Value : *PlayerValues)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object)
            {
                continue;
            }

            const TSharedPtr<FJsonObject> PlayerJson = Value->AsObject();
            FString SerializedPlayerId;
            if (PlayerJson.IsValid()
                && PlayerJson->TryGetStringField(TEXT("PlayerId"), SerializedPlayerId)
                && SerializedPlayerId.Equals(PlayerId, ESearchCase::IgnoreCase))
            {
                return PlayerJson;
            }
        }

        return nullptr;
    }
}


UGameManagerSubSystem::UGameManagerSubSystem()
{
    bIsGamePaused = false;
    bIsWorldLoading = false;
    bIsGamePaused = false;
    LoadingStatus = 0.0f;
    TotalSumFPS = 0;
    TotalCountFPS = 0;
    PrefabActorClass = APrefabActor::StaticClass();
    VehiclePawnClass = AVehiclePawn::StaticClass();
    WeaponActorClass = AWeaponActor::StaticClass();
    WorldEnvManagerClass = AWorldEnvManager::StaticClass();
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

    ReleaseMaterialDefaultAsset();
    RegisteredWorldSelectionWorld.Reset();
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
    if (!ensureMsgf(IsInGameThread(), TEXT("MaterialDefaultAsset must be resolved on the game thread")))
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
    // Installing fresh objects makes world replacement atomic from the request's perspective.
    MaterialDefaultRuntimeCache = nullptr;
    MaterialDefaultAssetInstance = nullptr;

    UMaterialDefaultAsset* ConfigInstance = nullptr;
    FString ConfigSource;

    if (UClass* ConfigClass = MaterialDefaultAssetClass.Get())
    {
        const bool bUsableClass =
            ConfigClass->IsChildOf(UMaterialDefaultAsset::StaticClass()) &&
            !ConfigClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
        if (!bUsableClass)
        {
            bMaterialDefaultAssetResolved = true;
            UE_LOG(LogTemp, Error,
                TEXT("MaterialDefaultAssetClass is not instantiable. Runtime glTF actors will use plugin defaults. Class=%s"),
                *GetPathNameSafe(ConfigClass));
            return false;
        }

        // StartGameManager is entered from AGameManagerActor::BeginPlay. Creating the object here
        // keeps UObject allocation and all subsequent soft-material loads on the game thread.
        ConfigInstance = NewObject<UMaterialDefaultAsset>(
            this,
            ConfigClass,
            NAME_None,
            RF_Transient);
        if (!IsValid(ConfigInstance))
        {
            bMaterialDefaultAssetResolved = true;
            UE_LOG(LogTemp, Error,
                TEXT("MaterialDefaultAssetClass instance could not be created. Runtime glTF actors will use plugin defaults. Class=%s"),
                *GetPathNameSafe(ConfigClass));
            return false;
        }

        // Root the instance before resolving. ResolveMaterials may synchronously load packages, and
        // a nested game-thread call must never observe an unreferenced configuration object.
        MaterialDefaultAssetInstance = ConfigInstance;
        ConfigSource = ConfigClass->GetPathName();
    }
    else
    {
        bMaterialDefaultAssetResolved = true;
        UE_LOG(LogTemp, Display,
            TEXT("No MaterialDefaultAssetClass is configured; glTFRuntime material defaults will be used."));
        return true;
    }

    UMaterialDefaultRuntimeCache* NewCache = NewObject<UMaterialDefaultRuntimeCache>(this);
    if (!IsValid(NewCache))
    {
        MaterialDefaultAssetInstance = nullptr;
        bMaterialDefaultAssetResolved = true;
        UE_LOG(LogTemp, Error, TEXT("MaterialDefaultAsset runtime reference guard could not be allocated."));
        return false;
    }

    // Assign before resolving so any re-entrant game-thread lookup sees a GC-rooted object.
    MaterialDefaultRuntimeCache = NewCache;

    TArray<FString> Failures;
    const bool bResolveCallSucceeded = ConfigInstance->ResolveMaterials(
        NewCache->References,
        Failures);
    bMaterialDefaultAssetResolved = true;

    for (const FString& Failure : Failures)
    {
        UE_LOG(LogTemp, Warning, TEXT("MaterialDefaultAsset reference skipped: %s"), *Failure);
    }

    UE_LOG(LogTemp, Display,
        TEXT("MaterialDefaultAsset runtime instance resolved for the active world. Configured=%d Named=%d Failures=%d Source=%s Instance=%s"),
        ConfigInstance->Materials.NumConfiguredReferences(),
        NewCache->References.ByMaterialName.Num(),
        Failures.Num(),
        *ConfigSource,
        *GetNameSafe(ConfigInstance));

    return bResolveCallSucceeded;
}

void UGameManagerSubSystem::ReleaseMaterialDefaultAsset()
{
    // Drop only the subsystem's pointer. Never Reset() the cache object itself: an async action may
    // still be holding the same guard until a native glTFRuntime callback reaches its terminal path.
    MaterialDefaultRuntimeCache = nullptr;
    MaterialDefaultAssetInstance = nullptr;
    MaterialDefaultAssetClass = nullptr;
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
        // while the manager checks player.json; a valid saved transform may still supersede it.
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
    VehiclePawnClass = InConfigActor->VehiclePawnClass;
    WeaponActorClass = InConfigActor->WeaponActorClass;

    // Use the editor-authored world environment class, with the native class as a safe fallback.
    if (InConfigActor->WorldEnvManagerClass)
    {
        WorldEnvManagerClass = InConfigActor->WorldEnvManagerClass;
    }
    else
    {
        WorldEnvManagerClass = AWorldEnvManager::StaticClass();
    }

    // Player/input code may call StartGameManager repeatedly. Rebuild this immutable runtime
    // configuration only when its effective editor-authored source changes.
    const UClass* RequestedMaterialClass = InConfigActor->MaterialDefaultAssetClass.Get();
    const bool bMaterialConfigurationChanged =
        MaterialDefaultAssetClass.Get() != RequestedMaterialClass;
    if (!bMaterialDefaultAssetResolved || bMaterialConfigurationChanged)
    {
        ReleaseMaterialDefaultAsset();
        MaterialDefaultAssetClass = InConfigActor->MaterialDefaultAssetClass;
        ResolveMaterialDefaultAsset();
    }

    SpawnActorClass = InConfigActor->SpawnActorClass;
    WaterClass = InConfigActor->WaterClass;
    RainWeatherActorClass = InConfigActor->RainWeatherActorClass;
    OceanTransform = InConfigActor->OceanTransform;
    LoadingWidgetClass = InConfigActor->LoadingWidgetClass;
    PlacementTraceDistance = InConfigActor->PlacementTraceDistance;
    CrosshairCollisionTraceDistance = InConfigActor->CrosshairCollisionTraceDistance;
    FreeSpacePlacementDistance = InConfigActor->FreeSpacePlacementDistance;
    bAllowFreeSpacePlacement = InConfigActor->bAllowFreeSpacePlacement;
    GridSize = InConfigActor->GridSize;
    SurfacePlacementOffset = InConfigActor->SurfacePlacementOffset;
    VehicleEnterDistance = InConfigActor->VehicleEnterDistance;
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
    AGameManagerActor* OwnerActor = ConfigActor.Get();
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
    if (!IsValid(InConfigActor))
    {
        return;
    }

    // GameInstance subsystems survive level travel. If the previous actor weak pointer expired
    // before its EndPlay callback reset the flags, bManagerStarted can still describe the old world.
    // Force-release that stale session before accepting the destination world's manager actor.
    if (bManagerStarted && ConfigActor.Get() != InConfigActor)
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
        else if (IsValid(Multiplayer))
        {
            // Single-player OpenLevel historically omitted the World URL option. Always prefer the
            // current world-selection value over a name retained by this persistent subsystem from
            // an earlier map; otherwise entities.dat can be read from the previous world's folder.
            FString SelectedWorldName = Multiplayer->GetSelectedWorldFolderName();
            SelectedWorldName.TrimStartAndEndInline();
            if (!SelectedWorldName.IsEmpty())
            {
                CurrentWorldName = MoveTemp(SelectedWorldName);
            }
        }

        CurrentWorldName.TrimStartAndEndInline();
        if (!CurrentWorldName.IsEmpty() && IsValid(Multiplayer))
        {
            Multiplayer->SetSelectedWorldFolderName(CurrentWorldName);
        }
        UE_LOG(LogTemp, Display,
            TEXT("Resolved runtime world data root. SelectedWorld=%s Root=%s EntityDat=%s"),
            CurrentWorldName.IsEmpty() ? TEXT("<empty>") : *CurrentWorldName,
            *GetWorldRootPath(),
            *GetManifestPath());

        if (World->GetNetMode() != NM_Standalone && World->GetNetMode() != NM_Client)
        {
            AMultiplayerWorldStateActor::SpawnOrUpdateForWorld(this, CurrentWorldName);
        }
    }

    // GameMode is chosen while the destination UWorld is initialized. Validate it before any custom
    // gameplay systems spawn actors, so a wrong map/profile assignment is immediately visible.
    ValidateResolvedGameMode();
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

void UGameManagerSubSystem::StopGameManager(
    const EEndPlayReason::Type EndPlayReason,
    const AGameManagerActor* RequestingActor)
{
    // An old map actor can finish EndPlay after the destination map has already started its own
    // manager. Never let that stale callback tear down the destination world's streaming session.
    if (RequestingActor && ConfigActor.IsValid() && ConfigActor.Get() != RequestingActor)
    {
        UE_LOG(LogTemp, Display,
            TEXT("[Gameplay] Ignored stale GameManagerActor EndPlay. Requester=%s Active=%s"),
            *GetNameSafe(RequestingActor),
            *GetNameSafe(ConfigActor.Get()));
        return;
    }

    CancelWorldBake();
    CompactTrackedEntityReferences();

    const bool bHadActiveMainWorld = bManagerStarted
        || bWorldBootstrapStarted
        || IsValid(StreamSubSystem)
        || IsValid(WorldEnvManagerActor)
        || IsValid(OceanActor)
        || IsValid(LoadingWidgetInstance)
        || SpawnedPrefabs.Num() > 0
        || SpawnedVehicles.Num() > 0
        || LastKnownSceneRecords.Num() > 0
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
        // Fallback for editor shutdown or travel paths that did not run the pre-travel commit.
        // CollectSceneRecords retains the last validated snapshot if entity EndPlay ran first.
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
    // prefab or vehicle can be destroyed before the manager receives its own EndPlay callback,
    // which previously allowed an empty entity snapshot to replace a populated entities.dat.
    WorldNameBeforeMenuTravel = CurrentWorldName;
    const bool bEntitySaveCompleted = SaveScene();
    SaveWorldData();
    SavePlayerData();

    bMenuTravelStatePrepared = true;
    // A failed/blocked save intentionally leaves the last committed DAT untouched. Do not retry
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
    // Backward-compatible Blueprint/C++ entry point. It is deliberately non-destructive so legacy
    // Exit-button listeners cannot freeze the UI before OpenLevel has even been requested.
    PrepareForMenuLevelTravelRequest();
}

void UGameManagerSubSystem::PrepareForReturnToMainWorld()
{
    // MainWorld/menu entry point. The cleanup path is shared by menu and world-selection level travel.
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
    // gameplay-world actors, streamed glTF assets, and async-load state reachable.
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
    ConfigActor = nullptr;
    ClearTransientRuntimeReferences();
    ResetWorldRuntimeReferences();

    bManagerStarted = false;
    bWorldBootstrapStarted = false;
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

    for (const TWeakObjectPtr<APrefabActor>& PrefabReference : SpawnedPrefabs)
    {
        DestroyActorIfValid(PrefabReference.Get());
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
    SpawnedPrefabs.Empty();
    SpawnedVehicles.Empty();
}

void UGameManagerSubSystem::CompactTrackedEntityReferences()
{
    const int32 RemovedPrefabs = SpawnedPrefabs.RemoveAllSwap(
        [](const TWeakObjectPtr<APrefabActor>& Reference)
        {
            const APrefabActor* Actor = Reference.Get();
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
    if (RemovedPrefabs > 0 && SpawnedPrefabs.Max() > FMath::Max(32, SpawnedPrefabs.Num() * 2))
    {
        SpawnedPrefabs.Shrink();
    }
    if (RemovedVehicles > 0 && SpawnedVehicles.Max() > FMath::Max(16, SpawnedVehicles.Num() * 2))
    {
        SpawnedVehicles.Shrink();
    }
}

void UGameManagerSubSystem::ResetWorldRuntimeReferences()
{
    ConfigActor.Reset();
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

    UWorld* CurrentWorld = GetWorld();
    const bool bNewWorldEnvManagerIsActive = bManagerStarted && ConfigActor.IsValid() &&
        ConfigActor->GetWorld() == CurrentWorld;
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
    UglTFStreamSubSystem* GlobalStreamSubSystem = nullptr;
    if (UGameInstance* GameInstance = GetGameInstance())
    {
        GlobalStreamSubSystem = GameInstance->GetSubsystem<UglTFStreamSubSystem>();
    }
    if (GlobalStreamSubSystem && !GlobalStreamSubSystem->IsActiveForWorld(CurrentWorld))
    {
        GlobalStreamSubSystem->StopMainWorldStreaming();
    }

    HideLoadingWidget();
    ClearPlacementGridMesh();
    ReleaseMaterialDefaultAsset();
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
    // 100% before the reserved entities.dat restoration node and caused Blueprint loading bars to
    // display completion while saved actors were still pending.
    return bWorldLoadCompleted ? 1.0f : FMath::Clamp(LoadingStatus, 0.0f, 0.99f);
}

void UGameManagerSubSystem::InitializeWorldBootstrap()
{
    if (bWorldBootstrapStarted)
    {
        return;
    }
    // The subsystem survives OpenLevel, while the initial player transform is scoped to one world.
    // Reset its source/applied markers before reading either legacy or per-player save data.
    ResetInitialPlayerTransformState();
    bWorldBootstrapStarted = true;
    bWorldLoadCompleted = false;

    SetWorldLoading(true);
    SetLoadingStatus(0.0f);

    // Loading UI, world data, world rendering, water, and GLB streaming now start from one owner.
    ShowLoadingWidget();
    LoadWorldData();
    LoadPlayerData();
    ApplyLevelSettings();
    SpawnWorldEnvManager();

    if (IsValid(WorldEnvManagerActor))
    {
        WorldEnvManagerActor->InitializeRendering(ActiveWorldData);
    }

    InitializeWorldSystems(
        ActiveWorldData,
        GetWorldFilePath(MODEL_DIRECTORY),
        GetWorldFilePath(PLAYER_DIRECTORY),
        ActivePlayerId);

    LoadWorldAsync();
}

void UGameManagerSubSystem::SpawnWorldEnvManager()
{
    UWorld* World = GetWorld();
    if (IsValid(WorldEnvManagerActor) || !World)
    {
        return;
    }

    UClass* EffectiveWorldEnvManagerClass = WorldEnvManagerClass ? WorldEnvManagerClass.Get() : AWorldEnvManager::StaticClass();

    for (TActorIterator<AWorldEnvManager> It(World); It; ++It)
    {
        AWorldEnvManager* ExistingWorldEnvManager = *It;
        if (IsValid(ExistingWorldEnvManager) && ExistingWorldEnvManager->IsA(EffectiveWorldEnvManagerClass))
        {
            WorldEnvManagerActor = ExistingWorldEnvManager;
            bSpawnedWorldEnvManager = false;
            return;
        }
    }

    FActorSpawnParameters Params;
    Params.Owner = ConfigActor.Get();
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    WorldEnvManagerActor = World->SpawnActor<AWorldEnvManager>(EffectiveWorldEnvManagerClass, FTransform::Identity, Params);
    bSpawnedWorldEnvManager = IsValid(WorldEnvManagerActor);
}

bool UGameManagerSubSystem::CheckWorldSystemsLoaded()
{
    // Reserve the final progress node for entities.dat validation and actor restoration. Without
    // this reservation the streaming subsystem could publish 100% and hide the loading screen
    // before saved prefabs/vehicles had actually been recreated in the destination world.
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
    SetLoadingStatus(FMath::Clamp(Percent, 0.0f, 1.0f) * EntityRestoreProgressStart);
    return bSystemsReady && bVisibleProgressComplete;
}

void UGameManagerSubSystem::LoadWorldData()
{
    ActiveWorldData = NewObject<UWorldData>(this);
    if (!IsValid(ActiveWorldData))
    {
        return;
    }

    // level.json belongs to the map author. It is loaded without backup recovery and is never
    // rewritten by runtime code. Missing/invalid files simply leave UWorldData defaults active.
    const FString LevelJsonPath = GetWorldFilePath(LEVEL_FILE_NAME);
    FSafeJsonLimits JsonLimits;
    JsonLimits.MaxFileBytes = 64ll * 1024ll * 1024ll;
    JsonLimits.bAllowBackupRecovery = false;
    const FSafeJsonLoadResult JsonResult = FSafeFileIO::LoadJsonBlocking(LevelJsonPath, JsonLimits);
    const TSharedPtr<FJsonObject> Json = JsonResult.IsSuccess() ? JsonResult.JsonObject : nullptr;
    if (!UWorldData::DeserializeData(ActiveWorldData, Json))
    {
        if (JsonResult.Status != ESafeFileIOStatus::Missing)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("level.json could not be loaded safely; using map-setting defaults. Path=%s Error=%s"),
                *LevelJsonPath,
                *JsonResult.Error);
        }
        else
        {
            UE_LOG(LogTemp, Log,
                TEXT("level.json does not exist; using map-setting defaults without creating JSON. Path=%s"),
                *LevelJsonPath);
        }
    }

    // Runtime state is authoritative only when the versioned/CRC-checked DAT is valid.
    FWorldRuntimeData RuntimeData;
    FString DatError;
    const bool bLoadedRuntimeDat = FBinaryDataStore::LoadWorldRuntime(
        GetWorldDatPath(),
        RuntimeData,
        DatError);
    if (bLoadedRuntimeDat)
    {
        ActiveWorldData->WorldTime = RuntimeData.WorldTime;
        ActiveWorldData->Player = RuntimeData.SelectedPlayer;
    }
    else
    {
        // level.json is map-author input only. Runtime time and selected-player state never migrate
        // from JSON; missing/corrupt world.dat starts from defaults and is rewritten transactionally.
        if (!DatError.IsEmpty())
        {
            UE_LOG(LogTemp, Warning,
                TEXT("world.dat unavailable; runtime defaults will be committed to a new DAT. Path=%s Error=%s"),
                *GetWorldDatPath(),
                *DatError);
        }
        bPendingInitialWorldDataSave = true;
    }

    FVector LegacyLocation = FVector::ZeroVector;
    if (Json.IsValid() && TryReadSavedLocation(Json, FString(), LegacyLocation))
    {
        // Transform migration is completed by LoadPlayerData and then persisted to players.dat.
        PlayerLocation = LegacyLocation;
        ActiveWorldData->PlayerLocation = LegacyLocation;
        InitialPlayerLocationSource = EInitialPlayerLocationSource::LegacyWorldFile;
    }
    else
    {
        ActiveWorldData->PlayerLocation = PlayerLocation;
    }

    SetWorldData(ActiveWorldData);

    // world.dat does not contain player transforms, so it can be created immediately and does not
    // need to wait for the Pawn/PlayerStart handshake.
    if (!bLoadedRuntimeDat)
    {
        SaveWorldData();
    }
}

void UGameManagerSubSystem::LoadPlayerData()
{
    bInitialPlayerDataLoadCompleted = false;
    ActivePlayerData = NewObject<UPlayerData>(this);
    if (!IsValid(ActivePlayerData))
    {
        bInitialPlayerDataLoadCompleted = true;
        TryResolveInitialPlayerTransform();
        return;
    }

    ActivePlayerId = IsValid(ActiveWorldData) && !ActiveWorldData->Player.IsEmpty()
        ? FPaths::GetCleanFilename(ActiveWorldData->Player)
        : FString(TEXT("Player"));

    FString DatError;
    bool bLoadedPlayerData = FBinaryDataStore::LoadPlayers(
        GetPlayersDatPath(),
        ActivePlayerData,
        DatError);
    bool bMigratedLegacyJson = false;
    TSharedPtr<FJsonObject> LegacyJson;

    if (!bLoadedPlayerData)
    {
        // Read old player.json/players.json once for migration. Runtime never creates or updates
        // either file, and JSON backup recovery stays disabled because JSON is user-owned input.
        const TArray<FString> LegacyPaths =
        {
            GetWorldFilePath(LEGACY_PLAYER_FILE_NAME),
            GetWorldFilePath(LEGACY_PLAYERS_FILE_NAME)
        };
        FSafeJsonLimits Limits;
        Limits.MaxFileBytes = 64ll * 1024ll * 1024ll;
        Limits.bAllowBackupRecovery = false;
        for (const FString& LegacyPath : LegacyPaths)
        {
            const FSafeJsonLoadResult LegacyResult = FSafeFileIO::LoadJsonBlocking(LegacyPath, Limits);
            if (LegacyResult.IsSuccess() &&
                UPlayerData::DeserializeData(ActivePlayerData, LegacyResult.JsonObject))
            {
                LegacyJson = LegacyResult.JsonObject;
                bLoadedPlayerData = true;
                bMigratedLegacyJson = true;
                UE_LOG(LogTemp, Log,
                    TEXT("Migrating legacy player JSON to data/players.dat. Source=%s"),
                    *LegacyPath);
                break;
            }
        }

        if (!bLoadedPlayerData && !DatError.IsEmpty())
        {
            UE_LOG(LogTemp, Warning,
                TEXT("players.dat unavailable; a validated default player record will be created. Path=%s Error=%s"),
                *GetPlayersDatPath(),
                *DatError);
        }
    }

    const FWorldPlayerRecord* ExistingRecord = ActivePlayerData->FindPlayer(ActivePlayerId);
    const bool bHadPersistentRecord = ExistingRecord != nullptr;
    FWorldPlayerRecord& PlayerRecord = ActivePlayerData->FindOrAddPlayer(ActivePlayerId);
    const float PlayerMaxHealth = IsValid(ActiveWorldData)
        ? FMath::Max(1.0f, ActiveWorldData->Gameplay.PlayerMaxHealth)
        : 100.0f;
    if (!bHadPersistentRecord || !FMath::IsFinite(PlayerRecord.Health))
    {
        PlayerRecord.Health = PlayerMaxHealth;
    }
    else
    {
        PlayerRecord.Health = FMath::Clamp(PlayerRecord.Health, 0.0f, PlayerMaxHealth);
    }

    bool bHasValidPlayerLocation = bHadPersistentRecord && IsFiniteWorldLocation(PlayerRecord.Location);
    bool bHasValidPlayerRotation = bHadPersistentRecord && IsFiniteRotation(PlayerRecord.Rotation);
    if (bMigratedLegacyJson)
    {
        // Legacy JSON may omit individual fields and deserialize them to harmless defaults. Require
        // the fields to have existed before treating them as an intentional saved transform.
        const TSharedPtr<FJsonObject> SerializedPlayer =
            FindSerializedPlayerRecord(LegacyJson, ActivePlayerId);
        FVector LegacyLocation = FVector::ZeroVector;
        FRotator LegacyRotation = FRotator::ZeroRotator;
        bHasValidPlayerLocation =
            TryReadSavedLocation(SerializedPlayer, TEXT("Location"), LegacyLocation);
        bHasValidPlayerRotation =
            TryReadSavedRotation(SerializedPlayer, TEXT("Rotation"), LegacyRotation);
        if (bHasValidPlayerLocation)
        {
            PlayerRecord.Location = LegacyLocation;
        }
        if (bHasValidPlayerRotation)
        {
            PlayerRecord.Rotation = LegacyRotation;
        }
    }

    if (bHasValidPlayerLocation)
    {
        PlayerLocation = PlayerRecord.Location;
        InitialPlayerLocationSource = EInitialPlayerLocationSource::PlayerFile;
    }
    else
    {
        PlayerRecord.DisplayName = ActivePlayerId;
        if (InitialPlayerLocationSource == EInitialPlayerLocationSource::LegacyWorldFile)
        {
            PlayerRecord.Location = PlayerLocation;
        }
        bPendingInitialPlayerDataSave = true;
    }

    LoadedInitialPlayerRotation = bHasValidPlayerRotation
        ? PlayerRecord.Rotation.GetNormalized()
        : FRotator::ZeroRotator;
    bHasLoadedInitialPlayerRotation = bHasValidPlayerRotation;
    if (bHasValidPlayerLocation && !bHasLoadedInitialPlayerRotation)
    {
        bPendingInitialPlayerDataSave = true;
    }
    if (bMigratedLegacyJson)
    {
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
    if (const UWorld* World = GetWorld(); World && World->GetNetMode() == NM_Client)
    {
        return;
    }

    if (!IsValid(ActiveWorldData) || !FMath::IsFinite(ActiveWorldData->WorldTime))
    {
        UE_LOG(LogTemp, Warning, TEXT("world.dat save skipped because runtime world state is invalid."));
        return;
    }

    FWorldRuntimeData RuntimeData;
    RuntimeData.WorldTime = ActiveWorldData->WorldTime;
    RuntimeData.SelectedPlayer = FPaths::GetCleanFilename(ActiveWorldData->Player);
    FBinaryDataStore::SaveWorldRuntimeAsync(
        GetWorldDatPath(),
        RuntimeData,
        [](FSafeFileWriteResult Result)
        {
            if (!Result.IsSuccess() &&
                Result.Status != ESafeFileIOStatus::ShuttingDown &&
                Result.Status != ESafeFileIOStatus::Superseded)
            {
                UE_LOG(LogTemp, Error,
                    TEXT("Failed to save world.dat safely. Path=%s Error=%s"),
                    *Result.Path,
                    *Result.Error);
            }
        });
    bPendingInitialWorldDataSave = false;
}

void UGameManagerSubSystem::SavePlayerData()
{
    if (const UWorld* World = GetWorld(); World && World->GetNetMode() == NM_Client)
    {
        return;
    }

    if (!IsValid(ActivePlayerData))
    {
        return;
    }

    if (!bInitialPlayerTransformResolved &&
        (InitialPlayerLocationSource == EInitialPlayerLocationSource::None ||
            bPendingInitialPlayerDataSave))
    {
        // A newly created record must wait until the registered Pawn supplies both a valid
        // PlayerStart location and rotation. Existing validated DAT records can be saved directly.
        bPendingInitialPlayerDataSave = true;
        return;
    }

    if (!IsFiniteWorldLocation(PlayerLocation))
    {
        UE_LOG(LogTemp, Warning, TEXT("players.dat save skipped because PlayerLocation is invalid."));
        return;
    }

    FWorldPlayerRecord& PlayerRecord = ActivePlayerData->FindOrAddPlayer(ActivePlayerId);
    PlayerRecord.Location = PlayerLocation;
    if (IsValid(ActiveWorldData))
    {
        PlayerRecord.Health = FMath::Clamp(
            FMath::IsFinite(PlayerRecord.Health) ? PlayerRecord.Health : ActiveWorldData->Gameplay.PlayerMaxHealth,
            0.0f,
            FMath::Max(1.0f, ActiveWorldData->Gameplay.PlayerMaxHealth));
    }
    if (bInitialPlayerTransformResolved)
    {
        if (const AActor* Player = PlayerActor.Get())
        {
            const FRotator RuntimeRotation = Player->GetActorRotation();
            if (IsFiniteRotation(RuntimeRotation))
            {
                PlayerRecord.Rotation = RuntimeRotation.GetNormalized();
            }
        }
    }

    FBinaryDataStore::SavePlayersAsync(
        GetPlayersDatPath(),
        ActivePlayerData,
        [](FSafeFileWriteResult Result)
        {
            if (!Result.IsSuccess() &&
                Result.Status != ESafeFileIOStatus::ShuttingDown &&
                Result.Status != ESafeFileIOStatus::Superseded)
            {
                UE_LOG(LogTemp, Error,
                    TEXT("Failed to save players.dat safely. Path=%s Error=%s"),
                    *Result.Path,
                    *Result.Error);
            }
        });
    bPendingInitialPlayerDataSave = false;
}

void UGameManagerSubSystem::SetSelectedPlayerForRuntime(const FString& PlayerFileName)
{
    if (!IsValid(ActiveWorldData))
    {
        return;
    }

    FString SafeName = FPaths::GetCleanFilename(PlayerFileName);
    SafeName.TrimStartAndEndInline();
    if (SafeName.Contains(TEXT("..")) || SafeName.Contains(TEXT("/")) || SafeName.Contains(TEXT("\\")))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Rejected unsafe selected-player name: %s"),
            *PlayerFileName);
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
    const EPlayMode ConfiguredDefault = ConfigActor.IsValid()
        ? ConfigActor->PlayMode
        : EPlayMode::Creator;
    const FString RuntimeModeKey = IsValid(ActiveWorldData)
        ? ActiveWorldData->Gameplay.WorldGameMode
        : FString();
    bool bRecognizedModeKey = false;
    PlayMode = ResolveRuntimePlayModeKey(RuntimeModeKey, ConfiguredDefault, bRecognizedModeKey);

    if (!bRecognizedModeKey)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[RuntimePlayMode] Unknown Runtime Play Mode Key '%s' in level.json. Using GameManagerActor default '%s'."),
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
        // Re-run the idempotent DAT restore at the exact point where world streaming is ready.
        // The earlier next-tick attempt can legitimately occur before the persistent GameInstance
        // streaming subsystem has attached itself to this destination UWorld.
        if (!bSavedSceneLoaded && !bSavedSceneLoadInProgress && !bSavedSceneLoadFailed)
        {
            LoadSavedScene();
        }

        // entities.dat is a real loading node, not a post-load side effect. Wait while validation or
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

    // Player transforms live only in data/players.dat. Keep the in-memory mirrors current so the
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
    PrefabActorClass = APrefabActor::StaticClass();
    VehiclePawnClass = AVehiclePawn::StaticClass();
    WeaponActorClass = AWeaponActor::StaticClass();
    WorldEnvManagerClass = AWorldEnvManager::StaticClass();
    SpawnActorClass = AglTFStreamActor::StaticClass();
    WaterClass = nullptr;
    RainWeatherActorClass = nullptr;
    LoadingWidgetClass = nullptr;
    OceanTransform = FTransform::Identity;

    EquippedWeapon = nullptr;
    SpawnedPrefabs.Empty();
    SpawnedVehicles.Empty();

    // This method is used only at world/session teardown. Empty releases the allocator capacity
    // retained by large worlds instead of carrying it into menus and subsequent level loads.
    PrefabFiles.Empty();
    VehicleFiles.Empty();
    WeaponFiles.Empty();
    AvailableItems.Empty();
    ToolbarSlots.Empty();
    bToolbarInitialized = false;

    // Keep CurrentWorldName across ordinary level travel. A confirmed menu return clears it only
    // after StopGameManager has finished saving the source gameplay world.
    ResetInitialPlayerTransformState();
    SelectedToolbarSlotIndex = 0;
    CurrentPrefabIndex = 0;
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
    LastKnownSceneRecords.Empty();
    bIsSavingScene = false;
    PendingWorldBakeModels.Empty();
    ActiveWorldBakeActor = nullptr;
    bWorldBakeInProgress = false;
    bWorldBakeStateFilesSaved = false;
    WorldBakeTotalModels = 0;
    WorldBakeCompletedModels = 0;
    WorldBakeFailedModels = 0;
    WorldBakeNextModelIndex = 0;

    CachedPlacementGridCenter = FVector::ZeroVector;
    CachedPlacementGridRadius = 0.0f;
    bPlacementGridBuilt = false;
}

void UGameManagerSubSystem::EnsureAssetFolders() const
{
    IFileManager::Get().MakeDirectory(*GetPrefabDirectory(), true);
    IFileManager::Get().MakeDirectory(*GetItemsDirectory(), true);
    IFileManager::Get().MakeDirectory(*GetDataDirectory(), true);
}

FString UGameManagerSubSystem::GetWorldRootPath() const
{
    const FString WorldName = CurrentWorldName.IsEmpty() ? FString(TEXT("New World")) : CurrentWorldName;
    return FPaths::Combine(PATH_ROOT, WorldName);
}

FString UGameManagerSubSystem::GetPrefabDirectory() const
{
    return FPaths::Combine(GetWorldRootPath(), TEXT("model"), TEXT("prefab"));
}

FString UGameManagerSubSystem::GetItemsDirectory() const
{
    return FPaths::Combine(GetWorldRootPath(), TEXT("item"));
}

FString UGameManagerSubSystem::GetDataDirectory() const
{
    return FPaths::Combine(GetWorldRootPath(), TEXT("data"));
}

FString UGameManagerSubSystem::GetPlayersDatPath() const
{
    return FPaths::Combine(GetDataDirectory(), TEXT("players.dat"));
}

FString UGameManagerSubSystem::GetWorldDatPath() const
{
    return FPaths::Combine(GetDataDirectory(), TEXT("world.dat"));
}

FString UGameManagerSubSystem::GetManifestPath() const
{
    return FPaths::Combine(GetDataDirectory(), TEXT("entities.dat"));
}

FString UGameManagerSubSystem::MakePlacementSourcePathForSave(const FString& SourcePath) const
{
    const FString NormalizedSource = NormalizeFullPathForWorldData(SourcePath);
    const FString NormalizedWorldRoot = NormalizeFullPathForWorldData(GetWorldRootPath());
    if (NormalizedSource.IsEmpty() || NormalizedWorldRoot.IsEmpty() ||
        !IsPathInsideDirectory(NormalizedSource, NormalizedWorldRoot))
    {
        // External assets remain absolute so an explicitly configured shared asset can still load.
        return NormalizedSource;
    }

    FString Relative = NormalizedSource;
    FString RelativeBase = NormalizedWorldRoot;
    if (!RelativeBase.EndsWith(TEXT("/")))
    {
        RelativeBase += TEXT("/");
    }
    if (!FPaths::MakePathRelativeTo(Relative, *RelativeBase))
    {
        return NormalizedSource;
    }

    FPaths::NormalizeFilename(Relative);
    FPaths::CollapseRelativeDirectories(Relative);
    if (Relative.IsEmpty() || Relative == TEXT("..") || Relative.StartsWith(TEXT("../")))
    {
        return NormalizedSource;
    }
    return Relative;
}

FString UGameManagerSubSystem::ResolvePlacementSourcePath(
    const FString& SavedPath,
    const EPlacedObjectKind Kind) const
{
    FString Trimmed = SavedPath.TrimStartAndEnd();
    FPaths::NormalizeFilename(Trimmed);
    if (Trimmed.IsEmpty())
    {
        return FString();
    }

    IFileManager& FileManager = IFileManager::Get();
    const FString WorldRoot = NormalizeFullPathForWorldData(GetWorldRootPath());

    // New records are stored relative to the selected world root. Reject traversal before testing.
    if (FPaths::IsRelative(Trimmed))
    {
        const FString Candidate = NormalizeFullPathForWorldData(FPaths::Combine(WorldRoot, Trimmed));
        if (IsPathInsideDirectory(Candidate, WorldRoot) && FileManager.FileExists(*Candidate))
        {
            return Candidate;
        }
    }
    else
    {
        const FString ExistingAbsolute = NormalizeFullPathForWorldData(Trimmed);
        if (!ExistingAbsolute.IsEmpty() && FileManager.FileExists(*ExistingAbsolute))
        {
            return ExistingAbsolute;
        }
    }

    // Repair records written by older builds that embedded an absolute path to a different
    // installation/world root. Only recognized asset-directory suffixes are carried forward.
    FString Canonical = Trimmed;
    Canonical.ReplaceInline(TEXT("\\"), TEXT("/"));
    const TArray<FString> Markers =
    {
        TEXT("/prefab/"),
        TEXT("/item/"),
        TEXT("/items/"),
        TEXT("/model/")
    };
    for (const FString& Marker : Markers)
    {
        const int32 MarkerIndex = Canonical.Find(Marker, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
        if (MarkerIndex == INDEX_NONE)
        {
            continue;
        }

        FString RelativeSuffix = Canonical.Mid(MarkerIndex + 1);
        if (RelativeSuffix.StartsWith(TEXT("items/"), ESearchCase::IgnoreCase))
        {
            RelativeSuffix = TEXT("item/") + RelativeSuffix.Mid(6);
        }
        const FString Candidate = NormalizeFullPathForWorldData(FPaths::Combine(WorldRoot, RelativeSuffix));
        if (IsPathInsideDirectory(Candidate, WorldRoot) && FileManager.FileExists(*Candidate))
        {
            UE_LOG(LogTemp, Log,
                TEXT("Repaired saved %s source path for the current world. Saved=%s Resolved=%s"),
                Kind == EPlacedObjectKind::Vehicle ? TEXT("vehicle") : TEXT("prefab"),
                *SavedPath,
                *Candidate);
            return Candidate;
        }
    }

    // Last-resort relocation support: when an author moved an asset within the selected world,
    // search by the exact filename. Accept only one match so an ambiguous deployment can never
    // silently spawn the wrong prefab or vehicle.
    const FString CleanFileName = FPaths::GetCleanFilename(Trimmed);
    if (!CleanFileName.IsEmpty())
    {
        const TArray<FString> SearchRoots =
        {
            GetPrefabDirectory(),
            FPaths::Combine(WorldRoot, TEXT("model")),
            GetItemsDirectory()
        };
        TArray<FString> UniqueMatches;
        for (const FString& SearchRoot : SearchRoots)
        {
            if (!FileManager.DirectoryExists(*SearchRoot))
            {
                continue;
            }

            TArray<FString> Matches;
            FileManager.FindFilesRecursive(Matches, *SearchRoot, *CleanFileName, true, false, false);
            for (const FString& Match : Matches)
            {
                const FString NormalizedMatch = NormalizeFullPathForWorldData(Match);
                if (!IsPathInsideDirectory(NormalizedMatch, WorldRoot))
                {
                    continue;
                }
                const bool bAlreadyAdded = UniqueMatches.ContainsByPredicate([&NormalizedMatch](const FString& Existing)
                {
                    return Existing.Equals(NormalizedMatch, ESearchCase::IgnoreCase);
                });
                if (!bAlreadyAdded)
                {
                    UniqueMatches.Add(NormalizedMatch);
                }
            }
        }

        if (UniqueMatches.Num() == 1)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("Recovered relocated saved %s by unique filename. Saved=%s Resolved=%s"),
                Kind == EPlacedObjectKind::Vehicle ? TEXT("vehicle") : TEXT("prefab"),
                *SavedPath,
                *UniqueMatches[0]);
            return UniqueMatches[0];
        }
        if (UniqueMatches.Num() > 1)
        {
            UE_LOG(LogTemp, Error,
                TEXT("Saved entity source filename is ambiguous in the selected world. Saved=%s Matches=%d"),
                *SavedPath,
                UniqueMatches.Num());
        }
    }

    return FString();
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
        // replacing an unreadable/non-applied entities.dat with an empty scene during autosave or EndPlay.
        bSavedSceneLoadFailed = true;
        UE_LOG(LogTemp, Error,
            TEXT("Entity DAT restore failed after %d attempts. World=%s Phase=%s Reason=%s"),
            AttemptCount,
            *GetWorldRootPath(),
            bWaitingForWorldReadiness ? TEXT("world-readiness") : TEXT("DAT-validation/apply"),
            *Reason);
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        bSavedSceneLoadFailed = true;
        UE_LOG(LogTemp, Error,
            TEXT("Entity DAT load could not be retried because the world is unavailable: %s"),
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
        TEXT("Entity DAT load will retry. Phase=%s Attempt=%d/%d Reason=%s"),
        bWaitingForWorldReadiness ? TEXT("world-readiness") : TEXT("DAT-validation/apply"),
        AttemptCount,
        MaximumAttempts,
        *Reason);
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

    TArray<FString> ItemDirectories;
    ItemDirectories.Add(GetItemsDirectory());
    ItemDirectories.Add(FPaths::Combine(FPaths::ProjectDir(), TEXT("World"), TEXT("item")));
    ItemDirectories.Add(FPaths::Combine(FPaths::ProjectDir(), TEXT("World"), WorldName, TEXT("item")));

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

    TArray<FString> PrefabCandidates;
    TArray<FString> ItemCandidates;
    AppendGltfFiles(PrefabDirectories, PrefabCandidates);
    AppendGltfFiles(ItemDirectories, ItemCandidates);

    auto ClassifyAsset = [this](const FString& ModelPath, const bool bItemFolderFallback)
    {
        bool bExplicitType = false;
        EAuthoredAssetType AssetType = ReadAuthoredAssetType(ModelPath, bExplicitType);
        if (!bExplicitType || AssetType == EAuthoredAssetType::Auto)
        {
            AssetType = bItemFolderFallback
                ? EAuthoredAssetType::Weapon
                : (DoesAssetFileContainWheelTag(ModelPath)
                    ? EAuthoredAssetType::Vehicle
                    : EAuthoredAssetType::Entity);
        }

        switch (AssetType)
        {
        case EAuthoredAssetType::Vehicle:
            VehicleFiles.AddUnique(ModelPath);
            break;
        case EAuthoredAssetType::Weapon:
            WeaponFiles.AddUnique(ModelPath);
            break;
        case EAuthoredAssetType::Prefab:
            PrefabFiles.AddUnique(ModelPath);
            break;
        case EAuthoredAssetType::Entity:
        case EAuthoredAssetType::Auto:
        default:
            break;
        }
    };

    for (const FString& ModelPath : PrefabCandidates)
    {
        bool bExplicitType = false;
        const EAuthoredAssetType AssetType = ReadAuthoredAssetType(ModelPath, bExplicitType);
        if (bExplicitType && AssetType == EAuthoredAssetType::Prefab)
        {
            PrefabFiles.AddUnique(ModelPath);
        }
    }
    for (const FString& ModelPath : ItemCandidates)
    {
        ClassifyAsset(ModelPath, true);
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

    auto ContainsReservedToken = [&Bytes, &ToUpperAscii](const ANSICHAR* Token) -> bool
    {
        const int32 TokenLen = FCStringAnsi::Strlen(Token);
        if (TokenLen <= 0 || Bytes.Num() < TokenLen + 1)
        {
            return false;
        }

        // The leading ';' is part of the grammar. The byte immediately after the token must
        // terminate the JSON string/name or begin another reserved token. This prevents a raw
        // byte match such as ;WHEELHOUSE from being treated as ;WHEEL.
        for (int32 Index = 0; Index <= Bytes.Num() - TokenLen - 1; ++Index)
        {
            if (Bytes[Index] != static_cast<uint8>(';'))
            {
                continue;
            }

            bool bMatches = true;
            for (int32 TokenIndex = 0; TokenIndex < TokenLen; ++TokenIndex)
            {
                if (ToUpperAscii(Bytes[Index + 1 + TokenIndex]) != static_cast<uint8>(Token[TokenIndex]))
                {
                    bMatches = false;
                    break;
                }
            }

            if (!bMatches)
            {
                continue;
            }

            const uint8 Next = Bytes[Index + 1 + TokenLen];
            if (Next == static_cast<uint8>(';') || Next == static_cast<uint8>('"') || Next == 0)
            {
                return true;
            }
        }

        return false;
    };

    return ContainsReservedToken("WHEEL");
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
        for (int32 Index = 0; Index < VehicleFiles.Num(); ++Index)
        {
            AvailableItems.Add(MakeToolbarItem(
                EToolbarItemKind::Vehicle,
                GetAssetDisplayName(VehicleFiles[Index]),
                VehicleFiles[Index],
                Index));
        }

        for (int32 Index = 0; Index < PrefabFiles.Num(); ++Index)
        {
            AvailableItems.Add(MakeToolbarItem(
                EToolbarItemKind::Prefab,
                GetAssetDisplayName(PrefabFiles[Index]),
                PrefabFiles[Index],
                Index));
        }

        for (int32 Index = 0; Index < WeaponFiles.Num(); ++Index)
        {
            AvailableItems.Add(MakeToolbarItem(
                EToolbarItemKind::Weapon,
                GetAssetDisplayName(WeaponFiles[Index]),
                WeaponFiles[Index],
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

        if (!Item.SourcePath.IsEmpty()
            && Candidate.SourcePath.Equals(Item.SourcePath, ESearchCase::IgnoreCase))
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
    const FToolbarItem Item = GetSelectedToolbarItem();

    switch (Item.Kind)
    {
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
    ScanAssetFolders();
    BuildAvailableItems();
    CurrentMode = EToolMode::PlacePrefab;
    LastSaveMessage = TEXT("Prefab 도구: 중앙 십자가 위치에 좌클릭으로 현재 Prefab을 설치합니다.");
    NotifyStateChanged();
}

void UGameManagerSubSystem::SelectVehicleTool()
{
    ScanAssetFolders();
    BuildAvailableItems();
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
        LastSaveMessage = TEXT("item/ 폴더에 gltf 또는 glb 무기가 없습니다.");
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
        LastSaveMessage = TEXT("item/ 폴더에 gltf 또는 glb 무기가 없습니다.");
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
        && CurrentMode == EToolMode::PlacePrefab;
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
        for (const TWeakObjectPtr<APrefabActor>& PrefabReference : SpawnedPrefabs)
        {
            const APrefabActor* Prefab = PrefabReference.Get();
            if (IsValid(Prefab) && Prefab->GetBaseName().Equals(BaseName, ESearchCase::IgnoreCase))
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
    const bool bVehiclePlacement = CurrentMode == EToolMode::PlaceVehicle
        || Item.Kind == EToolbarItemKind::Vehicle;
    if (!bVehiclePlacement)
    {
        Location = ApplyGridSnap(Location);
    }

    switch (Item.Kind)
    {
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
        if (CurrentMode == EToolMode::PlacePrefab && bHasPlacementLocation)
        {
            PlaceCurrentPrefab(Location);
        }
        else if (CurrentMode == EToolMode::PlaceVehicle && bHasPlacementLocation)
        {
            PlaceVehicle(Location, Item.SourcePath);
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
    // Stable endpoint retained for existing input mappings. Placement is edge-triggered on press.
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
    InputPrimaryAction();
}

void UGameManagerSubSystem::ConfirmCurrentPendingLocation()
{
    InputPrimaryPressed();
}

void UGameManagerSubSystem::PlaceCurrentPrefab(const FVector& Location)
{
    ScanAssetFolders();
    if (!PrefabFiles.IsValidIndex(CurrentPrefabIndex))
    {
        LastSaveMessage = TEXT("prefab/ 폴더에 gltf 또는 glb가 없습니다.");
        return;
    }

    const FString SourceFile = GlbValidation::NormalizePath(PrefabFiles[CurrentPrefabIndex]);
    if (SourceFile.IsEmpty() || !IFileManager::Get().FileExists(*SourceFile))
    {
        LastSaveMessage = TEXT("Prefab 에셋 파일이 없어 설치하지 않았습니다.");
        return;
    }

    const FString BaseName = FPaths::GetBaseFilename(SourceFile);
    const FString ObjectName = MakeObjectName(BaseName, EPlacedObjectKind::Prefab);

    FActorSpawnParameters Params;
    Params.Owner = ConfigActor.Get();
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    const FRotator SpawnRot = FRotator(0.0f, GetWorld()->GetFirstPlayerController() ? GetWorld()->GetFirstPlayerController()->GetControlRotation().Yaw : 0.0f, 0.0f);
    UClass* PrefabSpawnClass = PrefabActorClass ? PrefabActorClass.Get() : APrefabActor::StaticClass();
    APrefabActor* Actor = GetWorld()->SpawnActor<APrefabActor>(PrefabSpawnClass, FTransform(SpawnRot, Location), Params);
    if (!IsValid(Actor) && PrefabSpawnClass != APrefabActor::StaticClass())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Configured prefab actor class failed to spawn; retrying with native APrefabActor. Class=%s"),
            *GetNameSafe(PrefabSpawnClass));
        Actor = GetWorld()->SpawnActor<APrefabActor>(APrefabActor::StaticClass(), FTransform(SpawnRot, Location), Params);
    }
    if (IsValid(Actor))
    {
        Actor->SetRenderOnlyMode(UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this));
    }
    if (IsValid(Actor) && Actor->LoadPrefab(SourceFile, ObjectName))
    {
        SpawnedPrefabs.Add(TWeakObjectPtr<APrefabActor>(Actor));
        LastSaveMessage = FString::Printf(TEXT("설치됨: %s"), *ObjectName);
        SaveScene();
    }
    else if (IsValid(Actor))
    {
        Actor->Destroy();
        LastSaveMessage = TEXT("Prefab 로드 실패");
    }
}

void UGameManagerSubSystem::PlaceVehicle(const FVector& Location, const FString& SourceFile)
{
    const FString NormalizedSourceFile = GlbValidation::NormalizePath(SourceFile);
    if (NormalizedSourceFile.IsEmpty() || !IFileManager::Get().FileExists(*NormalizedSourceFile))
    {
        LastSaveMessage = TEXT("차량 에셋 파일이 없어 설치하지 않았습니다.");
        return;
    }

    FString ValidationReason;
    if (!GlbValidation::ValidateRuntimeModelFile(NormalizedSourceFile, ValidationReason))
    {
        LastSaveMessage = FString::Printf(TEXT("차량 에셋이 유효하지 않아 설치하지 않았습니다: %s"), *ValidationReason);
        return;
    }

    FActorSpawnParameters Params;
    Params.Owner = ConfigActor.Get();
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
    const FRotator SpawnRot = FRotator(
        0.0f,
        GetWorld()->GetFirstPlayerController() ? GetWorld()->GetFirstPlayerController()->GetControlRotation().Yaw : 0.0f,
        0.0f);
    UClass* VehicleSpawnClass = VehiclePawnClass ? VehiclePawnClass.Get() : AVehiclePawn::StaticClass();
    const FVector SpawnLocation = Location + FVector(0.0f, 0.0f, 220.0f);
    AVehiclePawn* Vehicle = GetWorld()->SpawnActor<AVehiclePawn>(
        VehicleSpawnClass,
        FTransform(SpawnRot, SpawnLocation),
        Params);
    if (!IsValid(Vehicle) && VehicleSpawnClass != AVehiclePawn::StaticClass())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Configured vehicle actor class failed to spawn; retrying with native AVehiclePawn. Class=%s"),
            *GetNameSafe(VehicleSpawnClass));
        Vehicle = GetWorld()->SpawnActor<AVehiclePawn>(
            AVehiclePawn::StaticClass(),
            FTransform(SpawnRot, SpawnLocation),
            Params);
    }
    if (!IsValid(Vehicle))
    {
        LastSaveMessage = TEXT("차량 액터를 생성하지 못했습니다.");
        return;
    }

    const FString VehicleBaseName = FPaths::GetBaseFilename(NormalizedSourceFile);
    const FString VehicleObjectName = MakeObjectName(VehicleBaseName, EPlacedObjectKind::Vehicle);
    if (!Vehicle->LoadVehicleModel(NormalizedSourceFile, VehicleObjectName))
    {
        Vehicle->Destroy();
        LastSaveMessage = TEXT("차량 모델을 로드하지 못해 액터를 제거했습니다.");
        return;
    }

    SpawnedVehicles.Add(TWeakObjectPtr<AVehiclePawn>(Vehicle));
    LastSaveMessage = TEXT("glTF 자동차 설치됨. F키로 탑승하세요.");
    SaveScene();
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
        LastSaveMessage = TEXT("현재 상태에서는 차량에 탑승할 수 없습니다.");
        NotifyStateChanged();
    }
}

void UGameManagerSubSystem::CollectSceneRecords(TArray<FPlacedObjectRecord>& OutPlaced) const
{
    // Start from the last fully validated snapshot. During level teardown Unreal may call actor
    // EndPlay before the manager actor, so rebuilding only from live pointers can otherwise publish
    // a valid but empty entities.dat over a non-empty world.
    OutPlaced = LastKnownSceneRecords;
    OutPlaced.RemoveAll([](const FPlacedObjectRecord& Record)
    {
        return Record.ObjectName.IsEmpty()
            || Record.SourceFile.IsEmpty()
            || (Record.Kind != EPlacedObjectKind::Prefab && Record.Kind != EPlacedObjectKind::Vehicle);
    });

    TSet<int32> ReplacedRecordIndices;
    auto UpsertLiveRecord = [this, &OutPlaced, &ReplacedRecordIndices](FPlacedObjectRecord&& Record)
    {
        Record.SourceFile = MakePlacementSourcePathForSave(Record.SourceFile);
        if (Record.ObjectName.IsEmpty() || Record.SourceFile.IsEmpty())
        {
            return;
        }

        int32 ExistingIndex = INDEX_NONE;
        for (int32 Index = 0; Index < OutPlaced.Num(); ++Index)
        {
            if (ReplacedRecordIndices.Contains(Index))
            {
                continue;
            }

            const FPlacedObjectRecord& Existing = OutPlaced[Index];
            if (Existing.Kind == Record.Kind
                && Existing.ObjectName.Equals(Record.ObjectName, ESearchCase::IgnoreCase))
            {
                ExistingIndex = Index;
                break;
            }
        }

        if (ExistingIndex != INDEX_NONE)
        {
            OutPlaced[ExistingIndex] = MoveTemp(Record);
            ReplacedRecordIndices.Add(ExistingIndex);
        }
        else
        {
            const int32 AddedIndex = OutPlaced.Add(MoveTemp(Record));
            ReplacedRecordIndices.Add(AddedIndex);
        }
    };

    for (const TWeakObjectPtr<APrefabActor>& PrefabReference : SpawnedPrefabs)
    {
        const APrefabActor* Prefab = PrefabReference.Get();
        if (IsValid(Prefab) && !Prefab->IsActorBeingDestroyed())
        {
            UpsertLiveRecord(Prefab->ToPlacementRecord());
        }
    }

    int32 VehicleRecordIndex = 0;
    for (const TWeakObjectPtr<AVehiclePawn>& VehicleReference : SpawnedVehicles)
    {
        const AVehiclePawn* Vehicle = VehicleReference.Get();
        if (IsValid(Vehicle) && !Vehicle->IsActorBeingDestroyed())
        {
            UpsertLiveRecord(Vehicle->ToPlacementRecord(VehicleRecordIndex));
            ++VehicleRecordIndex;
        }
    }
}

bool UGameManagerSubSystem::SaveScene()
{
    CompactTrackedEntityReferences();

    if (const UWorld* World = GetWorld(); World && World->GetNetMode() == NM_Client)
    {
        return false;
    }

    // Never publish an empty snapshot over a DAT generation that has not yet been validated/applied.
    // This covers autosave, EndPlay, and world-travel save paths. A missing DAT becomes a valid empty
    // scene only after LoadSavedScene() explicitly marks it as loaded.
    if (!bSavedSceneLoaded || bSavedSceneLoadInProgress || bSavedSceneLoadFailed)
    {
        LastSaveMessage = bSavedSceneLoadFailed
            ? TEXT("entities.dat 복원 실패 상태이므로 기존 데이터를 보호하기 위해 저장을 건너뜁니다.")
            : TEXT("entities.dat 복원이 끝나기 전에는 엔티티 저장을 시작하지 않습니다.");
        UE_LOG(LogTemp, Warning,
            TEXT("Runtime entity save skipped until DAT restore is safe. Loaded=%s InProgress=%s Failed=%s Path=%s"),
            bSavedSceneLoaded ? TEXT("true") : TEXT("false"),
            bSavedSceneLoadInProgress ? TEXT("true") : TEXT("false"),
            bSavedSceneLoadFailed ? TEXT("true") : TEXT("false"),
            *GetManifestPath());
        NotifyStateChanged();
        return false;
    }

    if (bIsSavingScene)
    {
        return false;
    }
    bIsSavingScene = true;

    EnsureAssetFolders();
    TArray<FPlacedObjectRecord> Placed;
    CollectSceneRecords(Placed);
    const FString EntitiesDatPath = GetManifestPath();
    const FSafeFileWriteResult SaveResult = FBinaryDataStore::SaveEntitiesBlocking(EntitiesDatPath, Placed);

    bool bSaved = SaveResult.IsSuccess();
    FString FailureReason = SaveResult.Error;
    if (bSaved)
    {
        // Do not trust a successful rename alone. Read the committed generation through the same
        // schema/CRC parser used at startup and compare every record before publishing the snapshot.
        TArray<FPlacedObjectRecord> VerifiedRecords;
        FString VerifyError;
        if (!FBinaryDataStore::LoadEntities(EntitiesDatPath, VerifiedRecords, VerifyError))
        {
            bSaved = false;
            FailureReason = FString::Printf(TEXT("read-after-write validation failed: %s"), *VerifyError);
        }
        else if (!ArePlacedObjectRecordsEquivalent(Placed, VerifiedRecords))
        {
            bSaved = false;
            FailureReason = TEXT("read-after-write validation returned different entity records");
        }
        else
        {
            LastKnownSceneRecords = MoveTemp(VerifiedRecords);
        }
    }

    LastSaveMessage = bSaved
        ? FString::Printf(TEXT("엔티티 저장 및 재검증 완료: %d개"), Placed.Num())
        : FString::Printf(TEXT("엔티티 저장/검증 실패: %s"), *FailureReason);

    if (bSaved)
    {
        UE_LOG(LogTemp, Display,
            TEXT("Runtime entities saved and verified. World=%s Path=%s Records=%d"),
            *GetWorldRootPath(),
            *EntitiesDatPath,
            Placed.Num());
    }
    else
    {
        UE_LOG(LogTemp, Error,
            TEXT("Runtime entities save/verification failed. World=%s Path=%s Records=%d Reason=%s"),
            *GetWorldRootPath(),
            *EntitiesDatPath,
            Placed.Num(),
            *FailureReason);
    }

    bIsSavingScene = false;
    NotifyStateChanged();
    return bSaved;
}

void UGameManagerSubSystem::BakeWorldData()
{
    UWorld* World = GetWorld();
    if (!World || World->GetNetMode() == NM_Client)
    {
        const FString Message = TEXT("월드 Bake는 권한이 있는 서버/싱글플레이 월드에서만 실행할 수 있습니다.");
        LastSaveMessage = Message;
        OnWorldBakeCompleted.Broadcast(false, Message);
        NotifyStateChanged();
        return;
    }

    if (bWorldBakeInProgress)
    {
        LastSaveMessage = TEXT("월드 DAT/SCZ Bake가 이미 진행 중입니다.");
        NotifyStateChanged();
        return;
    }

    EnsureAssetFolders();
    bWorldBakeInProgress = true;
    bWorldBakeStateFilesSaved = true;
    WorldBakeProgressValue = 0.0f;
    WorldBakeTotalModels = 0;
    WorldBakeCompletedModels = 0;
    WorldBakeFailedModels = 0;
    WorldBakeNextModelIndex = 0;
    PendingWorldBakeModels.Empty();
    ActiveWorldBakeActor = nullptr;
    OnWorldBakeProgress.Broadcast(0.0f);

    const bool bEntitiesSaved = SaveScene();
    bWorldBakeStateFilesSaved &= bEntitiesSaved;
    if (!bEntitiesSaved)
    {
        UE_LOG(LogTemp, Error,
            TEXT("World bake failed to write and verify entities.dat. Path=%s"),
            *GetManifestPath());
    }

    FWorldRuntimeData RuntimeData;
    const UWorldData* RuntimeWorldData = IsValid(ActiveWorldData) ? ActiveWorldData.Get() : CurrentWorldData.Get();
    RuntimeData.WorldTime = RuntimeWorldData && FMath::IsFinite(RuntimeWorldData->WorldTime)
        ? RuntimeWorldData->WorldTime
        : 0.0f;
    RuntimeData.SelectedPlayer = RuntimeWorldData && !RuntimeWorldData->Player.IsEmpty()
        ? FPaths::GetCleanFilename(RuntimeWorldData->Player)
        : FPaths::GetCleanFilename(ActivePlayerId);
    const FSafeFileWriteResult WorldResult = FBinaryDataStore::SaveWorldRuntimeBlocking(
        GetWorldDatPath(),
        RuntimeData);
    bWorldBakeStateFilesSaved &= WorldResult.IsSuccess();
    if (!WorldResult.IsSuccess())
    {
        UE_LOG(LogTemp, Error, TEXT("World bake failed to write world.dat: %s"), *WorldResult.Error);
    }

    UPlayerData* PlayersToSave = ActivePlayerData.Get();
    if (!IsValid(PlayersToSave))
    {
        PlayersToSave = NewObject<UPlayerData>(this);
    }
    const FSafeFileWriteResult PlayersResult = FBinaryDataStore::SavePlayersBlocking(
        GetPlayersDatPath(),
        PlayersToSave);
    bWorldBakeStateFilesSaved &= PlayersResult.IsSuccess();
    if (!PlayersResult.IsSuccess())
    {
        UE_LOG(LogTemp, Error, TEXT("World bake failed to write players.dat: %s"), *PlayersResult.Error);
    }

    IFileManager& FileManager = IFileManager::Get();
    const FString WorldRoot = GetWorldRootPath();
    TArray<FString> GlbFiles;
    TArray<FString> GltfFiles;
    FileManager.FindFilesRecursive(GlbFiles, *WorldRoot, TEXT("*.glb"), true, false, false);
    FileManager.FindFilesRecursive(GltfFiles, *WorldRoot, TEXT("*.gltf"), true, false, false);
    PendingWorldBakeModels.Reserve(GlbFiles.Num() + GltfFiles.Num());
    for (FString& ModelPath : GlbFiles)
    {
        ModelPath = GlbValidation::NormalizePath(ModelPath);
        if (!ModelPath.IsEmpty())
        {
            PendingWorldBakeModels.AddUnique(ModelPath);
        }
    }
    for (FString& ModelPath : GltfFiles)
    {
        ModelPath = GlbValidation::NormalizePath(ModelPath);
        if (!ModelPath.IsEmpty())
        {
            PendingWorldBakeModels.AddUnique(ModelPath);
        }
    }
    PendingWorldBakeModels.Sort([](const FString& A, const FString& B)
    {
        return A.Compare(B, ESearchCase::IgnoreCase) < 0;
    });

    WorldBakeTotalModels = PendingWorldBakeModels.Num();

    // The three world-state DAT transactions are complete. Reserve five percent for them and use
    // the remaining range for per-model metadata work, including cache-hit and skipped nodes.
    WorldBakeProgressValue = FMath::Max(WorldBakeProgressValue, 0.05f);
    OnWorldBakeProgress.Broadcast(WorldBakeProgressValue);

    LastSaveMessage = FString::Printf(
        TEXT("월드 DAT/SCZ Bake 시작: 상태 DAT 3개, 모델 SCZ %d개"),
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
    StartNextWorldBakeModel();
}

void UGameManagerSubSystem::StartNextWorldBakeModel()
{
    if (!bWorldBakeInProgress)
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        bWorldBakeStateFilesSaved = false;
        FinishWorldBake();
        return;
    }

    while (WorldBakeNextModelIndex < PendingWorldBakeModels.Num())
    {
        const FString ModelPath = PendingWorldBakeModels[WorldBakeNextModelIndex++];
        if (ModelPath.IsEmpty() || !IFileManager::Get().FileExists(*ModelPath))
        {
            ++WorldBakeCompletedModels;
            ++WorldBakeFailedModels;
            RefreshWorldBakeProgress();
            continue;
        }

        FActorSpawnParameters Params;
        Params.Owner = ConfigActor.Get();
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        UClass* BakeClass = SpawnActorClass ? SpawnActorClass.Get() : AglTFStreamActor::StaticClass();
        AglTFStreamActor* BakeActor = FActorHelper::SpawnActorDeferred<AglTFStreamActor>(
            World,
            BakeClass,
            FTransform::Identity,
            Params);
        if (!IsValid(BakeActor))
        {
            ++WorldBakeCompletedModels;
            ++WorldBakeFailedModels;
            RefreshWorldBakeProgress();
            continue;
        }

        ActiveWorldBakeActor = BakeActor;
        BakeActor->InitMetadataBake(ModelPath);
        BakeActor->OnModelSizeCacheBakeFinished.AddUObject(
            this,
            &UGameManagerSubSystem::HandleWorldBakeModelFinished);
        BakeActor->FinishSpawning(FTransform::Identity);
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

    const float ActiveModelProgress = IsValid(ActiveWorldBakeActor)
        ? FMath::Clamp(ActiveWorldBakeActor->GetLoadingStatus(), 0.0f, 1.0f)
        : 0.0f;
    const float ModelProgress = WorldBakeTotalModels > 0
        ? FMath::Clamp(
            (static_cast<float>(WorldBakeCompletedModels) + ActiveModelProgress) /
                static_cast<float>(WorldBakeTotalModels),
            0.0f,
            1.0f)
        : 1.0f;

    // Keep 100 percent reserved for FinishWorldBake, after the final DAT has been committed and
    // the active actor has released its parser and temporary mesh references.
    const float CalculatedProgress = FMath::Min(0.05f + ModelProgress * 0.95f, 0.999f);
    const float SafeProgress = FMath::Max(WorldBakeProgressValue, CalculatedProgress);
    if (!FMath::IsNearlyEqual(SafeProgress, WorldBakeProgressValue, KINDA_SMALL_NUMBER))
    {
        WorldBakeProgressValue = SafeProgress;
        OnWorldBakeProgress.Broadcast(WorldBakeProgressValue);
    }
}

void UGameManagerSubSystem::HandleWorldBakeModelFinished(AglTFStreamActor* BakeActor, bool bSuccess)
{
    if (!bWorldBakeInProgress || !IsValid(BakeActor) || BakeActor != ActiveWorldBakeActor.Get())
    {
        return;
    }

    BakeActor->OnModelSizeCacheBakeFinished.RemoveAll(this);
    ActiveWorldBakeActor = nullptr;
    BakeActor->ReleaseRuntimeResourcesForWorldExit();
    BakeActor->Destroy();

    ++WorldBakeCompletedModels;
    if (!bSuccess)
    {
        ++WorldBakeFailedModels;
    }

    RefreshWorldBakeProgress();
    StartNextWorldBakeModel();
}

void UGameManagerSubSystem::FinishWorldBake()
{
    if (!bWorldBakeInProgress)
    {
        return;
    }

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(WorldBakeProgressTimerHandle);
    }

    const bool bSuccess = bWorldBakeStateFilesSaved && WorldBakeFailedModels == 0;
    WorldBakeProgressValue = 1.0f;
    const FString Message = bSuccess
        ? FString::Printf(TEXT("월드 DAT/SCZ Bake 완료: 상태 DAT 3개, 모델 SCZ %d개"), WorldBakeTotalModels)
        : FString::Printf(
            TEXT("월드 DAT/SCZ Bake 완료(오류 있음): 상태 DAT=%s, 모델 SCZ 실패=%d/%d"),
            bWorldBakeStateFilesSaved ? TEXT("정상") : TEXT("실패"),
            WorldBakeFailedModels,
            WorldBakeTotalModels);

    bWorldBakeInProgress = false;
    PendingWorldBakeModels.Empty();
    ActiveWorldBakeActor = nullptr;
    WorldBakeNextModelIndex = 0;
    LastSaveMessage = Message;
    OnWorldBakeProgress.Broadcast(1.0f);
    OnWorldBakeCompleted.Broadcast(bSuccess, Message);
    NotifyStateChanged();
}

void UGameManagerSubSystem::CancelWorldBake()
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(WorldBakeProgressTimerHandle);
    }

    if (IsValid(ActiveWorldBakeActor))
    {
        ActiveWorldBakeActor->OnModelSizeCacheBakeFinished.RemoveAll(this);
        ActiveWorldBakeActor->ReleaseRuntimeResourcesForWorldExit();
        ActiveWorldBakeActor->Destroy();
    }

    ActiveWorldBakeActor = nullptr;
    PendingWorldBakeModels.Empty();
    bWorldBakeInProgress = false;
    bWorldBakeStateFilesSaved = false;
    WorldBakeTotalModels = 0;
    WorldBakeCompletedModels = 0;
    WorldBakeFailedModels = 0;
    WorldBakeNextModelIndex = 0;
    WorldBakeProgressValue = 0.0f;
}

bool UGameManagerSubSystem::LoadSavedScene()
{
    if (bSavedSceneLoaded || bSavedSceneLoadInProgress)
    {
        return false;
    }

    UWorld* World = GetWorld();
    if (World && World->GetNetMode() == NM_Client)
    {
        // Clients receive authoritative gameplay actors from the server.
        LastKnownSceneRecords.Empty();
        bSavedSceneLoaded = true;
        bSavedSceneLoadFailed = false;
        SavedSceneReadinessAttemptCount = 0;
        SavedSceneDataAttemptCount = 0;
        return true;
    }

    if (!World || !ConfigActor.IsValid() || GetWorldRootPath().IsEmpty()
        || !IsValid(UInstancedEntitySubsystem::Get(this)))
    {
        ++SavedSceneReadinessAttemptCount;
        ScheduleSavedSceneLoadRetry(
            TEXT("world or entity-instancing subsystem is not ready"),
            true);
        return false;
    }

    // The startup next-tick callback can run before world-model streaming has registered its shared
    // ISM assets. A synchronous prefab load at that point can fail even though both DAT and model are
    // valid. Leave the restoration node pending; LoadWorldAsync calls this again once streaming is ready.
    UglTFStreamSubSystem* ActiveStreamSubsystem = IsValid(StreamSubSystem)
        ? StreamSubSystem.Get()
        : UglTFStreamSubSystem::Get(this);
    if (IsValid(ActiveStreamSubsystem)
        && ActiveStreamSubsystem->IsActiveForWorld(World)
        && !ActiveStreamSubsystem->IsInitialWorldReady())
    {
        UE_LOG(LogTemp, VeryVerbose,
            TEXT("Entity DAT restore is waiting for initial world streaming. World=%s Path=%s"),
            *GetWorldRootPath(),
            *GetManifestPath());
        return false;
    }

    bSavedSceneLoadInProgress = true;
    SavedSceneReadinessAttemptCount = 0;
    World->GetTimerManager().ClearTimer(SavedSceneLoadRetryTimerHandle);

    const FString EntitiesDatPath = GetManifestPath();
    const FString EntitiesBackupPath = EntitiesDatPath + TEXT(".bak");
    const bool bPrimaryDatExists = IFileManager::Get().FileExists(*EntitiesDatPath);
    const bool bBackupDatExists = IFileManager::Get().FileExists(*EntitiesBackupPath);
    const bool bHadDatGeneration = bPrimaryDatExists || bBackupDatExists;

    TArray<FPlacedObjectRecord> Placed;
    FString LoadError;
    bool bLoaded = FBinaryDataStore::LoadEntities(EntitiesDatPath, Placed, LoadError);
    bool bRecoveredNonEmptyBackup = false;
    bool bMigratedLegacyJson = false;
    bool bHadLegacySource = false;

    // Older builds could commit an empty but structurally valid primary during EndPlay after the
    // entity actors had already been destroyed. Normal CRC fallback cannot identify that semantic
    // data loss, so recover a valid non-empty backup when the primary is empty.
    if (bLoaded && Placed.IsEmpty() && bPrimaryDatExists && bBackupDatExists)
    {
        TArray<FPlacedObjectRecord> BackupRecords;
        FString BackupError;
        if (FBinaryDataStore::LoadEntities(EntitiesBackupPath, BackupRecords, BackupError)
            && !BackupRecords.IsEmpty())
        {
            UE_LOG(LogTemp, Warning,
                TEXT("Recovered non-empty entities.dat.bak over an empty primary. Path=%s Records=%d"),
                *EntitiesDatPath,
                BackupRecords.Num());
            Placed = MoveTemp(BackupRecords);
            bRecoveredNonEmptyBackup = true;
        }
        else if (!BackupError.IsEmpty())
        {
            UE_LOG(LogTemp, Warning,
                TEXT("Empty primary entity DAT had no usable non-empty backup. Path=%s Reason=%s"),
                *EntitiesDatPath,
                *BackupError);
        }
    }

    // One-way compatibility migration. Legacy JSON is read-only and is never rewritten or backed up.
    // It may also rescue a non-empty legacy scene after an older build committed an empty DAT and
    // no usable non-empty DAT backup remains.
    if (!bLoaded || (Placed.IsEmpty() && !bRecoveredNonEmptyBackup))
    {
        const TArray<FString> LegacyPaths =
        {
            FPaths::Combine(GetWorldRootPath(), TEXT("entities.json")),
            FPaths::Combine(GetWorldRootPath(), TEXT("runtime_installed.json"))
        };
        FSafeJsonLimits Limits;
        Limits.MaxFileBytes = 64ll * 1024ll * 1024ll;
        Limits.MaxDepth = 32;
        Limits.MaxValues = 1000000;
        Limits.MaxContainerEntries = 100000;
        Limits.MaxStringCharacters = 32768;
        Limits.MaxPrimitiveCharacters = 128;
        Limits.bAllowBackupRecovery = false;

        for (const FString& LegacyPath : LegacyPaths)
        {
            bHadLegacySource |= IFileManager::Get().FileExists(*LegacyPath);
            const FSafeJsonLoadResult Legacy = FSafeFileIO::LoadJsonBlocking(LegacyPath, Limits);
            if (!Legacy.IsSuccess() || !Legacy.JsonObject.IsValid())
            {
                continue;
            }

            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            if (!Legacy.JsonObject->TryGetArrayField(TEXT("Objects"), Values) || !Values
                || Values->Num() > Limits.MaxContainerEntries)
            {
                continue;
            }

            TArray<FPlacedObjectRecord> Parsed;
            Parsed.Reserve(Values->Num());
            bool bAllValid = true;
            for (const TSharedPtr<FJsonValue>& Value : *Values)
            {
                FPlacedObjectRecord Record;
                if (!Value.IsValid() || Value->Type != EJson::Object
                    || !Record.FromJson(Value->AsObject()))
                {
                    bAllValid = false;
                    break;
                }

                // Value 1 was the removed generated-mesh format; only supported runtime entities migrate.
                if (Record.Kind == EPlacedObjectKind::Prefab || Record.Kind == EPlacedObjectKind::Vehicle)
                {
                    Parsed.Add(MoveTemp(Record));
                }
            }
            if (bAllValid && (!bLoaded || !Parsed.IsEmpty()))
            {
                Placed = MoveTemp(Parsed);
                bLoaded = true;
                bMigratedLegacyJson = true;
                UE_LOG(LogTemp, Log,
                    TEXT("Migrating legacy entity JSON to data/entities.dat. Source=%s Records=%d"),
                    *LegacyPath,
                    Placed.Num());
                break;
            }
        }
    }

    if (!bLoaded)
    {
        if (!bHadDatGeneration && !bHadLegacySource)
        {
            // A world with no saved entities is a valid empty scene, not a load failure.
            LastKnownSceneRecords.Empty();
            bSavedSceneLoadInProgress = false;
            bSavedSceneLoaded = true;
            bSavedSceneLoadFailed = false;
            SavedSceneReadinessAttemptCount = 0;
            SavedSceneDataAttemptCount = 0;
            UE_LOG(LogTemp, Display,
                TEXT("No entities.dat exists; continuing with an empty scene. World=%s Path=%s"),
                *GetWorldRootPath(),
                *EntitiesDatPath);
            NotifyStateChanged();
            return true;
        }

        const FString RetryReason = LoadError.IsEmpty()
            ? TEXT("entities.dat or legacy entity data could not be validated")
            : LoadError;
        ++SavedSceneDataAttemptCount;
        ScheduleSavedSceneLoadRetry(RetryReason, false);
        return false;
    }

    UE_LOG(LogTemp, Display,
        TEXT("Validated entity DAT for restoration. World=%s Path=%s Records=%d Attempt=%d/%d"),
        *GetWorldRootPath(),
        *EntitiesDatPath,
        Placed.Num(),
        SavedSceneDataAttemptCount + 1,
        MaxSavedSceneDataAttempts);

    FActorSpawnParameters Params;
    Params.Owner = ConfigActor.Get();
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    int32 SpawnedCount = 0;
    int32 MissingSourceCount = 0;
    int32 InvalidSourceCount = 0;
    int32 TransientSpawnFailureCount = 0;
    bool bNormalizedRecordChanged = bMigratedLegacyJson || bRecoveredNonEmptyBackup;
    TArray<FPlacedObjectRecord> NormalizedRecords = Placed;
    TArray<APrefabActor*> AttemptPrefabs;
    TArray<AVehiclePawn*> AttemptVehicles;
    AttemptPrefabs.Reserve(Placed.Num());
    AttemptVehicles.Reserve(Placed.Num());

    for (int32 RecordIndex = 0; RecordIndex < Placed.Num(); ++RecordIndex)
    {
        const FPlacedObjectRecord& Record = Placed[RecordIndex];
        const FString ResolvedSourceFile = ResolvePlacementSourcePath(Record.SourceFile, Record.Kind);
        if (ResolvedSourceFile.IsEmpty())
        {
            ++MissingSourceCount;
            UE_LOG(LogTemp, Error,
                TEXT("Saved entity source could not be resolved. Index=%d Kind=%d Name=%s Source=%s World=%s"),
                RecordIndex,
                static_cast<int32>(Record.Kind),
                *Record.ObjectName,
                *Record.SourceFile,
                *GetWorldRootPath());
            continue;
        }

        FString ValidationReason;
        if (!GlbValidation::ValidateRuntimeModelFile(ResolvedSourceFile, ValidationReason))
        {
            ++InvalidSourceCount;
            UE_LOG(LogTemp, Error,
                TEXT("Saved entity model failed validation. Index=%d Name=%s Source=%s Reason=%s"),
                RecordIndex,
                *Record.ObjectName,
                *ResolvedSourceFile,
                *ValidationReason);
            continue;
        }

        const FString PortableSource = MakePlacementSourcePathForSave(ResolvedSourceFile);
        if (NormalizedRecords.IsValidIndex(RecordIndex) && !PortableSource.IsEmpty()
            && !NormalizedRecords[RecordIndex].SourceFile.Equals(PortableSource, ESearchCase::CaseSensitive))
        {
            NormalizedRecords[RecordIndex].SourceFile = PortableSource;
            bNormalizedRecordChanged = true;
        }

        if (Record.Kind == EPlacedObjectKind::Prefab)
        {
            UClass* PrefabSpawnClass = PrefabActorClass ? PrefabActorClass.Get() : APrefabActor::StaticClass();
            APrefabActor* Prefab = World->SpawnActor<APrefabActor>(PrefabSpawnClass, Record.Transform, Params);
            if (!IsValid(Prefab) && PrefabSpawnClass != APrefabActor::StaticClass())
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("Configured prefab restore class failed; retrying native class. Class=%s Name=%s"),
                    *GetNameSafe(PrefabSpawnClass),
                    *Record.ObjectName);
                Prefab = World->SpawnActor<APrefabActor>(APrefabActor::StaticClass(), Record.Transform, Params);
            }
            if (IsValid(Prefab))
            {
                Prefab->SetRenderOnlyMode(UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this));
            }
            if (IsValid(Prefab) && Prefab->LoadPrefab(ResolvedSourceFile, Record.ObjectName))
            {
                AttemptPrefabs.Add(Prefab);
                ++SpawnedCount;
                UE_LOG(LogTemp, Display,
                    TEXT("Restored prefab entity. Index=%d Name=%s Source=%s"),
                    RecordIndex,
                    *Record.ObjectName,
                    *ResolvedSourceFile);
            }
            else
            {
                ++TransientSpawnFailureCount;
                UE_LOG(LogTemp, Warning,
                    TEXT("Prefab restoration attempt failed. Index=%d Name=%s Source=%s"),
                    RecordIndex,
                    *Record.ObjectName,
                    *ResolvedSourceFile);
                if (IsValid(Prefab))
                {
                    Prefab->Destroy();
                }
            }
        }
        else if (Record.Kind == EPlacedObjectKind::Vehicle)
        {
            UClass* VehicleSpawnClass = VehiclePawnClass ? VehiclePawnClass.Get() : AVehiclePawn::StaticClass();
            AVehiclePawn* Vehicle = World->SpawnActor<AVehiclePawn>(VehicleSpawnClass, Record.Transform, Params);
            if (!IsValid(Vehicle) && VehicleSpawnClass != AVehiclePawn::StaticClass())
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("Configured vehicle restore class failed; retrying native class. Class=%s Name=%s"),
                    *GetNameSafe(VehicleSpawnClass),
                    *Record.ObjectName);
                Vehicle = World->SpawnActor<AVehiclePawn>(AVehiclePawn::StaticClass(), Record.Transform, Params);
            }
            if (IsValid(Vehicle) && Vehicle->LoadVehicleModel(ResolvedSourceFile, Record.ObjectName))
            {
                AttemptVehicles.Add(Vehicle);
                ++SpawnedCount;
                UE_LOG(LogTemp, Display,
                    TEXT("Restored vehicle entity. Index=%d Name=%s Source=%s"),
                    RecordIndex,
                    *Record.ObjectName,
                    *ResolvedSourceFile);
            }
            else
            {
                ++TransientSpawnFailureCount;
                UE_LOG(LogTemp, Warning,
                    TEXT("Vehicle restoration attempt failed. Index=%d Name=%s Source=%s"),
                    RecordIndex,
                    *Record.ObjectName,
                    *ResolvedSourceFile);
                if (IsValid(Vehicle))
                {
                    Vehicle->Destroy();
                }
            }
        }
        else
        {
            ++InvalidSourceCount;
            UE_LOG(LogTemp, Error,
                TEXT("Unsupported entity kind in validated DAT. Index=%d Kind=%d"),
                RecordIndex,
                static_cast<int32>(Record.Kind));
        }
    }

    if (TransientSpawnFailureCount > 0)
    {
        ++SavedSceneDataAttemptCount;
        if (SavedSceneDataAttemptCount < MaxSavedSceneDataAttempts)
        {
            for (APrefabActor* Prefab : AttemptPrefabs)
            {
                if (IsValid(Prefab))
                {
                    Prefab->Destroy();
                }
            }
            for (AVehiclePawn* Vehicle : AttemptVehicles)
            {
                if (IsValid(Vehicle))
                {
                    Vehicle->Destroy();
                }
            }

            const FString RetryReason = FString::Printf(
                TEXT("%d entity model(s) failed to register after DAT validation"),
                TransientSpawnFailureCount);
            ScheduleSavedSceneLoadRetry(RetryReason, false);
            return false;
        }
    }

    for (APrefabActor* Prefab : AttemptPrefabs)
    {
        if (IsValid(Prefab))
        {
            SpawnedPrefabs.Add(TWeakObjectPtr<APrefabActor>(Prefab));
        }
    }
    for (AVehiclePawn* Vehicle : AttemptVehicles)
    {
        if (IsValid(Vehicle))
        {
            SpawnedVehicles.Add(TWeakObjectPtr<AVehiclePawn>(Vehicle));
        }
    }

    // Preserve every validated record, including temporarily unresolved records. This prevents any
    // later teardown save from deleting data that was present in the committed DAT generation.
    LastKnownSceneRecords = NormalizedRecords;

    // Normalize successfully resolved legacy/absolute paths and recommit a recovered non-empty
    // backup. The DAT transaction creates and validates its own .bak generation.
    if (bNormalizedRecordChanged)
    {
        const FSafeFileWriteResult NormalizeResult =
            FBinaryDataStore::SaveEntitiesBlocking(EntitiesDatPath, NormalizedRecords);
        if (!NormalizeResult.IsSuccess())
        {
            UE_LOG(LogTemp, Warning,
                TEXT("Loaded entities but could not normalize/recover entities.dat: %s"),
                *NormalizeResult.Error);
        }
        else
        {
            TArray<FPlacedObjectRecord> VerifiedNormalizedRecords;
            FString VerifyError;
            if (!FBinaryDataStore::LoadEntities(EntitiesDatPath, VerifiedNormalizedRecords, VerifyError)
                || !ArePlacedObjectRecordsEquivalent(NormalizedRecords, VerifiedNormalizedRecords))
            {
                UE_LOG(LogTemp, Error,
                    TEXT("Normalized entity DAT failed read-after-write verification. Path=%s Reason=%s"),
                    *EntitiesDatPath,
                    *VerifyError);
            }
            else
            {
                LastKnownSceneRecords = MoveTemp(VerifiedNormalizedRecords);
            }
        }
    }

    const bool bPartialApplyFailure = MissingSourceCount > 0
        || InvalidSourceCount > 0
        || TransientSpawnFailureCount > 0;
    bSavedSceneLoadInProgress = false;
    bSavedSceneLoaded = true;
    bSavedSceneLoadFailed = bPartialApplyFailure;
    SavedSceneReadinessAttemptCount = 0;
    SavedSceneDataAttemptCount = 0;
    if (bPartialApplyFailure)
    {
        LastSaveMessage = TEXT("entities.dat은 로드했지만 일부 엔티티를 복원하지 못해 기존 DAT 보호를 위해 자동 저장을 잠갔습니다.");
        UE_LOG(LogTemp, Error,
            TEXT("Entity DAT was only partially applied; scene saving is locked. Path=%s Records=%d Spawned=%d Missing=%d Invalid=%d SpawnFailed=%d"),
            *EntitiesDatPath,
            Placed.Num(),
            SpawnedCount,
            MissingSourceCount,
            InvalidSourceCount,
            TransientSpawnFailureCount);
    }
    else
    {
        LastSaveMessage = FString::Printf(TEXT("엔티티 복원 완료: %d개"), SpawnedCount);
        UE_LOG(LogTemp, Display,
            TEXT("Entity DAT fully applied. World=%s Path=%s Records=%d Spawned=%d"),
            *GetWorldRootPath(),
            *EntitiesDatPath,
            Placed.Num(),
            SpawnedCount);
    }
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

FString UGameManagerSubSystem::BuildStatusText() const
{
    const FString ModeString = StaticEnum<EToolMode>()->GetDisplayNameTextByValue(
        static_cast<int64>(CurrentMode)).ToString();
    const FToolbarItem SelectedItem = GetSelectedToolbarItem();
    const FString SelectedItemName = SelectedItem.DisplayName.IsEmpty()
        ? TEXT("비어 있음")
        : SelectedItem.DisplayName;
    const FString CrosshairPlacementText = !bLastTraceHasPlacementLocation
        ? TEXT("NONE")
        : (bLastTraceBlockingHit ? TEXT("SURFACE") : TEXT("AIR / FREE-SPACE"));

    return FString::Printf(
        TEXT("[Creator Toolbar]\nMode: %s | PlayMode: %s\nToolbar Slot: %d / 7 | Item: %s (%s)\nInventory Window: %s | Available Items: %d\nSnap: %s / Grid %.0f cm\nCrosshair: X %.0f Y %.0f Z %.0f | Placement: %s | Collision %.0f cm / Max %.0f cm\nControls: MouseWheel=toolbar slot, E=item list, LMB=place/fire, F=enter/exit vehicle, SnapAction/G=toggle snap\nWorld: %s\nData: %s\n%s"),
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
        *GetDataDirectory(),
        *LastSaveMessage);
}

FString UGameManagerSubSystem::BuildHUDText() const
{
    return BuildStatusText();
}
