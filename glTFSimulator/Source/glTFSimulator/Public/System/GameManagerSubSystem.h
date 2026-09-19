// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file GameManagerSubSystem.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Templates/Atomic.h"
#include "Engine/HitResult.h"
#include "GameFramework/Actor.h"
#include "Model/glTFMaterialAssetReferences.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TimerManager.h"
#include "World/PlacementTypes.h"
#include "System/ProjectConfig.h"
#include "System/WorldArchive.h"
#include "GameManagerSubSystem.generated.h"

class ADynamicActor;
class AVehiclePawn;
class AWeaponActor;
class UCameraComponent;
class UMaterialInterface;
class UMaterialDefaultRuntimeCache;
class UProceduralMeshComponent;
class USceneComponent;
class UWorldData;
class UPlayerData;
class UUserWidget;
class AWorldEnvManager;
class AStaticActor;
class UWorldSceneStreamingSubsystem;
class UWorldSourceModelBuilder;
class UGameSettings;
class UPostProcessComponent;
class AGlTFSimulatorGameplayGameModeBase;
class UWorld;
class APlayerController;

UENUM(BlueprintType)
enum class EToolMode : uint8
{
    None = 0 UMETA(DisplayName="None"),
    PlaceStatic = 1 UMETA(DisplayName="Place Static"),
    PlaceVehicle = 2 UMETA(DisplayName="Place Vehicle"),
    Weapon = 3 UMETA(DisplayName="Weapon")
};

UENUM(BlueprintType)
enum class EPlayMode : uint8
{
    Creator UMETA(DisplayName="Creator Mode"),
    RealLife UMETA(DisplayName="Real Life Mode")
};

UENUM(BlueprintType)
enum class EToolbarItemKind : uint8
{
    None = 0 UMETA(DisplayName="None"),
    Static = 1 UMETA(DisplayName="Static"),
    Weapon = 2 UMETA(DisplayName="Weapon"),
    Vehicle = 3 UMETA(DisplayName="Vehicle")
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FToolbarItem
{
    GENERATED_BODY()

    UPROPERTY(Transient)
    EToolbarItemKind Kind = EToolbarItemKind::None;

    UPROPERTY(Transient)
    FString DisplayName;

    UPROPERTY(Transient)
    FString ModelReference;

    UPROPERTY(Transient)
    int32 ModelIndex = INDEX_NONE;

    UPROPERTY(Transient)
    bool bAvailable = false;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FGameStateChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FToolbarChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FGameMessageChanged, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FItemListWindowChanged, bool, bOpen);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FWorldBakeProgress, float, Progress);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FWorldBakeCompleted, bool, bSuccess, const FString&, Message);

UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UGameManagerSubSystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    UGameManagerSubSystem();

    /** GC-visible copy of the active manager's Static actor class; used by the distance streamer. */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Game|Classes")
    TSubclassOf<AStaticActor> StaticActorClass;

    /** Validated external folder actually used by startup. Empty means no valid selection. */
    UFUNCTION(BlueprintPure, Category="Game|World")
    FString GetSelectedWorldRootPath() const { return GetWorldRootPath(); }

    bool IsActiveGameMode(const AGlTFSimulatorGameplayGameModeBase* GameMode) const;

    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    static UGameManagerSubSystem* GetSubSystem(UWorld* InWorld);
    static UGameManagerSubSystem* GetSubSystem(AActor* InActor);
    static UGameManagerSubSystem* GetSubSystem(const UObject* WorldContextObject);
    UFUNCTION(BlueprintPure, Category="Game", meta=(WorldContext="WorldContextObject"))
    static UGameManagerSubSystem* FindGameManager(const UObject* WorldContextObject);

    /**
     * Returns the single resolved material set owned by the game system. The returned reference is
     * valid for the current game-thread call and remains GC-safe because every UObject is held by a
     * UPROPERTY on this subsystem. Missing/invalid assets produce an empty set rather than a crash.
     */
    const FglTFMaterialAssetReferences& GetMaterialDefaultReferences();

    /**
     * Returns the shared request-lifetime GC guard used by asynchronous glTFRuntime operations.
     * Call only on the game thread. The returned object may outlive the active world when a native
     * callback is still draining, but it never retains actors, worlds, parsers, or components.
     */
    UMaterialDefaultRuntimeCache* AcquireMaterialDefaultReferenceGuard();

    UFUNCTION(BlueprintPure, Category="Game|Assets")
    bool IsMaterialDefaultAssetReady() const { return bMaterialDefaultAssetResolved; }

    // Returns gameplay to the single MainWorld and asks MainGameMode to show world selection after travel.
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle", meta=(WorldContext="WorldContextObject"))
    static void OpenWorldSelectionScreen(const UObject* WorldContextObject, TSoftObjectPtr<UWorld> MainWorld);

    /** Native request path used by pause UI. True means this or an equivalent duplicate request owns travel. */
    static bool TryOpenWorldSelectionScreen(const UObject* WorldContextObject, TSoftObjectPtr<UWorld> MainWorld);

    /** Rolls back an accepted request when OpenLevel fails and the gameplay world remains active. */
    void CancelWorldSelectionMenuTravel();

    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle")
    void StartGameplaySession(AGlTFSimulatorGameplayGameModeBase* InGameMode);

    /** Client-side render/session initialization. GameMode exists only on authority, so clients use their PlayerController as lifetime owner. */
    void StartClientGameplaySession(APlayerController* InOwnerController);

    /** Stops the active session. A non-null requester may stop only the session it owns. */
    void StopGameplaySession(
        const EEndPlayReason::Type EndPlayReason,
        const AGlTFSimulatorGameplayGameModeBase* RequestingGameMode = nullptr);
    void UpdateGameManager(float DeltaSeconds);
    void ApplyGameModeConfig(const AGlTFSimulatorGameplayGameModeBase* InGameMode);

    UFUNCTION(BlueprintCallable, Category="Game|Settings")
    void SaveSettings();
    UFUNCTION(BlueprintCallable, Category="Game|Settings")
    void UpdateSettings();
    UFUNCTION(BlueprintCallable, Category="Game|Pause")
    void TogglePause();
    UFUNCTION(BlueprintCallable, Category="Game|Pause")
    void SetGamePaused(bool bPaused);

    /** Registers or replaces the loading widget. Missing UI is created automatically from the central registry. */
    UFUNCTION(BlueprintCallable, Category="Game|Loading")
    void SetLoadingWidget(UUserWidget* InWidget);

    bool HasLoadingWidget() const;

    UFUNCTION(BlueprintCallable, Category="Game|Loading")
    void SetWorldLoading(bool bLoading);
    UFUNCTION(BlueprintPure, Category="Game|Loading")
    bool IsWorldLoading() const { return bIsWorldLoading; }

    /**
     * Registers the active character. During initial world-state restoration this also completes the one-shot
     * saved-transform/PlayerStart handshake; later pawn replacements only update the reference.
     */
    void SetPlayerActor(AActor* Actor);
    /** Reasserts a saved initial view rotation after GameMode finishes possessing the first player. */
    void ApplyPendingInitialPlayerControlRotation(APlayerController* Controller);
    void SetCameraComponent(USceneComponent* InCamera);
    void SetGameSettings(UGameSettings* Settings) { GameSettings = Settings; }
    void SetWorldData(UWorldData* Data) { CurrentWorldData = Data; }
    /**
     * Stores a direct child key beneath PATH_WORLDS. Empty input deliberately clears the hand-off;
     * malformed input is rejected so URL/replication data can never become an arbitrary path.
     */
    UFUNCTION(BlueprintCallable, Category="Game|World")
    void SetCurrentWorldName(FString Name);
    /**
     * Canonical validator shared by menu travel, replicated state, and destination initialization.
     * Set bRequireExistingDirectory only at the point where a world is about to be opened.
     */
    static bool TryNormalizeWorldFolderName(
        const FString& Candidate,
        FString& OutNormalized,
        bool bRequireExistingDirectory = false);
    /** Accepts runtime location updates only from the registered primary player actor. */
    void SetPlayerLocation(const FVector& Location, const AActor* SourceActor);
    void SetPostProcess(UPostProcessComponent* InPostProcess) { PostProcess = InPostProcess; }
    template <typename T> T* GetPlayerActor() const { return Cast<T>(PlayerActor.Get()); }
    template <typename T> T* GetCameraComponent() const { return Cast<T>(CurrentCamera.Get()); }
    UFUNCTION(BlueprintPure, Category="SettingData")
    UGameSettings* GetGameSettings() const { return GameSettings.Get(); }
    FVector GetPlayerLocation() const { return PlayerLocation; }
    FVector GetCameraLocation() const { return IsValid(CurrentCamera.Get()) ? CurrentCamera->GetComponentLocation() : FVector::ZeroVector; }
    bool GetGamePaused() const { return bIsGamePaused; }
    UWorldData* GetWorldData() const { return CurrentWorldData.Get(); }

    /** Authority-side command helpers shared by console and the future chat command path. */
    bool SetWorldTimeSeconds(double Seconds);
    bool AddWorldTimeSeconds(double DeltaSeconds);
    bool SetWorldDay(double DayNumber);
    UFUNCTION(BlueprintPure, Category="Game|Player")
    UPlayerData* GetPlayerData() const { return ActivePlayerData.Get(); }
    UFUNCTION(BlueprintPure, Category="Game|Weapon")
    AWeaponActor* GetEquippedWeaponActor() const { return EquippedWeapon.Get(); }
    UFUNCTION(BlueprintPure, Category="Game|Level")
    bool AreCheatsEnabledForCurrentLevel() const { return bCurrentLevelCheatsEnabled; }
    FString GetCurrentWorldName() const { return CurrentWorldName; }
    static void ToggleFullscreen();
    void SetLoadingStatus(float InValue) { LoadingStatus = FMath::Clamp(InValue, 0.0f, 1.0f); }
    UFUNCTION(BlueprintCallable, Category="Game|Loading")
    float GetLoadingStatus() const { return LoadingStatus; }
    UFUNCTION(BlueprintPure, Category="Game|Debug")
    FString GetDebugText();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SelectPreviousStatic();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SelectNextStatic();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SelectStaticPlacementTool();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SelectVehicleTool();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SelectPreviousWeapon();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SelectNextWeapon();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void EquipCurrentWeapon();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void ToggleSnap();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SetSnapEnabled(bool bEnabled);

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SetGridSize(float NewGridSize);

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void ToggleFirstPerson();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    bool SaveScene();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    bool LoadSavedScene();

    /** Called by the chunk streamer after a UUID-backed Static/Dynamic object is instantiated. */
    void TrackStreamedWorldObject(AActor* Actor);

    /** Creates Projects/<Name>/config.json and Projects/<Name>/resources atomically enough for immediate discovery. */
    UFUNCTION(BlueprintCallable, Category="Game|Projects")
    bool CreateProjectByName(const FString& ProjectName);

    /** Scans Projects/<Project>/resources and publishes either a World .gworld or an external .gasset pack. */
    UFUNCTION(BlueprintCallable, Category="Game|Projects")
    bool BuildProjectByName(const FString& ProjectName);

    UFUNCTION(BlueprintPure, Category="Game|Projects")
    static FString GetProjectsRootPath();

    /** Returns only valid Projects/<Folder>/config.json + resources/ authoring projects. */
    UFUNCTION(BlueprintCallable, Category="Game|Projects")
    void GetProjectSummaries(TArray<FGlTFSimulatorProjectSummary>& OutProjects) const;

    /** Runtime folder used for installable Character/Dynamic .gasset packs. */
    UFUNCTION(BlueprintPure, Category="Game|Projects")
    static FString GetExternalResourcesRootPath();

    UFUNCTION(BlueprintCallable, Category="Game|Projects")
    bool GetProjectConfigurationByName(
        const FString& ProjectName,
        EGlTFSimulatorProjectType& OutProjectType,
        bool& bOutAllowExternalAssets,
        FString& OutDisplayName) const;

    UFUNCTION(BlueprintCallable, Category="Game|Projects")
    bool SetProjectTypeByName(const FString& ProjectName, EGlTFSimulatorProjectType ProjectType);

    UFUNCTION(BlueprintCallable, Category="Game|Projects")
    bool SetWorldExternalAssetsAllowedByName(const FString& ProjectName, bool bAllowed);

    /** Atomically builds immutable WorldName.gworld from the explicitly prepared authoring database. */
    UFUNCTION(BlueprintCallable, Category="Game|Bake")
    void BakeWorldData();

    UFUNCTION(BlueprintPure, Category="Game|Bake")
    bool IsWorldBakeInProgress() const { return bWorldBakeInProgress; }

    /** Includes project validation/indexing before the source-model bake flag becomes true. */
    UFUNCTION(BlueprintPure, Category="Game|Bake")
    bool IsProjectBuildInProgress() const
    {
        return bWorldBakeInProgress || bWorldArchiveCommitInFlight || !ActiveBuildProjectRoot.IsEmpty();
    }

    UFUNCTION(BlueprintPure, Category="Game|Bake")
    float GetWorldBakeProgress() const { return WorldBakeProgressValue; }

    UFUNCTION(BlueprintPure, Category="Game|Bake")
    int32 GetWorldBakeTotalModels() const { return WorldBakeTotalModels; }

    UFUNCTION(BlueprintPure, Category="Game|Bake")
    int32 GetWorldBakeCompletedModels() const { return WorldBakeCompletedModels; }

    /** Updates the selected player runtime record and persists it to WorldName.dat. */
    UFUNCTION(BlueprintCallable, Category="Game|World Data")
    void SetSelectedPlayerForRuntime(const FString& PlayerId);

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void RefreshAssetLists();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SetCurrentToolMode(EToolMode NewMode);

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    bool SetCurrentStaticIndex(int32 NewIndex);

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    bool SetCurrentWeaponIndex(int32 NewIndex);

    /** Left mouse pressed. Executes the selected placement/equipment action once. */
    UFUNCTION(BlueprintCallable, Category="Game|Input")
    void InputPrimaryPressed();

    /** Secondary action endpoint retained for project input mappings. */
    UFUNCTION(BlueprintCallable, Category="Game|Input")
    void InputSecondaryAction();

    UFUNCTION(BlueprintCallable, Category="Game|Input")
    void InputInteractAction();

    UFUNCTION(BlueprintCallable, Category="Game|Input")
    void InputToggleFirstPersonAction();

    UFUNCTION(BlueprintCallable, Category="Game|Input")
    void InputToolbarScrollAction(float ScrollValue);

    UFUNCTION(BlueprintCallable, Category="Game|Input")
    void InputToggleItemListAction();

    UFUNCTION(BlueprintCallable, Category="Game|Input")
    void InputToggleSnapModeAction();

    /** Optional vehicle input path. X = steering, Y = throttle. */
    UFUNCTION(BlueprintCallable, Category="Game|Input")
    void InputVehicleMoveAction(const FVector2D& MoveValue);

    UFUNCTION(BlueprintCallable, Category="Game|Input")
    void InputVehicleThrottleAction(float Throttle);

    UFUNCTION(BlueprintCallable, Category="Game|Input")
    void InputVehicleSteeringAction(float Steering);

    /** Blueprint aliases for the currently selected placement action. */
    UFUNCTION(BlueprintCallable, Category="Game|Placement")
    void SelectCurrentTraceLocation();

    UFUNCTION(BlueprintCallable, Category="Game|Placement")
    void ConfirmCurrentPendingLocation();

    UFUNCTION(BlueprintPure, Category="Game|Status")
    FString BuildStatusText() const;

    UFUNCTION(BlueprintPure, Category="Game|Status")
    FString BuildHUDText() const;

    UFUNCTION(BlueprintPure, Category="Game|Status")
    FString GetLastMessage() const { return LastSaveMessage; }

    UFUNCTION(BlueprintPure, Category="Game|Toolbar")
    int32 GetToolbarSlotCount() const { return 7; }

    UFUNCTION(BlueprintPure, Category="Game|Toolbar")
    int32 GetSelectedToolbarSlotIndex() const { return SelectedToolbarSlotIndex; }

    UFUNCTION(BlueprintPure, Category="Game|Toolbar")
    FToolbarItem GetToolbarItemAtSlot(int32 SlotIndex) const;

    UFUNCTION(BlueprintPure, Category="Game|Toolbar")
    FToolbarItem GetSelectedToolbarItem() const;

    UFUNCTION(BlueprintCallable, Category="Game|Toolbar")
    bool SelectToolbarSlot(int32 SlotIndex);

    UFUNCTION(BlueprintCallable, Category="Game|Toolbar")
    void ScrollToolbarSelection(float ScrollValue);

    UFUNCTION(BlueprintCallable, Category="Game|Toolbar")
    bool SetToolbarSlotFromAvailableItem(int32 SlotIndex, int32 AvailableItemIndex);

    UFUNCTION(BlueprintCallable, Category="Game|Toolbar")
    bool SelectAvailableItemForCurrentToolbarSlot(int32 AvailableItemIndex, bool bCloseItemList = true);

    UFUNCTION(BlueprintPure, Category="Game|Inventory")
    int32 GetAvailableItemCount() const { return AvailableItems.Num(); }

    UFUNCTION(BlueprintPure, Category="Game|Inventory")
    FToolbarItem GetAvailableItemAtIndex(int32 Index) const;

    UFUNCTION(BlueprintCallable, Category="Game|Inventory")
    void ToggleItemListWindow();

    UFUNCTION(BlueprintCallable, Category="Game|Inventory")
    void SetItemListWindowOpen(bool bOpen);

    UFUNCTION(BlueprintPure, Category="Game|Inventory")
    bool IsItemListWindowOpen() const { return bItemListWindowOpen; }

    UFUNCTION(BlueprintCallable, Category="Game|Mode")
    void SetPlayMode(EPlayMode NewMode);

    UFUNCTION(BlueprintPure, Category="Game|Mode")
    EPlayMode GetPlayMode() const { return PlayMode; }

    UFUNCTION(BlueprintPure, Category="Game|Status")
    FString GetCurrentStaticName() const;

    UFUNCTION(BlueprintPure, Category="Game|Status")
    FString GetCurrentWeaponName() const;

    UFUNCTION(BlueprintPure, Category="Game|Assets")
    int32 GetStaticCount() const { return StaticReferences.Num(); }

    UFUNCTION(BlueprintPure, Category="Game|Assets")
    int32 GetWeaponCount() const { return WeaponReferences.Num(); }

    UFUNCTION(BlueprintPure, Category="Game|Assets")
    int32 GetCurrentStaticIndex() const { return CurrentStaticIndex; }

    UFUNCTION(BlueprintPure, Category="Game|Assets")
    int32 GetCurrentWeaponIndex() const { return CurrentWeaponIndex; }

    UFUNCTION(BlueprintPure, Category="Game|Assets")
    FString GetStaticNameAtIndex(int32 Index) const;

    UFUNCTION(BlueprintPure, Category="Game|Assets")
    FString GetWeaponNameAtIndex(int32 Index) const;

    UFUNCTION(BlueprintPure, Category="Game|Assets")
    FString GetStaticReferenceAtIndex(int32 Index) const;

    UFUNCTION(BlueprintPure, Category="Game|Assets")
    FString GetWeaponReferenceAtIndex(int32 Index) const;

    UFUNCTION(BlueprintPure, Category="Game|Status")
    bool IsSnapEnabled() const { return bSnapToGrid; }

    UFUNCTION(BlueprintPure, Category="Game|Status")
    float GetGridSize() const { return GridSize; }

    UFUNCTION(BlueprintPure, Category="Game|Status")
    EToolMode GetCurrentToolMode() const { return CurrentMode; }

    UFUNCTION(BlueprintPure, Category="Game|Placement")
    FVector GetPreviewPlacementLocation() const { return LastPreviewLocation; }

    UFUNCTION(BlueprintPure, Category="Game|Placement")
    FVector GetCurrentCrosshairWorldLocation() const { return LastPreviewLocation; }

    UFUNCTION(BlueprintPure, Category="Game|Placement")
    bool HasCrosshairBlockingHit() const { return bLastTraceBlockingHit; }

    UFUNCTION(BlueprintPure, Category="Game|Placement")
    bool HasCrosshairPlacementLocation() const { return bLastTraceHasPlacementLocation; }

    UFUNCTION(BlueprintPure, Category="Game|Placement")
    bool IsCrosshairFreeSpacePlacement() const { return bLastTraceHasPlacementLocation && bLastTraceUsedFreeSpace; }

    UFUNCTION(BlueprintPure, Category="Game|Placement")
    AActor* GetCrosshairHitActor() const;

    /** Starts gameplay-owned model streaming and optional ocean actor creation. */
    UFUNCTION(BlueprintCallable, Category="Game|World")
    void InitializeWorldSystems(UWorldData* InWorldData, const FString& InWorldRoot, const FString& InInitialPlayerName);

    /** Stops world streaming and clears transient world actors created by this manager. */
    UFUNCTION(BlueprintCallable, Category="Game|World")
    void StopWorldSystems();

    /** Releases runtime main-world actors/assets that can otherwise survive a level transition through GameInstance subsystems. */
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle")
    void ReleaseMainWorldRuntimeMemory(bool bForceGarbageCollection = true);

    UFUNCTION(BlueprintPure, Category="Game|Lifecycle")
    bool HasPendingMainWorldRuntimePurge() const { return bPendingMainWorldRuntimePurge; }

    /** Requests that MainWorld opens directly on the world-selection widget after the next level travel. */
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle")
    void RequestWorldSelectionMenuOnNextMainWorld();

    /** Consumes and clears the pending MainWorld world-selection request. */
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle")
    bool ConsumeWorldSelectionMenuRequest();

    /** Clears the pending MainWorld world-selection request. */
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle")
    void ClearWorldSelectionMenuRequest();

    UFUNCTION(BlueprintPure, Category="Game|Lifecycle")
    bool ShouldOpenWorldSelectionMenuOnNextMainWorld() const { return bOpenWorldSelectionMenuOnNextMainWorld; }


    /** Returns the gameplay-owned world data object that drives time, sky, player position, and save data. */
    UFUNCTION(BlueprintPure, Category="Game|World")
    UWorldData* GetActiveWorldData() const { return ActiveWorldData.Get(); }

    /** Returns true when initial .gworld model ranges and the player replacement are ready. */
    UFUNCTION(BlueprintPure, Category="Game|World")
    bool AreWorldSystemsReady() const;

    /** Returns the loading percent reported by the built-world stream subsystem. */
    UFUNCTION(BlueprintPure, Category="Game|World")
    float GetWorldSystemsLoadingStatus() const;

    UPROPERTY(BlueprintAssignable, Category="Game|Events")
    FGameStateChanged OnStateChanged;

    UPROPERTY(BlueprintAssignable, Category="Game|Events")
    FGameMessageChanged OnMessageChanged;

    UPROPERTY(BlueprintAssignable, Category="Game|Events")
    FToolbarChanged OnToolbarChanged;

    UPROPERTY(BlueprintAssignable, Category="Game|Events")
    FItemListWindowChanged OnItemListWindowChanged;

    UPROPERTY(BlueprintAssignable, Category="Game|Bake")
    FWorldBakeProgress OnWorldBakeProgress;

    UPROPERTY(BlueprintAssignable, Category="Game|Bake")
    FWorldBakeCompleted OnWorldBakeCompleted;

protected:

private:
    UPROPERTY()
    TObjectPtr<AActor> PlayerActor;
    UPROPERTY()
    TObjectPtr<USceneComponent> CurrentCamera;
    UPROPERTY()
    TObjectPtr<UGameSettings> GameSettings;
    UPROPERTY()
    TObjectPtr<UPostProcessComponent> PostProcess;
    UPROPERTY()
    TObjectPtr<UWorldData> CurrentWorldData;
    bool bIsGamePaused = false;
    bool bIsWorldLoading = false;
    FString CurrentWorldName;
    FString PendingWorldConfigJson;
    FString ActiveBuildProjectRoot;
    FString ActiveBuildProjectName;
    EGlTFSimulatorProjectType ActiveBuildProjectType = EGlTFSimulatorProjectType::World;
    bool bActiveBuildAllowExternalAssets = false;
    FVector PlayerLocation = FVector::ZeroVector;
    float LoadingStatus = 0.0f;
    int32 TotalSumFPS = 0;
    int32 TotalCountFPS = 0;
    FString GetHardwareInfoText(FString InString);
    FString GetFramerateInfoText(FString InString);


    UPROPERTY(Transient)
    TWeakObjectPtr<AGlTFSimulatorGameplayGameModeBase> ConfigGameMode;

    /** Game-thread session owner: authority GameMode or client PlayerController. */
    TWeakObjectPtr<AActor> SessionOwner;

    FTimerManager& GetWorldTimerManager() const;
    FVector GetSessionOwnerLocation() const;
    void EnsureRuntimeComponents();
    bool ResolveMaterialDefaultAsset();
    void ReleaseMaterialDefaultAsset();

    UPROPERTY(Transient)
    TObjectPtr<USceneComponent> Root;

    UPROPERTY(Transient)
    TObjectPtr<UProceduralMeshComponent> PlacementGridComponent;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInterface> PlacementGridMaterial;



    /**
     * The one shared strong-reference object for the active world. Async requests retain this guard
     * in addition to their plugin-required request-local maps, preventing GC races during native callbacks.
     */
    UPROPERTY(Transient)
    TObjectPtr<UMaterialDefaultRuntimeCache> MaterialDefaultRuntimeCache;

    bool bMaterialDefaultAssetResolved = false;
    bool bMaterialDefaultAssetResolving = false;

    UPROPERTY(Transient)
    float PlacementGridSpacing = 100.0f;

    UPROPERTY(Transient)
    float PlacementGridLineThickness = 0.55f;

    UPROPERTY(Transient)
    float PlacementGridMaxRadius = 300.0f;

    // The grid is intentionally minimal: only center axes plus small 1m ticks, fading by 3 cells.
    UPROPERTY(Transient)
    float PlacementGridStrongRadius = 100.0f;

    UPROPERTY(Transient)
    float PlacementGridFadeRadius = 300.0f;

    UPROPERTY(Transient)
    TSubclassOf<ADynamicActor> DynamicActorClass;

    UPROPERTY(Transient)
    TSubclassOf<AVehiclePawn> VehiclePawnClass;

    UPROPERTY(Transient)
    TSubclassOf<AWeaponActor> WeaponActorClass;

    // GameManager owns the world boot sequence so WorldEnvManager can stay rendering-only.
    UPROPERTY(Transient)
    TSubclassOf<AWorldEnvManager> WorldEnvManagerClass;

    UPROPERTY(Transient)
    TSubclassOf<AActor> WaterClass;

    UPROPERTY(Transient)
    TSubclassOf<AActor> RainWeatherActorClass;

    UPROPERTY(Transient)
    FTransform OceanTransform;

    UPROPERTY()
    TObjectPtr<UWorldData> ActiveWorldData;

    UPROPERTY()
    TObjectPtr<UPlayerData> ActivePlayerData;

    UPROPERTY()
    TObjectPtr<AWorldEnvManager> WorldEnvManagerActor;

    UPROPERTY()
    TObjectPtr<AActor> OceanActor;

    UPROPERTY()
    TObjectPtr<UWorldSceneStreamingSubsystem> StreamSubSystem;

    UPROPERTY()
    TObjectPtr<UUserWidget> LoadingWidgetInstance;

    bool bManagerStarted = false;
    bool bRuntimeWorldStateInitialized = false;
    bool bWorldLoadCompleted = false;
    bool bSpawnedWorldEnvManager = false;
    bool bPendingMainWorldRuntimePurge = false;
    bool bOpenWorldSelectionMenuOnNextMainWorld = false;
    /** Separate from the destination-menu request so travel state cannot be applied twice. */
    bool bWorldSelectionMenuTravelInProgress = false;
    /** Distinguishes a real same-world duplicate from a stale GameInstance-level guard. */
    TWeakObjectPtr<UWorld> WorldSelectionTravelSourceWorld;
    /** Restored only when the travel watchdog proves that the old gameplay world never left. */
    FString WorldNameBeforeMenuTravel;
    bool bMenuTravelStatePrepared = false;
    bool bMenuTravelSaveCompleted = false;
    bool bCurrentLevelCheatsEnabled = false;
    FString ActivePlayerId = TEXT("Player");
    FDelegateHandle PostLoadMapCleanupHandle;

    /** Identifies why an initial location is trustworthy; FVector::ZeroVector is a valid saved value. */
    enum class EInitialPlayerLocationSource : uint8
    {
        None,
        EntityArchive
    };

    EInitialPlayerLocationSource InitialPlayerLocationSource = EInitialPlayerLocationSource::None;
    FRotator LoadedInitialPlayerRotation = FRotator::ZeroRotator;
    bool bHasLoadedInitialPlayerRotation = false;
    bool bInitialPlayerDataLoadCompleted = false;
    bool bInitialPlayerTransformResolved = false;
    bool bPendingInitialControlRotation = false;
    bool bPendingInitialWorldDataSave = false;
    bool bPendingInitialPlayerDataSave = false;

    // Runtime copy of the configured center-crosshair trace distance.
    UPROPERTY(Transient)
    float PlacementTraceDistance = 1000.0f;

    // Only this short distance is checked for blocking collision under the center crosshair.
    // If no blocking hit is found in this range, the cursor can still resolve to a free-space point.
    UPROPERTY(Transient)
    float CrosshairCollisionTraceDistance = 1000.0f;

    // Hard cap for free-space placement when the collision trace does not hit anything.
    // 1000 cm is 10 meters in Unreal units.
    UPROPERTY(Transient)
    float FreeSpacePlacementDistance = 1000.0f;

    // When true, a missed collision trace becomes a valid air placement point at FreeSpacePlacementDistance.
    UPROPERTY(Transient)
    bool bAllowFreeSpacePlacement = true;

    UPROPERTY(Transient)
    float GridSize = 100.0f;

    UPROPERTY(Transient)
    float SurfacePlacementOffset = 2.0f;

    UPROPERTY(Transient)
    float VehicleEnterDistance = 450.0f;

    /** Periodically saves runtime placed Static objects and vehicles. */
    UPROPERTY(Transient)
    bool bAutoSaveScene = true;

    UPROPERTY(Transient)
    float SceneAutoSaveIntervalSeconds = 60.0f;

    /** Saves entities one last time when this manager leaves the world. */
    UPROPERTY(Transient)
    bool bSaveSceneOnEndPlay = true;

    UPROPERTY(Transient)
    EPlayMode PlayMode = EPlayMode::Creator;

    UPROPERTY()
    TObjectPtr<AWeaponActor> EquippedWeapon;

    /** Non-owning tracking only; the UWorld owns actor lifetime. */
    UPROPERTY(Transient)
    TArray<TWeakObjectPtr<AStaticActor>> SpawnedStatics;

    UPROPERTY(Transient)
    TArray<TWeakObjectPtr<AVehiclePawn>> SpawnedVehicles;

    /** Opaque gworld:// keys copied from the verified archive directory; never source filenames. */
    TArray<FString> StaticReferences;
    TArray<FString> VehicleReferences;
    TArray<FString> WeaponReferences;
    TArray<FToolbarItem> AvailableItems;
    TArray<FToolbarItem> ToolbarSlots;
    int32 SelectedToolbarSlotIndex = 0;
    int32 CurrentStaticIndex = 0;
    int32 CurrentWeaponIndex = 0;
    EToolMode CurrentMode = EToolMode::None;
    bool bSnapToGrid = false;
    bool bFirstPerson = false;
    bool bItemListWindowOpen = false;
    bool bToolbarInitialized = false;
    FVector LastPreviewLocation = FVector::ZeroVector;
    FString LastSaveMessage;
    /** True after the initial-radius .dat chunks have been validated and applied. */
    bool bSavedSceneLoaded = false;
    bool bSavedSceneLoadInProgress = false;
    /** Records an initial chunk validation failure without modifying its committed generation. */
    bool bSavedSceneLoadFailed = false;
    int32 SavedSceneReadinessAttemptCount = 0;
    int32 SavedSceneDataAttemptCount = 0;
    FVector LastTraceStart = FVector::ZeroVector;
    FVector LastTraceDirection = FVector::ForwardVector;
    FHitResult LastTraceHit;
    bool bLastTraceBlockingHit = false; // True only when the short collision trace actually hit a blocking object.
    bool bLastTraceHasPlacementLocation = false; // True when the crosshair resolved either to a hit surface or to a free-space point.
    bool bLastTraceUsedFreeSpace = false; // True when the last cursor point came from the 10m air fallback instead of collision.
    FVector CachedPlacementGridCenter = FVector::ZeroVector;
    float CachedPlacementGridRadius = 0.0f;
    bool bPlacementGridBuilt = false;
    bool bIsSavingScene = false;

    bool bWorldBakeInProgress = false;
    bool bWorldArchiveCommitInFlight = false;
    bool bAutoBuildForStartup = false;
    bool bWorldStartupContinued = false;
    uint64 WorldBakeGeneration = 0;
    /** Worker-visible cancellation flag; a cancelled archive is never published from a stale world. */
    TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> WorldBakeCancellation;
    float WorldBakeProgressValue = 0.0f;
    int32 WorldBakeTotalModels = 0;
    int32 WorldBakeCompletedModels = 0;
    int32 WorldBakeFailedModels = 0;
    int32 WorldBakeNextModelIndex = 0;
    TArray<FModelDefinition> PendingWorldBakeModels;
    TArray<FGWorldBuildModel> CompletedWorldBuildModels;
    TArray<FSoundDefinition> PendingWorldBuildSounds;
    TMap<FGuid, FString> PendingWorldBuildSoundJson;
    UPROPERTY(Transient)
    TObjectPtr<UWorldSourceModelBuilder> ActiveWorldBuildTask;

    FTimerHandle SceneAutoSaveTimerHandle;
    FTimerHandle WorldDataSaveTimerHandle;
    FTimerHandle SavedSceneLoadRetryTimerHandle;
    /** Polls the active source builder so one-model jobs expose decode progress. */
    FTimerHandle WorldBakeProgressTimerHandle;

    void StartGameplaySessionInternal(AActor* InOwnerActor, const AGlTFSimulatorGameplayGameModeBase* InConfigGameMode);
    void ClearTransientRuntimeReferences();
    void DestroyTrackedRuntimeActors();
    void CompactTrackedEntityReferences();
    void ResetWorldRuntimeReferences();
    void RequestRuntimeGarbageCollection(const TCHAR* Reason) const;
    void PrepareForMenuLevelTravelRequest();
    void FinalizeWorldSelectionTravelState();
    void RequestPostLoadRuntimeMemoryCleanup();
    void HandlePostLoadMapRuntimeCleanup(UWorld* LoadedWorld);
    void RunPostLoadRuntimeMemoryCleanup();
    /**
     * Verifies that a loaded gameplay world is owned by the proper gameplay GameMode.
     * It never spawns a replacement manager actor; authority startup belongs to GameMode and
     * clients use their local PlayerController after the replicated world key arrives.
     */
    void ValidatePostLoadGameplayLifecycle(UWorld* LoadedWorld);
    void RefreshBuiltModelLists();
    FString GetWorldRootPath() const;
    void ScheduleSavedSceneLoadRetry(const FString& Reason, bool bWaitingForWorldReadiness);
    bool TracePlacementLocation(FVector& OutLocation, FHitResult& OutHit);
    FVector ApplyGridSnap(const FVector& Location) const;
    bool ShouldShowPlacementGrid() const;
    void UpdatePlacementGrid();
    void RebuildPlacementGridMesh(const FVector& Center, float Radius);
    FString GetAssetDisplayName(const FString& ModelReference) const;
    void AutoSaveScene();
    void ClearPlacementGridMesh();
    FString MakeObjectName(const FString& BaseName, EPlacedObjectKind Kind) const;
    int32 CountExistingBaseName(const FString& BaseName, EPlacedObjectKind Kind) const;
    void PlaceCurrentStatic(const FVector& Location);
    void PlaceVehicle(const FVector& Location, const FString& ModelReference);
    void TryEnterOrExitVehicle();
    void StartNextWorldBakeModel();
    void RefreshWorldBakeProgress();
    void HandleWorldBuildModelFinished(UWorldSourceModelBuilder* BuildTask, bool bSuccess);
    void FinishWorldBake();
    void CompleteWorldArchiveBuild(bool bSuccess, const FString& ArchivePath, const FString& Error);
    void CancelWorldBake();
    /** Ends only the runtime-world initialization/loading transaction; the environment actor stays alive for diagnostics. */
    void FailWorldStartup(const FString& Message);
    void ContinueWorldStartupAfterDatabase();
    void BuildAvailableItems();
    void InitializeToolbarSlotsIfNeeded();
    void ReconcileToolbarSlotsWithAvailableItems();
    void ApplySelectedToolbarItem(bool bBroadcastChange = true);
    FToolbarItem MakeToolbarItem(EToolbarItemKind Kind, const FString& DisplayName, const FString& ModelReference = FString(), int32 ModelIndex = INDEX_NONE) const;
    int32 FindAvailableItemIndexMatching(const FToolbarItem& Item) const;
    bool ShouldSpawnOcean() const;
    void SpawnOcean();
    /** Keeps the global ocean centered on the local camera in XY while preserving authored sea level Z. */
    void UpdateOceanFollow();
    void StartGameplayWorldStreaming(const FString& InWorldRoot, const FString& InInitialPlayerName);
    void InitializeRuntimeWorldState();
    void SpawnWorldEnvManager();
    bool CheckWorldSystemsLoaded();
    void LoadWorldData();
    void LoadPlayerData();
    void SaveWorldData();
    void SavePlayerData();
    void ResetInitialPlayerTransformState();
    void TryResolveInitialPlayerTransform();
    void FlushPendingInitialTransformSaves();
    void SaveWorldDataDelayed();
    void ApplyLevelSettings();
    void ApplyGameplaySettings();
    /** Logs and validates the actual server GameMode selected before gameplay runtime initialization begins. */
    void ValidateResolvedGameMode() const;
    void LoadWorldAsync();
    void UpdateWorldTime(float DeltaSeconds);
    void ShowLoadingWidget();
    void HideLoadingWidget();
    void NotifyStateChanged();
    void NotifyToolbarChanged();
};
