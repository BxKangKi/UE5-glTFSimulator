// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/HitResult.h"
#include "GameFramework/Actor.h"
#include "Model/glTFMaterialAssetReferences.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TimerManager.h"
#include "World/PlacementTypes.h"
#include "GameManagerSubSystem.generated.h"

class APrefabActor;
class AVehiclePawn;
class AWeaponActor;
class UCameraComponent;
class UMaterialInterface;
class UMaterialDefaultAsset;
class UMaterialDefaultRuntimeCache;
class UProceduralMeshComponent;
class USceneComponent;
class UWorldData;
class UPlayerData;
class AWeatherActor;
class UUserWidget;
class AWorldEnvManager;
class AglTFStreamActor;
class UglTFStreamSubSystem;
class UGameSettings;
class UPostProcessComponent;
class AGameManagerActor;
class UWorld;
class APlayerController;

UENUM(BlueprintType)
enum class EToolMode : uint8
{
    None = 0 UMETA(DisplayName="None"),
    PlacePrefab = 1 UMETA(DisplayName="Place Prefab"),
    // Values 2 and 3 are reserved for the removed object/vertex authoring tools.
    PlaceVehicle = 4 UMETA(DisplayName="Place Vehicle"),
    Weapon = 5 UMETA(DisplayName="Weapon")
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
    // Value 1 is reserved for the removed object-creation item.
    Prefab = 2 UMETA(DisplayName="Prefab"),
    Weapon = 3 UMETA(DisplayName="Weapon"),
    Vehicle = 4 UMETA(DisplayName="Vehicle")
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
    FString SourcePath;

    UPROPERTY(Transient)
    int32 SourceIndex = INDEX_NONE;

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

    // Opens the directly assigned menu world and asks its StartActor to show world selection after travel.
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle", meta=(WorldContext="WorldContextObject"))
    static void OpenWorldSelectionScreen(const UObject* WorldContextObject, TSoftObjectPtr<UWorld> WorldSelectionWorld);

    /** Native request path used by pause UI. True means this or an equivalent duplicate request owns travel. */
    static bool TryOpenWorldSelectionScreen(const UObject* WorldContextObject, TSoftObjectPtr<UWorld> WorldSelectionWorld);

    /** Remembers the currently loaded StartActor menu world without a hard-coded map name or asset path. */
    void RegisterWorldSelectionWorld(TSoftObjectPtr<UWorld> InWorldSelectionWorld);

    /** Fallback for gameplay controllers whose Blueprint default does not repeat the menu-world assignment. */
    TSoftObjectPtr<UWorld> GetRegisteredWorldSelectionWorld() const { return RegisteredWorldSelectionWorld; }

    /** Rolls back an accepted request when OpenLevel fails and the gameplay world remains active. */
    void CancelWorldSelectionMenuTravel();

    // Opens the directly assigned main-menu world from the world-selection screen.
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle", meta=(WorldContext="WorldContextObject"))
    static void OpenMainMenuFromWorldSelection(const UObject* WorldContextObject, TSoftObjectPtr<UWorld> MainMenuWorld);

    // Clears editor-only undo transactions before menu-triggered map travel. No-op outside editor builds.
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle", meta=(WorldContext="WorldContextObject"))
    static void ResetEditorTransactionBufferForWorldTravel(const UObject* WorldContextObject, const FString& Reason);

    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle")
    void StartGameManager(class AGameManagerActor* InConfigActor);

    /** Stops the active session. A non-null requester may stop only the session it owns. */
    void StopGameManager(
        const EEndPlayReason::Type EndPlayReason,
        const class AGameManagerActor* RequestingActor = nullptr);
    void UpdateGameManager(float DeltaSeconds);
    void ApplyEditorConfig(const class AGameManagerActor* InConfigActor);

    UFUNCTION(BlueprintCallable, Category="Game|Settings")
    void SaveSettings();
    UFUNCTION(BlueprintCallable, Category="Game|Settings")
    void UpdateSettings();
    UFUNCTION(BlueprintCallable, Category="Game|Pause")
    void TogglePause();
    UFUNCTION(BlueprintCallable, Category="Game|Pause")
    void SetGamePaused(bool bPaused);
    UFUNCTION(BlueprintCallable, Category="Game|Loading")
    void SetWorldLoading(bool bLoading);
    UFUNCTION(BlueprintPure, Category="Game|Loading")
    bool IsWorldLoading() const { return bIsWorldLoading; }

    /**
     * Registers the active character. During world bootstrap this also completes the one-shot
     * saved-transform/PlayerStart handshake; later pawn replacements only update the reference.
     */
    void SetPlayerActor(AActor* Actor);
    /** Reasserts a saved initial view rotation after GameMode finishes possessing the first player. */
    void ApplyPendingInitialPlayerControlRotation(APlayerController* Controller);
    void SetCameraComponent(USceneComponent* InCamera) { CurrentCamera = InCamera; }
    void SetGameSettings(UGameSettings* Settings) { GameSettings = Settings; }
    void SetWorldData(UWorldData* Data) { CurrentWorldData = Data; }
    UFUNCTION(BlueprintCallable, Category="Game|World")
    void SetCurrentWorldName(FString Name) { CurrentWorldName = Name; }
    /** Accepts runtime location updates only from the registered primary player actor. */
    void SetPlayerLocation(const FVector& Location, const AActor* SourceActor);
    void SetPostProcess(UPostProcessComponent* InPostProcess) { PostProcess = InPostProcess; }
    template <typename T> T* GetPlayerActor() const { return Cast<T>(PlayerActor); }
    template <typename T> T* GetCameraComponent() const { return Cast<T>(CurrentCamera); }
    UFUNCTION(BlueprintPure, Category="SettingData")
    UGameSettings* GetGameSettings() const { return GameSettings; }
    FVector GetPlayerLocation() const { return PlayerLocation; }
    FVector GetCameraLocation() const { return IsValid(CurrentCamera) ? CurrentCamera->GetComponentLocation() : FVector::ZeroVector; }
    bool GetGamePaused() const { return bIsGamePaused; }
    UWorldData* GetWorldData() const { return CurrentWorldData; }
    UFUNCTION(BlueprintPure, Category="Game|Player")
    UPlayerData* GetPlayerData() const { return ActivePlayerData; }
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
    void SelectPreviousPrefab();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SelectNextPrefab();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SelectPrefabPlacementTool();

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

    /**
     * Writes data/entities.dat, data/players.dat, data/world.dat and validates/builds every sibling
     * model .scz under the selected world. Designed to be mapped directly to a world-list UI button.
     */
    UFUNCTION(BlueprintCallable, Category="Game|Bake")
    void BakeWorldData();

    UFUNCTION(BlueprintPure, Category="Game|Bake")
    bool IsWorldBakeInProgress() const { return bWorldBakeInProgress; }

    UFUNCTION(BlueprintPure, Category="Game|Bake")
    float GetWorldBakeProgress() const { return WorldBakeProgressValue; }

    /** Updates the selected player runtime record and persists it to data/world.dat. */
    UFUNCTION(BlueprintCallable, Category="Game|World Data")
    void SetSelectedPlayerForRuntime(const FString& PlayerFileName);

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void RefreshAssetLists();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SetCurrentToolMode(EToolMode NewMode);

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    bool SetCurrentPrefabIndex(int32 NewIndex);

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    bool SetCurrentWeaponIndex(int32 NewIndex);

    /** Compatibility short-click action for the currently selected placement/equipment item. */
    UFUNCTION(BlueprintCallable, Category="Game|Input")
    void InputPrimaryAction();

    /** Left mouse pressed. Executes the selected placement/equipment action once. */
    UFUNCTION(BlueprintCallable, Category="Game|Input")
    void InputPrimaryPressed();

    /** Left mouse released. Retained as a stable input-mapping endpoint. */
    UFUNCTION(BlueprintCallable, Category="Game|Input")
    void InputPrimaryReleased();

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
    FString GetCurrentPrefabName() const;

    UFUNCTION(BlueprintPure, Category="Game|Status")
    FString GetCurrentWeaponName() const;

    UFUNCTION(BlueprintPure, Category="Game|Assets")
    int32 GetPrefabCount() const { return PrefabFiles.Num(); }

    UFUNCTION(BlueprintPure, Category="Game|Assets")
    int32 GetWeaponCount() const { return WeaponFiles.Num(); }

    UFUNCTION(BlueprintPure, Category="Game|Assets")
    int32 GetCurrentPrefabIndex() const { return CurrentPrefabIndex; }

    UFUNCTION(BlueprintPure, Category="Game|Assets")
    int32 GetCurrentWeaponIndex() const { return CurrentWeaponIndex; }

    UFUNCTION(BlueprintPure, Category="Game|Assets")
    FString GetPrefabNameAtIndex(int32 Index) const;

    UFUNCTION(BlueprintPure, Category="Game|Assets")
    FString GetWeaponNameAtIndex(int32 Index) const;

    UFUNCTION(BlueprintPure, Category="Game|Assets")
    FString GetPrefabPathAtIndex(int32 Index) const;

    UFUNCTION(BlueprintPure, Category="Game|Assets")
    FString GetWeaponPathAtIndex(int32 Index) const;

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
    void InitializeWorldSystems(UWorldData* InWorldData, const FString& InModelDirectory, const FString& InPlayerDirectory, const FString& InInitialPlayerName);

    /** Stops world streaming and clears transient world actors created by this manager. */
    UFUNCTION(BlueprintCallable, Category="Game|World")
    void StopWorldSystems();

    /** Saves the active scene, stops runtime streaming, and marks a full purge for menu/world-selection level travel. */
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle")
    void PrepareForReturnToMenuLevel();

    /** MainWorld-specific entry point that shares the common menu-travel cleanup path. */
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle")
    void PrepareForReturnToMainWorld();

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
    UWorldData* GetActiveWorldData() const { return ActiveWorldData; }

    /** Returns true when initial streamed GLB models and the player replacement are ready. */
    UFUNCTION(BlueprintPure, Category="Game|World")
    bool AreWorldSystemsReady() const;

    /** Returns the loading percent reported by the GLB stream subsystem. */
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
    FVector PlayerLocation = FVector::ZeroVector;
    float LoadingStatus = 0.0f;
    int32 TotalSumFPS = 0;
    int32 TotalCountFPS = 0;
    FString GetHardwareInfoText(FString InString);
    FString GetFramerateInfoText(FString InString);


    UPROPERTY(Transient)
    TWeakObjectPtr<AGameManagerActor> ConfigActor;

    FTimerManager& GetWorldTimerManager() const;
    FVector GetManagerActorLocation() const;
    void EnsureRuntimeComponents();
    bool ResolveMaterialDefaultAsset();
    void ReleaseMaterialDefaultAsset();

    UPROPERTY(Transient)
    TObjectPtr<USceneComponent> Root;

    UPROPERTY(Transient)
    TObjectPtr<UProceduralMeshComponent> PlacementGridComponent;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInterface> PlacementGridMaterial;


    /** Class copied from the configuration actor. No mutable configuration instance exists before play. */
    UPROPERTY(Transient)
    TSubclassOf<UMaterialDefaultAsset> MaterialDefaultAssetClass;

    /**
     * Transient configuration instance created on the game thread when the manager starts. Keeping
     * it as a UPROPERTY prevents collection during nested synchronous material loads or re-entrant calls.
     */
    UPROPERTY(Transient)
    TObjectPtr<UMaterialDefaultAsset> MaterialDefaultAssetInstance;

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
    TSubclassOf<APrefabActor> PrefabActorClass;

    UPROPERTY(Transient)
    TSubclassOf<AVehiclePawn> VehiclePawnClass;

    UPROPERTY(Transient)
    TSubclassOf<AWeaponActor> WeaponActorClass;

    UPROPERTY(Transient)
    TSubclassOf<AWeatherActor> WeatherActorClass;

    // GameManager owns the world boot sequence so WorldEnvManager can stay rendering-only.
    UPROPERTY(Transient)
    TSubclassOf<AWorldEnvManager> WorldEnvManagerClass;

    UPROPERTY(Transient)
    TSubclassOf<AglTFStreamActor> SpawnActorClass;

    UPROPERTY(Transient)
    TSubclassOf<AActor> WaterClass;

    UPROPERTY(Transient)
    FTransform OceanTransform;

    UPROPERTY(Transient)
    TSubclassOf<UUserWidget> LoadingWidgetClass;

    UPROPERTY()
    TObjectPtr<UWorldData> ActiveWorldData;

    UPROPERTY()
    TObjectPtr<UPlayerData> ActivePlayerData;

    UPROPERTY()
    TObjectPtr<AWeatherActor> ActiveWeatherActor;

    UPROPERTY()
    TObjectPtr<AWorldEnvManager> WorldEnvManagerActor;

    UPROPERTY()
    TObjectPtr<AActor> OceanActor;

    UPROPERTY()
    TObjectPtr<UglTFStreamSubSystem> StreamSubSystem;

    UPROPERTY()
    TObjectPtr<UUserWidget> LoadingWidgetInstance;

    bool bManagerStarted = false;
    bool bWorldBootstrapStarted = false;
    bool bWorldLoadCompleted = false;
    bool bSpawnedWorldEnvManager = false;
    bool bPendingMainWorldRuntimePurge = false;
    bool bOpenWorldSelectionMenuOnNextMainWorld = false;
    /** Separate from the destination-menu request so compatibility code may pre-set that request safely. */
    bool bWorldSelectionMenuTravelInProgress = false;
    /** Menu world captured from the active StartActor; no name/path literal is used. */
    TSoftObjectPtr<UWorld> RegisteredWorldSelectionWorld;
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
        LegacyWorldFile,
        PlayerFile
    };

    EInitialPlayerLocationSource InitialPlayerLocationSource = EInitialPlayerLocationSource::None;
    FRotator LoadedInitialPlayerRotation = FRotator::ZeroRotator;
    bool bHasLoadedInitialPlayerRotation = false;
    bool bInitialPlayerDataLoadCompleted = false;
    bool bInitialPlayerTransformResolved = false;
    bool bPendingInitialControlRotation = false;
    bool bPendingInitialWorldDataSave = false;
    bool bPendingInitialPlayerDataSave = false;

    // Legacy placement distance kept so older Blueprint defaults do not lose the property.
    // The center-crosshair cursor now uses CrosshairCollisionTraceDistance and FreeSpacePlacementDistance below.
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

    /** Periodically saves runtime placed prefabs and vehicles. */
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
    TArray<TWeakObjectPtr<APrefabActor>> SpawnedPrefabs;

    UPROPERTY(Transient)
    TArray<TWeakObjectPtr<AVehiclePawn>> SpawnedVehicles;

    TArray<FString> PrefabFiles;
    TArray<FString> VehicleFiles;
    TArray<FString> WeaponFiles;
    TArray<FToolbarItem> AvailableItems;
    TArray<FToolbarItem> ToolbarSlots;
    int32 SelectedToolbarSlotIndex = 0;
    int32 CurrentPrefabIndex = 0;
    int32 CurrentWeaponIndex = 0;
    EToolMode CurrentMode = EToolMode::None;
    bool bSnapToGrid = false;
    bool bFirstPerson = false;
    bool bItemListWindowOpen = false;
    bool bToolbarInitialized = false;
    FVector LastPreviewLocation = FVector::ZeroVector;
    FString LastSaveMessage;
    /** True only after entities.dat was validated and its records were applied (or confirmed absent). */
    bool bSavedSceneLoaded = false;
    bool bSavedSceneLoadInProgress = false;
    /** Protects an unreadable entities.dat from being overwritten by an empty autosave. */
    bool bSavedSceneLoadFailed = false;
    int32 SavedSceneReadinessAttemptCount = 0;
    int32 SavedSceneDataAttemptCount = 0;
    /** Last validated entity snapshot. Preserves records while actors are tearing down or temporarily unavailable. */
    TArray<FPlacedObjectRecord> LastKnownSceneRecords;
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
    bool bWorldBakeStateFilesSaved = false;
    float WorldBakeProgressValue = 0.0f;
    int32 WorldBakeTotalModels = 0;
    int32 WorldBakeCompletedModels = 0;
    int32 WorldBakeFailedModels = 0;
    int32 WorldBakeNextModelIndex = 0;
    TArray<FString> PendingWorldBakeModels;

    UPROPERTY(Transient)
    TObjectPtr<AglTFStreamActor> ActiveWorldBakeActor;

    FTimerHandle SceneAutoSaveTimerHandle;
    FTimerHandle WorldDataSaveTimerHandle;
    FTimerHandle SavedSceneLoadRetryTimerHandle;
    /** Polls the active metadata actor so one-model Bake jobs also expose size-scan progress. */
    FTimerHandle WorldBakeProgressTimerHandle;

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
    void ScanAssetFolders();
    void EnsureAssetFolders() const;
    FString GetWorldRootPath() const;
    FString GetPrefabDirectory() const;
    FString GetItemsDirectory() const;
    FString GetDataDirectory() const;
    FString GetManifestPath() const;
    FString GetPlayersDatPath() const;
    FString GetWorldDatPath() const;
    FString MakePlacementSourcePathForSave(const FString& SourcePath) const;
    FString ResolvePlacementSourcePath(const FString& SavedPath, EPlacedObjectKind Kind) const;
    void ScheduleSavedSceneLoadRetry(const FString& Reason, bool bWaitingForWorldReadiness);
    bool TracePlacementLocation(FVector& OutLocation, FHitResult& OutHit);
    FVector ApplyGridSnap(const FVector& Location) const;
    bool ShouldShowPlacementGrid() const;
    void UpdatePlacementGrid();
    void RebuildPlacementGridMesh(const FVector& Center, float Radius);
    bool DoesAssetFileContainWheelTag(const FString& FilePath) const;
    FString GetAssetDisplayName(const FString& AssetPath) const;
    void AutoSaveScene();
    void ClearPlacementGridMesh();
    FString MakeObjectName(const FString& BaseName, EPlacedObjectKind Kind) const;
    int32 CountExistingBaseName(const FString& BaseName, EPlacedObjectKind Kind) const;
    void PlaceCurrentPrefab(const FVector& Location);
    void PlaceVehicle(const FVector& Location, const FString& SourceFile);
    void TryEnterOrExitVehicle();
    void CollectSceneRecords(TArray<FPlacedObjectRecord>& OutPlaced) const;
    void StartNextWorldBakeModel();
    void RefreshWorldBakeProgress();
    void HandleWorldBakeModelFinished(AglTFStreamActor* BakeActor, bool bSuccess);
    void FinishWorldBake();
    void CancelWorldBake();
    void BuildAvailableItems();
    void InitializeToolbarSlotsIfNeeded();
    void ReconcileToolbarSlotsWithAvailableItems();
    void ApplySelectedToolbarItem(bool bBroadcastChange = true);
    FToolbarItem MakeToolbarItem(EToolbarItemKind Kind, const FString& DisplayName, const FString& SourcePath = FString(), int32 SourceIndex = INDEX_NONE) const;
    int32 FindAvailableItemIndexMatching(const FToolbarItem& Item) const;
    bool ShouldSpawnOcean() const;
    void SpawnOcean();
    void MainWorldStreaming(const FString& InModelDirectory, const FString& InPlayerDirectory, const FString& InInitialPlayerName);
    void StartWorldStreaming(const FString& InModelDirectory, const FString& InPlayerDirectory, const FString& InInitialPlayerName);
    void InitializeWorldBootstrap();
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
    void ApplyWeatherSettings();
    void ApplyGameplaySettings();
    /** Logs and validates the actual server GameMode selected before gameplay bootstrap begins. */
    void ValidateResolvedGameMode() const;
    void LoadWorldAsync();
    void UpdateWorldTime(float DeltaSeconds);
    FString GetWorldFilePath(const FString& FileName) const;
    void ShowLoadingWidget();
    void HideLoadingWidget();
    void NotifyStateChanged();
    void NotifyToolbarChanged();
};
