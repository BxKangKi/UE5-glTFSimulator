// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TimerManager.h"
#include "World/PlacementTypes.h"
#include "GameManagerSubSystem.generated.h"

class APrefabActor;
class AEditableMeshActor;
class AVehiclePawn;
class AWeaponActor;
class UCameraComponent;
class UMaterialInterface;
class UProceduralMeshComponent;
class USceneComponent;
class UWorldData;
class UUserWidget;
class AWorldManager;
class AglTFStreamActor;
class UglTFStreamSubSystem;
class UGameSettings;
class UPostProcessComponent;
class AGameManagerActor;
class UAssetManageSubSystem;
class UWorld;

UENUM(BlueprintType)
enum class EToolMode : uint8
{
    None UMETA(DisplayName="None"),
    PlacePrefab UMETA(DisplayName="Place Prefab"),
    PlaceEmptyObject UMETA(DisplayName="Place Empty Object"),
    EditVertices UMETA(DisplayName="Edit Vertices"),
    PlaceVehicle UMETA(DisplayName="Place Vehicle"),
    Weapon UMETA(DisplayName="Weapon")
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
    None UMETA(DisplayName="None"),
    CreateObject UMETA(DisplayName="Create Object"),
    Prefab UMETA(DisplayName="Prefab"),
    Weapon UMETA(DisplayName="Weapon"),
    Vehicle UMETA(DisplayName="Vehicle")
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

    // Opens StartWorld and asks its StartActor to show the world-selection widget after travel.
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle", meta=(WorldContext="WorldContextObject"))
    static void OpenWorldSelectionScreen(const UObject* WorldContextObject, FName WorldSelectionLevelName = NAME_None);

    // Opens the main menu from the world-selection screen. Bind this to the world-selection Back button when you want to reload StartWorld.
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle", meta=(WorldContext="WorldContextObject"))
    static void OpenMainMenuFromWorldSelection(const UObject* WorldContextObject, FName MainMenuLevelName = NAME_None);

    // Clears editor-only undo transactions before menu-triggered map travel. No-op outside editor builds.
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle", meta=(WorldContext="WorldContextObject"))
    static void ResetEditorTransactionBufferForWorldTravel(const UObject* WorldContextObject, const FString& Reason);

    // Blueprint-safe wrapper for widgets that still perform OpenLevel in Blueprint.
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle", meta=(WorldContext="WorldContextObject"))
    static void PrepareForWorldTravelFromUI(const UObject* WorldContextObject);

    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle")
    void StartGameManager(class AGameManagerActor* InConfigActor);

    void StopGameManager(const EEndPlayReason::Type EndPlayReason);
    void TickGameManager(float DeltaSeconds);
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

    void SetPlayerActor(AActor* Actor) { PlayerActor = Actor; }
    void SetCameraComponent(USceneComponent* InCamera) { CurrentCamera = InCamera; }
    void SetGameSettings(UGameSettings* Settings) { GameSettings = Settings; }
    void SetWorldData(UWorldData* Data) { CurrentWorldData = Data; }
    UFUNCTION(BlueprintCallable, Category="Game|World")
    void SetCurrentWorldName(FString Name) { CurrentWorldName = Name; }
    void SetPlayerLocation(FVector Location) { PlayerLocation = Location; }
    void SetPostProcess(UPostProcessComponent* InPostProcess) { PostProcess = InPostProcess; }
    template <typename T> T* GetPlayerActor() const { return Cast<T>(PlayerActor); }
    template <typename T> T* GetCameraComponent() const { return Cast<T>(CurrentCamera); }
    UFUNCTION(BlueprintPure, Category="SettingData")
    UGameSettings* GetGameSettings() const { return GameSettings; }
    FVector GetPlayerLocation() const { return PlayerLocation; }
    FVector GetCameraLocation() const { return IsValid(CurrentCamera) ? CurrentCamera->GetComponentLocation() : FVector::ZeroVector; }
    bool GetGamePaused() const { return bIsGamePaused; }
    UWorldData* GetWorldData() const { return CurrentWorldData; }
    FString GetCurrentWorldName() const { return CurrentWorldName; }
    static void ToggleFullscreen();
    void SetLoadingStatus(float InValue) { LoadingStatus = (int32)(InValue * 100); }
    UFUNCTION(BlueprintCallable, Category="Game|Loading")
    float GetLoadingStatus() const { return (float)(LoadingStatus / 100.0f); }
    UFUNCTION(BlueprintPure, Category="Game|Debug")
    FString GetDebugText();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SelectPreviousPrefab();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SelectNextPrefab();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SelectPrefabPlacementTool();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SelectEmptyObjectTool();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SelectVertexTool();

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
    bool FinishCurrentEditableMesh();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void CancelCurrentEditableMesh(bool bDestroyActor = true);

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    bool SaveScene();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    bool LoadSavedScene();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void RefreshAssetLists();

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    void SetCurrentToolMode(EToolMode NewMode);

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    bool SetCurrentPrefabIndex(int32 NewIndex);

    UFUNCTION(BlueprintCallable, Category="Game|UI Actions")
    bool SetCurrentWeaponIndex(int32 NewIndex);

    /** Compatibility short-click action. Prefer InputPrimaryPressed/Released for click-vs-hold vertex editing. */
    UFUNCTION(BlueprintCallable, Category="Game|Input")
    void InputPrimaryAction();

    /** Left mouse pressed. In vertex edit mode, pressing an existing vertex starts click/hold classification. */
    UFUNCTION(BlueprintCallable, Category="Game|Input")
    void InputPrimaryPressed();

    /** Left mouse released. Short click selects a linked-vertex source; hold+move commits vertex movement. */
    UFUNCTION(BlueprintCallable, Category="Game|Input")
    void InputPrimaryReleased();

    /** Right mouse / secondary action. Ends current vertex editing; invalid/no-face objects are canceled. */
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

    /** Useful for BP widgets that want explicit Select/Confirm buttons as well as LMB/RMB. */
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

    UFUNCTION(BlueprintPure, Category="Game|Toolbar")
    bool IsSelectedToolbarItemObjectCreation() const;

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

    UFUNCTION(BlueprintPure, Category="Game|Placement")
    bool HasPendingPlacementSelection() const { return bHasPendingEmptyObjectLocation || bHasPendingVertexLocation; }

    UFUNCTION(BlueprintPure, Category="Game|Placement")
    FVector GetPendingPlacementSelection() const;

    UFUNCTION(BlueprintCallable, Category="Game|Placement")
    void ClearPendingPlacementSelection();

    UFUNCTION(BlueprintPure, Category="Game|Generated Mesh")
    bool IsEditingGeneratedMesh() const;

    UFUNCTION(BlueprintPure, Category="Game|Generated Mesh")
    AEditableMeshActor* GetCurrentEditableMeshActor() const { return CurrentEditableActor.Get(); }

    UFUNCTION(BlueprintPure, Category="Game|Generated Mesh")
    int32 GetCurrentEditableMeshVertexCount() const;

    UFUNCTION(BlueprintPure, Category="Game|Generated Mesh")
    int32 GetCurrentEditableMeshTriangleCount() const;

    UFUNCTION(BlueprintPure, Category="Game|Generated Mesh")
    bool IsCurrentEditableMeshTopologyValid() const;

    UFUNCTION(BlueprintPure, Category="Game|Generated Mesh")
    int32 GetHighlightedEditableVertexIndex() const { return HighlightedEditableVertexIndex; }

    UFUNCTION(BlueprintPure, Category="Game|Generated Mesh")
    bool IsMovingEditableVertex() const { return bMovingHighlightedEditableVertex; }

    UFUNCTION(BlueprintPure, Category="Game|Generated Mesh")
    bool IsVertexPrimaryPressActive() const { return bPrimaryVertexPressActive; }

    UFUNCTION(BlueprintPure, Category="Game|Generated Mesh")
    int32 GetConnectedEditableVertexSourceIndex() const { return ConnectedEditableVertexSourceIndex; }

    UFUNCTION(BlueprintCallable, Category="Game|Generated Mesh")
    void GetSpawnedGeneratedMeshActors(TArray<AEditableMeshActor*>& OutActors) const;

    /** Starts gameplay-owned model streaming and optional ocean actor creation. */
    UFUNCTION(BlueprintCallable, Category="Game|World")
    void InitializeWorldSystems(UWorldData* InWorldData, const FString& InModelDirectory, const FString& InPlayerDirectory, const FString& InInitialPlayerName);

    /** Stops world streaming and clears transient world actors created by this manager. */
    UFUNCTION(BlueprintCallable, Category="Game|World")
    void StopWorldSystems();

    /** Saves the active scene, stops runtime streaming, and marks a full purge for menu/world-selection level travel. */
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle")
    void PrepareForReturnToMenuLevel();

    /** Legacy wrapper kept for older Blueprint calls. Use PrepareForReturnToMenuLevel(). */
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle")
    void PrepareForReturnToStartWorld();

    /** Releases runtime main-world actors/assets that can otherwise survive a level transition through GameInstance subsystems. */
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle")
    void ReleaseMainWorldRuntimeMemory(bool bForceGarbageCollection = true);

    UFUNCTION(BlueprintPure, Category="Game|Lifecycle")
    bool HasPendingMainWorldRuntimePurge() const { return bPendingMainWorldRuntimePurge; }

    /** Requests that StartWorld opens directly on the world-selection widget after the next level travel. */
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle")
    void RequestWorldSelectionMenuOnNextStartWorld();

    /** Consumes and clears the pending StartWorld world-selection request. */
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle")
    bool ConsumeWorldSelectionMenuRequest();

    /** Clears the pending StartWorld world-selection request. */
    UFUNCTION(BlueprintCallable, Category="Game|Lifecycle")
    void ClearWorldSelectionMenuRequest();

    UFUNCTION(BlueprintPure, Category="Game|Lifecycle")
    bool ShouldOpenWorldSelectionMenuOnNextStartWorld() const { return bOpenWorldSelectionMenuOnNextStartWorld; }


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
    FVector PlayerLocation;
    int32 LoadingStatus = 0;
    int32 TotalSumFPS = 0;
    int32 TotalCountFPS = 0;
    FString GetHardwareInfoText(FString InString);
    FString GetFramerateInfoText(FString InString);


    UPROPERTY(Transient)
    TWeakObjectPtr<AGameManagerActor> ConfigActor;

    FTimerManager& GetWorldTimerManager() const;
    FVector GetManagerActorLocation() const;
    void EnsureRuntimeComponents();

    UPROPERTY(Transient)
    TObjectPtr<USceneComponent> Root;

    UPROPERTY(Transient)
    TObjectPtr<UProceduralMeshComponent> PlacementGridComponent;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInterface> PlacementGridMaterial;

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
    TSubclassOf<AEditableMeshActor> EditableMeshActorClass;

    UPROPERTY(Transient)
    TSubclassOf<AVehiclePawn> VehiclePawnClass;

    UPROPERTY(Transient)
    TSubclassOf<AWeaponActor> WeaponActorClass;

    // GameManager owns the world boot sequence so WorldManager can stay rendering-only.
    UPROPERTY(Transient)
    TSubclassOf<AWorldManager> WorldManagerClass;

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
    TObjectPtr<AWorldManager> WorldManagerActor;

    UPROPERTY()
    TObjectPtr<AActor> OceanActor;

    UPROPERTY()
    TObjectPtr<UglTFStreamSubSystem> StreamSubSystem;

    UPROPERTY()
    TObjectPtr<UUserWidget> LoadingWidgetInstance;

    bool bManagerStarted = false;
    bool bWorldBootstrapStarted = false;
    bool bWorldLoadCompleted = false;
    bool bSpawnedWorldManager = false;
    bool bPendingMainWorldRuntimePurge = false;
    bool bOpenWorldSelectionMenuOnNextStartWorld = false;
    FDelegateHandle PostLoadMapCleanupHandle;

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
    float VertexSelectionRayDistance = 28.0f;

    UPROPERTY(Transient)
    float VertexDragHoldSeconds = 0.18f;

    UPROPERTY(Transient)
    float VertexDragStartDistance = 18.0f;

    UPROPERTY(Transient)
    float VehicleEnterDistance = 450.0f;

    /** Temporary kill-switch: hides and blocks the hand-built vertex/object creation tool without removing the code. */
    UPROPERTY(Transient)
    bool bEnableObjectVertexCreation = false;

    /** Periodically saves runtime placed prefabs/vehicles/generated entities. */
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
    TObjectPtr<AEditableMeshActor> CurrentEditableActor;

    UPROPERTY()
    TObjectPtr<AEditableMeshActor> PendingEmptyObjectPreviewActor;

    UPROPERTY()
    TObjectPtr<AWeaponActor> EquippedWeapon;

    UPROPERTY()
    TArray<TObjectPtr<APrefabActor>> SpawnedPrefabs;

    UPROPERTY()
    TArray<TObjectPtr<AEditableMeshActor>> SpawnedGeneratedMeshes;

    UPROPERTY()
    TArray<TObjectPtr<AVehiclePawn>> SpawnedVehicles;

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
    FVector PendingEmptyObjectLocation = FVector::ZeroVector;
    FVector PendingVertexLocation = FVector::ZeroVector;
    bool bHasPendingEmptyObjectLocation = false;
    bool bHasPendingVertexLocation = false;
    float LastVertexDistance = 0.0f;
    FString LastSaveMessage;
    bool bSavedSceneLoaded = false;
    FVector LastTraceStart = FVector::ZeroVector;
    FVector LastTraceDirection = FVector::ForwardVector;
    FHitResult LastTraceHit;
    bool bLastTraceBlockingHit = false; // True only when the short collision trace actually hit a blocking object.
    bool bLastTraceHasPlacementLocation = false; // True when the crosshair resolved either to a hit surface or to a free-space point.
    bool bLastTraceUsedFreeSpace = false; // True when the last cursor point came from the 10m air fallback instead of collision.
    int32 HighlightedEditableVertexIndex = INDEX_NONE; // Vertex currently selected by the center-crosshair ray.
    bool bMovingHighlightedEditableVertex = false; // True while a held click is dragging a vertex.
    bool bPrimaryVertexPressActive = false; // True between left-button press and release while deciding click vs hold.
    bool bPrimaryVertexDragActive = false; // True after hold-time and movement thresholds convert a press into a drag.
    int32 PressedEditableVertexIndex = INDEX_NONE; // Vertex that was under the crosshair when the current left press began.
    int32 ConnectedEditableVertexSourceIndex = INDEX_NONE; // Vertex that the next new or merged segment should continue from.
    double PrimaryVertexPressStartTime = 0.0; // World time when the current vertex press started.
    FVector PrimaryVertexPressStartLocation = FVector::ZeroVector; // World location used to measure drag distance from the press start.
    bool bCurrentEditableActorWasExisting = false;
    bool bHasOriginalEditableMeshRecord = false;
    FGeneratedMeshRecord OriginalEditableMeshRecord;
    FVector CachedPlacementGridCenter = FVector::ZeroVector;
    float CachedPlacementGridRadius = 0.0f;
    bool bPlacementGridBuilt = false;
    bool bIsSavingScene = false;
    FTimerHandle SceneAutoSaveTimerHandle;
    FTimerHandle WorldDataSaveTimerHandle;

    void ClearTransientRuntimeReferences();
    void DestroyTrackedRuntimeActors();
    void ResetWorldRuntimeReferences();
    void RequestRuntimeGarbageCollection(const TCHAR* Reason) const;
    UAssetManageSubSystem* GetAssetManagerSubsystem() const;
    void RequestPostLoadRuntimeMemoryCleanup();
    void HandlePostLoadMapRuntimeCleanup(UWorld* LoadedWorld);
    void RunPostLoadRuntimeMemoryCleanup();
    void ScanAssetFolders();
    void EnsureAssetFolders() const;
    FString GetWorldRootPath() const;
    FString GetPrefabDirectory() const;
    FString GetItemsDirectory() const;
    FString GetManifestPath() const;
    FString GetLegacyManifestPath() const;
    FString GetLegacyGltfScenePath() const;
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
    bool TryUseObjectCreationItemAtCrosshair();
    bool CloseCurrentEditableMeshForToolChange();
    void PlaceCurrentPrefab(const FVector& Location);
    void PlaceEmptyObject(const FVector& Location, AEditableMeshActor* ExistingPreviewActor = nullptr);
    void BeginEditingExistingMesh(AEditableMeshActor* MeshActor);
    void AddVertexToEditableObject(const FVector& Location);
    bool AddExistingVertexToEditableObject(int32 ExistingVertexIndex);
    bool FinishOrCancelCurrentVertexEditing();
    void PlaceVehicle(const FVector& Location, const FString& SourceFile = FString());
    void TryEnterOrExitVehicle();
    void CollectSceneRecords(TArray<FPlacedObjectRecord>& OutPlaced, TArray<FGeneratedMeshRecord>& OutMeshes) const;
    void UpdatePendingEmptyObjectPreview(const FVector& Location);
    void DestroyPendingEmptyObjectPreview();
    void UpdateObjectCreationPreview();
    void UpdateEditableVertexPreviewAndSelection();
    void BeginEditableVertexPrimaryPress(int32 VertexIndex);
    void UpdateEditableVertexPrimaryPressDrag();
    void EndEditableVertexPrimaryPress();
    void BeginConnectedVertexCreationFromIndex(int32 VertexIndex);
    void ClearConnectedVertexCreationState();
    void ClearEditableVertexMoveState();
    AEditableMeshActor* GetEditableMeshFromHit(const FHitResult& Hit) const;
    void BuildAvailableItems();
    void InitializeToolbarSlotsIfNeeded();
    void ReconcileToolbarSlotsWithAvailableItems();
    void ApplySelectedToolbarItem(bool bBroadcastChange = true);
    FToolbarItem MakeToolbarItem(EToolbarItemKind Kind, const FString& DisplayName, const FString& SourcePath = FString(), int32 SourceIndex = INDEX_NONE) const;
    int32 FindAvailableItemIndexMatching(const FToolbarItem& Item) const;
    bool IsObjectCreationItem(const FToolbarItem& Item) const;
    bool ShouldSpawnOcean() const;
    void SpawnOcean();
    void StartWorldStreaming(const FString& InModelDirectory, const FString& InPlayerDirectory, const FString& InInitialPlayerName);
    void InitializeWorldBootstrap();
    void SpawnWorldManager();
    bool CheckWorldSystemsLoaded();
    void LoadWorldData();
    void SaveWorldData();
    void SaveWorldDataTick();
    void LoadWorldAsync();
    void UpdateWorldTime(float DeltaSeconds);
    FString GetWorldFilePath(const FString& FileName) const;
    void ShowLoadingWidget();
    void HideLoadingWidget();
    void NotifyStateChanged();
    void NotifyToolbarChanged();
};
