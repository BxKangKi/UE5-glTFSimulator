// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"
#include "Runtime/RuntimePlacementTypes.h"
#include "RuntimeGameplayManager.generated.h"

class ARuntimePrefabActor;
class ARuntimeEditableMeshActor;
class ARuntimeVehiclePawn;
class ARuntimeWeaponActor;
class UCameraComponent;
class UMaterialInterface;
class UProceduralMeshComponent;
class USceneComponent;

UENUM(BlueprintType)
enum class ERuntimeToolMode : uint8
{
    None UMETA(DisplayName="None"),
    PlacePrefab UMETA(DisplayName="Place Prefab"),
    PlaceEmptyObject UMETA(DisplayName="Place Empty Object"),
    EditVertices UMETA(DisplayName="Edit Vertices"),
    PlaceVehicle UMETA(DisplayName="Place Vehicle"),
    Weapon UMETA(DisplayName="Weapon")
};

UENUM(BlueprintType)
enum class ERuntimePlayMode : uint8
{
    Creator UMETA(DisplayName="Creator Mode"),
    RealLife UMETA(DisplayName="Real Life Mode")
};

UENUM(BlueprintType)
enum class ERuntimeToolbarItemKind : uint8
{
    None UMETA(DisplayName="None"),
    CreateObject UMETA(DisplayName="Create Object"),
    Prefab UMETA(DisplayName="Prefab"),
    Weapon UMETA(DisplayName="Weapon"),
    Vehicle UMETA(DisplayName="Vehicle")
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FRuntimeToolbarItem
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Toolbar")
    ERuntimeToolbarItemKind Kind = ERuntimeToolbarItemKind::None;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Toolbar")
    FString DisplayName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Toolbar")
    FString SourcePath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Toolbar")
    int32 SourceIndex = INDEX_NONE;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Toolbar")
    bool bAvailable = false;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FRuntimeGameplayStateChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FRuntimeToolbarChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRuntimeGameplayMessageChanged, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRuntimeItemListWindowChanged, bool, bOpen);

UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API ARuntimeGameplayManager : public AActor
{
    GENERATED_BODY()

public:
    ARuntimeGameplayManager();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    void SelectPreviousPrefab();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    void SelectNextPrefab();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    void SelectPrefabPlacementTool();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    void SelectEmptyObjectTool();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    void SelectVertexTool();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    void SelectVehicleTool();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    void SelectPreviousWeapon();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    void SelectNextWeapon();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    void EquipCurrentWeapon();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    void ToggleSnap();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    void SetSnapEnabled(bool bEnabled);

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    void SetGridSize(float NewGridSize);

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    void ToggleFirstPerson();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    bool FinishCurrentEditableMesh();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    void CancelCurrentEditableMesh(bool bDestroyActor = true);

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    bool SaveRuntimeScene();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    bool LoadSavedRuntimeScene();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    void RefreshRuntimeAssetLists();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    void SetCurrentToolMode(ERuntimeToolMode NewMode);

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    bool SetCurrentPrefabIndex(int32 NewIndex);

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|UI Actions")
    bool SetCurrentWeaponIndex(int32 NewIndex);

    /** Compatibility short-click action. Prefer InputPrimaryPressed/Released for click-vs-hold vertex editing. */
    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Input")
    void InputPrimaryAction();

    /** Left mouse pressed. In vertex edit mode, pressing an existing vertex starts click/hold classification. */
    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Input")
    void InputPrimaryPressed();

    /** Left mouse released. Short click selects a linked-vertex source; hold+move commits vertex movement. */
    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Input")
    void InputPrimaryReleased();

    /** Right mouse / secondary action. Ends current vertex editing; invalid/no-face objects are canceled. */
    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Input")
    void InputSecondaryAction();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Input")
    void InputInteractAction();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Input")
    void InputToggleFirstPersonAction();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Input")
    void InputToolbarScrollAction(float ScrollValue);

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Input")
    void InputToggleItemListAction();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Input")
    void InputToggleSnapModeAction();

    /** Optional vehicle input path. X = steering, Y = throttle. */
    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Input")
    void InputVehicleMoveAction(const FVector2D& MoveValue);

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Input")
    void InputVehicleThrottleAction(float Throttle);

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Input")
    void InputVehicleSteeringAction(float Steering);

    /** Useful for BP widgets that want explicit Select/Confirm buttons as well as LMB/RMB. */
    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Placement")
    void SelectCurrentTraceLocation();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Placement")
    void ConfirmCurrentPendingLocation();

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Status")
    FString BuildRuntimeStatusText() const;

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Status")
    FString BuildHUDText() const;

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Status")
    FString GetLastRuntimeMessage() const { return LastSaveMessage; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Toolbar")
    int32 GetToolbarSlotCount() const { return 7; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Toolbar")
    int32 GetSelectedToolbarSlotIndex() const { return SelectedToolbarSlotIndex; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Toolbar")
    FRuntimeToolbarItem GetToolbarItemAtSlot(int32 SlotIndex) const;

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Toolbar")
    FRuntimeToolbarItem GetSelectedToolbarItem() const;

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Toolbar")
    bool SelectToolbarSlot(int32 SlotIndex);

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Toolbar")
    void ScrollToolbarSelection(float ScrollValue);

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Toolbar")
    bool SetToolbarSlotFromAvailableItem(int32 SlotIndex, int32 AvailableItemIndex);

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Toolbar")
    bool SelectAvailableItemForCurrentToolbarSlot(int32 AvailableItemIndex, bool bCloseItemList = true);

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Toolbar")
    bool IsSelectedToolbarItemObjectCreation() const;

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Inventory")
    int32 GetAvailableItemCount() const { return AvailableItems.Num(); }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Inventory")
    FRuntimeToolbarItem GetAvailableItemAtIndex(int32 Index) const;

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Inventory")
    void ToggleItemListWindow();

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Inventory")
    void SetItemListWindowOpen(bool bOpen);

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Inventory")
    bool IsItemListWindowOpen() const { return bItemListWindowOpen; }

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Mode")
    void SetRuntimePlayMode(ERuntimePlayMode NewMode);

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Mode")
    ERuntimePlayMode GetRuntimePlayMode() const { return RuntimePlayMode; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Status")
    FString GetCurrentPrefabName() const;

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Status")
    FString GetCurrentWeaponName() const;

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Assets")
    int32 GetPrefabCount() const { return PrefabFiles.Num(); }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Assets")
    int32 GetWeaponCount() const { return WeaponFiles.Num(); }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Assets")
    int32 GetCurrentPrefabIndex() const { return CurrentPrefabIndex; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Assets")
    int32 GetCurrentWeaponIndex() const { return CurrentWeaponIndex; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Assets")
    FString GetPrefabNameAtIndex(int32 Index) const;

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Assets")
    FString GetWeaponNameAtIndex(int32 Index) const;

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Assets")
    FString GetPrefabPathAtIndex(int32 Index) const;

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Assets")
    FString GetWeaponPathAtIndex(int32 Index) const;

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Status")
    bool IsSnapEnabled() const { return bSnapToGrid; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Status")
    float GetGridSize() const { return GridSize; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Status")
    ERuntimeToolMode GetCurrentToolMode() const { return CurrentMode; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Placement")
    FVector GetPreviewPlacementLocation() const { return LastPreviewLocation; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Placement")
    FVector GetCurrentCrosshairWorldLocation() const { return LastPreviewLocation; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Placement")
    bool HasCrosshairBlockingHit() const { return bLastTraceBlockingHit; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Placement")
    bool HasCrosshairPlacementLocation() const { return bLastTraceHasPlacementLocation; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Placement")
    bool IsCrosshairFreeSpacePlacement() const { return bLastTraceHasPlacementLocation && bLastTraceUsedFreeSpace; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Placement")
    AActor* GetCrosshairHitActor() const;

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Placement")
    bool HasPendingPlacementSelection() const { return bHasPendingEmptyObjectLocation || bHasPendingVertexLocation; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Placement")
    FVector GetPendingPlacementSelection() const;

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Placement")
    void ClearPendingPlacementSelection();

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Generated Mesh")
    bool IsEditingGeneratedMesh() const;

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Generated Mesh")
    ARuntimeEditableMeshActor* GetCurrentEditableMeshActor() const { return CurrentEditableActor.Get(); }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Generated Mesh")
    int32 GetCurrentEditableMeshVertexCount() const;

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Generated Mesh")
    int32 GetCurrentEditableMeshTriangleCount() const;

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Generated Mesh")
    bool IsCurrentEditableMeshTopologyValid() const;

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Generated Mesh")
    int32 GetHighlightedEditableVertexIndex() const { return HighlightedEditableVertexIndex; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Generated Mesh")
    bool IsMovingEditableVertex() const { return bMovingHighlightedEditableVertex; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Generated Mesh")
    bool IsVertexPrimaryPressActive() const { return bPrimaryVertexPressActive; }

    UFUNCTION(BlueprintPure, Category="Runtime Gameplay|Generated Mesh")
    int32 GetConnectedEditableVertexSourceIndex() const { return ConnectedEditableVertexSourceIndex; }

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay|Generated Mesh")
    void GetSpawnedGeneratedMeshActors(TArray<ARuntimeEditableMeshActor*>& OutActors) const;

    /** Blueprint widgets can call this to get the active manager without depending on C++ HUD creation. */
    UFUNCTION(BlueprintPure, Category="Runtime Gameplay", meta=(WorldContext="WorldContextObject"))
    static ARuntimeGameplayManager* FindRuntimeGameplayManager(const UObject* WorldContextObject);

    UPROPERTY(BlueprintAssignable, Category="Runtime Gameplay|Events")
    FRuntimeGameplayStateChanged OnRuntimeStateChanged;

    UPROPERTY(BlueprintAssignable, Category="Runtime Gameplay|Events")
    FRuntimeGameplayMessageChanged OnRuntimeMessageChanged;

    UPROPERTY(BlueprintAssignable, Category="Runtime Gameplay|Events")
    FRuntimeToolbarChanged OnRuntimeToolbarChanged;

    UPROPERTY(BlueprintAssignable, Category="Runtime Gameplay|Events")
    FRuntimeItemListWindowChanged OnRuntimeItemListWindowChanged;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Tick(float DeltaSeconds) override;

private:
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Runtime Gameplay|Grid", meta=(AllowPrivateAccess="true"))
    TObjectPtr<USceneComponent> Root;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Runtime Gameplay|Grid", meta=(AllowPrivateAccess="true"))
    TObjectPtr<UProceduralMeshComponent> PlacementGridComponent;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Grid", meta=(AllowPrivateAccess="true"))
    TObjectPtr<UMaterialInterface> PlacementGridMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Grid", meta=(AllowPrivateAccess="true", ClampMin="1.0", Units="cm"))
    float PlacementGridSpacing = 100.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Grid", meta=(AllowPrivateAccess="true", ClampMin="0.25", Units="cm"))
    float PlacementGridLineThickness = 0.55f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Grid", meta=(AllowPrivateAccess="true", ClampMin="100.0", Units="cm"))
    float PlacementGridMaxRadius = 300.0f;

    // The grid is intentionally minimal: only center axes plus small 1m ticks, fading by 3 cells.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Grid", meta=(AllowPrivateAccess="true", ClampMin="100.0", Units="cm"))
    float PlacementGridStrongRadius = 100.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Grid", meta=(AllowPrivateAccess="true", ClampMin="100.0", Units="cm"))
    float PlacementGridFadeRadius = 300.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Classes", meta=(AllowPrivateAccess="true"))
    TSubclassOf<ARuntimePrefabActor> PrefabActorClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Classes", meta=(AllowPrivateAccess="true"))
    TSubclassOf<ARuntimeEditableMeshActor> EditableMeshActorClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Classes", meta=(AllowPrivateAccess="true"))
    TSubclassOf<ARuntimeVehiclePawn> VehiclePawnClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Classes", meta=(AllowPrivateAccess="true"))
    TSubclassOf<ARuntimeWeaponActor> WeaponActorClass;

    // Legacy placement distance kept so older Blueprint defaults do not lose the property.
    // The center-crosshair cursor now uses CrosshairCollisionTraceDistance and FreeSpacePlacementDistance below.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Placement", meta=(AllowPrivateAccess="true"))
    float PlacementTraceDistance = 1000.0f;

    // Only this short distance is checked for blocking collision under the center crosshair.
    // If no blocking hit is found in this range, the cursor can still resolve to a free-space point.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Placement", meta=(AllowPrivateAccess="true", ClampMin="0.0", Units="cm"))
    float CrosshairCollisionTraceDistance = 1000.0f;

    // Hard cap for free-space placement when the collision trace does not hit anything.
    // 1000 cm is 10 meters in Unreal units.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Placement", meta=(AllowPrivateAccess="true", ClampMin="1.0", Units="cm"))
    float FreeSpacePlacementDistance = 1000.0f;

    // When true, a missed collision trace becomes a valid air placement point at FreeSpacePlacementDistance.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Placement", meta=(AllowPrivateAccess="true"))
    bool bAllowFreeSpacePlacement = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Placement", meta=(AllowPrivateAccess="true"))
    float GridSize = 100.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Placement", meta=(AllowPrivateAccess="true"))
    float SurfacePlacementOffset = 2.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Placement", meta=(AllowPrivateAccess="true"))
    float VertexSelectionRayDistance = 28.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Placement", meta=(AllowPrivateAccess="true"))
    float VertexDragHoldSeconds = 0.18f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Placement", meta=(AllowPrivateAccess="true"))
    float VertexDragStartDistance = 18.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Vehicle", meta=(AllowPrivateAccess="true"))
    float VehicleEnterDistance = 450.0f;

    /** Temporary kill-switch: hides and blocks the hand-built vertex/object creation tool without removing the code. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Object Creation", meta=(AllowPrivateAccess="true"))
    bool bEnableObjectVertexCreation = false;

    /** Periodically saves runtime placed prefabs/vehicles/generated entities. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Save", meta=(AllowPrivateAccess="true"))
    bool bAutoSaveRuntimeScene = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Save", meta=(AllowPrivateAccess="true", ClampMin="5.0", Units="s"))
    float RuntimeSceneAutoSaveIntervalSeconds = 60.0f;

    /** Saves entities one last time when this manager leaves the world. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Save", meta=(AllowPrivateAccess="true"))
    bool bSaveRuntimeSceneOnEndPlay = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Gameplay|Mode", meta=(AllowPrivateAccess="true"))
    ERuntimePlayMode RuntimePlayMode = ERuntimePlayMode::Creator;

    UPROPERTY()
    TObjectPtr<ARuntimeEditableMeshActor> CurrentEditableActor;

    UPROPERTY()
    TObjectPtr<ARuntimeEditableMeshActor> PendingEmptyObjectPreviewActor;

    UPROPERTY()
    TObjectPtr<ARuntimeWeaponActor> EquippedWeapon;

    UPROPERTY()
    TArray<TObjectPtr<ARuntimePrefabActor>> SpawnedPrefabs;

    UPROPERTY()
    TArray<TObjectPtr<ARuntimeEditableMeshActor>> SpawnedGeneratedMeshes;

    UPROPERTY()
    TArray<TObjectPtr<ARuntimeVehiclePawn>> SpawnedVehicles;

    TArray<FString> PrefabFiles;
    TArray<FString> VehicleFiles;
    TArray<FString> WeaponFiles;
    TArray<FRuntimeToolbarItem> AvailableItems;
    TArray<FRuntimeToolbarItem> ToolbarSlots;
    int32 SelectedToolbarSlotIndex = 0;
    int32 CurrentPrefabIndex = 0;
    int32 CurrentWeaponIndex = 0;
    ERuntimeToolMode CurrentMode = ERuntimeToolMode::None;
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
    FRuntimeGeneratedMeshRecord OriginalEditableMeshRecord;
    FVector CachedPlacementGridCenter = FVector::ZeroVector;
    float CachedPlacementGridRadius = 0.0f;
    bool bPlacementGridBuilt = false;
    bool bIsSavingRuntimeScene = false;
    FTimerHandle RuntimeSceneAutoSaveTimerHandle;

    void ScanRuntimeFolders();
    void EnsureRuntimeFolders() const;
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
    bool DoesRuntimeAssetFileContainWheelTag(const FString& FilePath) const;
    FString GetRuntimeAssetDisplayName(const FString& AssetPath) const;
    void AutoSaveRuntimeScene();
    void ClearPlacementGridMesh();
    FString MakeRuntimeObjectName(const FString& BaseName, ERuntimePlacedObjectKind Kind) const;
    int32 CountExistingBaseName(const FString& BaseName, ERuntimePlacedObjectKind Kind) const;
    bool TryUseObjectCreationItemAtCrosshair();
    bool CloseCurrentEditableMeshForToolChange();
    void PlaceCurrentPrefab(const FVector& Location);
    void PlaceEmptyObject(const FVector& Location, ARuntimeEditableMeshActor* ExistingPreviewActor = nullptr);
    void BeginEditingExistingMesh(ARuntimeEditableMeshActor* MeshActor);
    void AddVertexToEditableObject(const FVector& Location);
    bool AddExistingVertexToEditableObject(int32 ExistingVertexIndex);
    bool FinishOrCancelCurrentVertexEditing();
    void PlaceVehicle(const FVector& Location, const FString& SourceFile = FString());
    void TryEnterOrExitVehicle();
    void CollectSceneRecords(TArray<FRuntimePlacedObjectRecord>& OutPlaced, TArray<FRuntimeGeneratedMeshRecord>& OutMeshes) const;
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
    ARuntimeEditableMeshActor* GetEditableMeshFromHit(const FHitResult& Hit) const;
    void BuildAvailableItems();
    void InitializeToolbarSlotsIfNeeded();
    void ReconcileToolbarSlotsWithAvailableItems();
    void ApplySelectedToolbarItem(bool bBroadcastChange = true);
    FRuntimeToolbarItem MakeToolbarItem(ERuntimeToolbarItemKind Kind, const FString& DisplayName, const FString& SourcePath = FString(), int32 SourceIndex = INDEX_NONE) const;
    int32 FindAvailableItemIndexMatching(const FRuntimeToolbarItem& Item) const;
    bool IsObjectCreationItem(const FRuntimeToolbarItem& Item) const;
    void NotifyRuntimeStateChanged();
    void NotifyToolbarChanged();
};
