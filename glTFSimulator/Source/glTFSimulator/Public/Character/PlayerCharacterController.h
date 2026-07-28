// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "UI/CreatorHUDWidget.h"
#include "System/GameManagerActor.h"
#include "TimerManager.h"
#include "PlayerCharacterController.generated.h"

class AGameManagerActor;
class UGameManagerSubSystem;
class UInputAction;
class UInputMappingContext;
struct FInputActionValue;
class UUserWidget;
class UPauseMenuWidget;
class USettingsMenuWidget;
class UGameUpdateSubSystem;
class UWorld;

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FPlayerInputMappingContextConfig
{
    GENERATED_BODY()

public:
    /** Enhanced Input Mapping Context asset to add for this controller. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input|Enhanced Input")
    TObjectPtr<UInputMappingContext> MappingContext = nullptr;

    /** Higher priorities override lower priorities when contexts conflict. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input|Enhanced Input")
    int32 Priority = 50;
};

/**
 * Project-level PlayerController input router.
 *
 * Character movement, camera input, vehicle input, and pause can be received from
 * Enhanced Input InputAction assets. Gameplay tool selection, prefab/weapon selection,
 * snap, and scene saving are intentionally handled by a Blueprint UserWidget
 * instead of separate InputAction fields. World placement uses mouse
 * input: left pressed/released drives click-vs-hold editing, right click finishes vertex editing.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API APlayerCharacterController : public APlayerController
{
    GENERATED_BODY()

public:
    APlayerCharacterController();

    /** 2D axis. X = right/left, Y = forward/back. Also drives the vehicle when possessed. */
    UFUNCTION(BlueprintCallable, Category="Input|Character")
    void Input_Move(const FVector2D& MoveValue);

    /** 2D axis. X = yaw, Y = pitch. */
    UFUNCTION(BlueprintCallable, Category="Input|Character")
    void Input_Look(const FVector2D& LookValue);

    UFUNCTION(BlueprintCallable, Category="Input|Character")
    void Input_JumpStarted();

    UFUNCTION(BlueprintCallable, Category="Input|Character")
    void Input_JumpCompleted();

    UFUNCTION(BlueprintCallable, Category="Input|Character")
    void Input_SprintStarted();

    UFUNCTION(BlueprintCallable, Category="Input|Character")
    void Input_SprintCompleted();

    UFUNCTION(BlueprintCallable, Category="Input|Character")
    void Input_CrouchStarted();

    UFUNCTION(BlueprintCallable, Category="Input|Character")
    void Input_CrouchCompleted();

    UFUNCTION(BlueprintCallable, Category="Input|Character")
    void Input_FlyPressed();

    UFUNCTION(BlueprintCallable, Category="Input|Character")
    void Input_RagdollPressed();

    /** Replaces the old GameManager LeftMouseButton pressed path. */
    UFUNCTION(BlueprintCallable, Category="Input|Mouse")
    void Input_PrimaryPressed();

    /** LeftMouseButton released. Required for click-vs-hold vertex editing. */
    UFUNCTION(BlueprintCallable, Category="Input|Mouse")
    void Input_PrimaryReleased();

    /** Replaces the old GameManager RightMouseButton BindKey path. */
    UFUNCTION(BlueprintCallable, Category="Input|Mouse")
    void Input_SecondaryPressed();

    /** Replaces the old GameManager F BindKey path. */
    UFUNCTION(BlueprintCallable, Category="Input|Vehicle")
    void Input_InteractPressed();

    /** Replaces the old GameManager V BindKey path. */
    UFUNCTION(BlueprintCallable, Category="Input|Character")
    void Input_ToggleFirstPersonPressed();

    /** U key or assigned InputAction. Cycles through player GLB files managed by glTFStreamSubSystem. */
    UFUNCTION(BlueprintCallable, Category="Input|Character")
    void Input_ChangeCharacterPressed();

    /** Mouse wheel / Axis1D. Positive wheel selects the previous toolbar slot, negative selects the next slot. */
    UFUNCTION(BlueprintCallable, Category="Input|Toolbar")
    void Input_ToolbarScroll(float ScrollValue);

    /** E key or assigned InputAction. Opens/closes the full item list window for your BP UserWidget. */
    UFUNCTION(BlueprintCallable, Category="Input|Toolbar")
    void Input_ToggleItemListPressed();

    /** Assigned snap InputAction or fallback G. Toggles grid snap while editing/placing. */
    UFUNCTION(BlueprintCallable, Category="Input|Toolbar")
    void Input_SnapPressed();

    /** Optional 2D vehicle axis. X = steering, Y = throttle. */
    UFUNCTION(BlueprintCallable, Category="Input|Vehicle")
    void Input_VehicleMove(const FVector2D& MoveValue);

    UFUNCTION(BlueprintCallable, Category="Input|Vehicle")
    void Input_VehicleThrottle(float Throttle);

    UFUNCTION(BlueprintCallable, Category="Input|Vehicle")
    void Input_VehicleSteering(float Steering);

    UFUNCTION(BlueprintCallable, Category="Input|Vehicle")
    void Input_VehicleStop();

    UFUNCTION(BlueprintCallable, Category="Input|Character")
    void ClearLatchedMovementInput();

    UFUNCTION(BlueprintCallable, Category = "Input|System")
    void Input_DebugPressed();

    UFUNCTION(BlueprintCallable, Category = "Input|System")
    void Input_PausePressed();

    /** Restores normal gameplay mouse capture: cursor hidden, camera look enabled. */
    UFUNCTION(BlueprintCallable, Category="Input|Mouse")
    void ApplyGameInputMode();

    /** Optional UI mode for pause menus or deliberately clickable widgets. */
    UFUNCTION(BlueprintCallable, Category="Input|Mouse")
    void ApplyUIInputMode(UUserWidget* WidgetToFocus);

    /** Loading-screen input mode: cursor visible, never locked, never hidden during capture. */
    UFUNCTION(BlueprintCallable, Category="Input|Mouse")
    void ApplyLoadingInputMode(UUserWidget* WidgetToFocus);

    /** Applies all Enhanced Input Mapping Context assets assigned in this controller or its Blueprint subclass. */
    UFUNCTION(BlueprintCallable, Category="Input|Enhanced Input")
    void ApplyConfiguredInputMappingContexts();

    /** Re-applies assigned mapping contexts and prints a diagnostic message. Call this from BP after changing IA/IMC values at runtime. */
    UFUNCTION(BlueprintCallable, Category="Input|Enhanced Input")
    void RefreshConfiguredEnhancedInput();

    /** Human-readable status showing which controller, mapping contexts, and action bindings are active. */
    UFUNCTION(BlueprintPure, Category="Input|Enhanced Input")
    FString GetInputSetupStatus() const;

    UFUNCTION(BlueprintCallable, Category="Input|Enhanced Input")
    void PrintInputSetupStatus() const;

    UFUNCTION(BlueprintPure, Category="Input|Enhanced Input")
    FString GetInputFixVersion() const;

    UFUNCTION(BlueprintCallable, Category="Gameplay")
    UGameManagerSubSystem* GetGameManager();

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void SetupInputComponent() override;
    virtual void BeginPlayingState() override;

public:
    /** Backward-compatible single IMC slot. Assign your main IMC here in a Blueprint child. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Mapping", meta=(DisplayName="Primary Input Mapping Context"))
    TObjectPtr<UInputMappingContext> InputMappingContext;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Mapping", meta=(DisplayName="Primary Input Mapping Priority"))
    int32 InputMappingPriority = 50;

    /** Optional extra IMCs. Useful when Character, Vehicle, and System actions are separated. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Mapping")
    TArray<FPlayerInputMappingContextConfig> AdditionalInputMappingContexts;

    /** Automatically add the assigned IMCs in BeginPlay. Disable only if a Blueprint applies them manually. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Mapping")
    bool bApplyInputMappingContextsOnBeginPlay = true;

    /** Clears existing Enhanced Input mappings before adding the configured contexts. Off by default to avoid removing project defaults. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Mapping")
    bool bClearExistingInputMappingsBeforeAdding = false;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Mouse")
    bool bForceGameInputModeOnBeginPlay = true;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Mouse")
    bool bHideMouseCursorDuringGameplay = true;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Fallback Keys")
    bool bEnableFallbackKeyBindings = true;

    /** When true, legacy fallback keys are skipped for actions that have an InputAction assigned, preventing double execution. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Fallback Keys")
    bool bBindFallbackKeysOnlyForUnassignedInputActions = true;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Fallback Keys")
    bool bAutoSpawnGameManager = true;

    /** BP subclass of AGameManagerActor to spawn when no manager is placed in the level. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Gameplay")
    TSubclassOf<AGameManagerActor> GameManagerActorClass;

    /** Optional WBP subclass of UCreatorHUDWidget. Nothing is created when this is empty. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Creator HUD")
    TSubclassOf<UCreatorHUDWidget> CreatorHUDWidgetClass;

    /** Disabled by default because the Creator HUD is now expected to be created explicitly by your own WBP/Blueprint flow. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Creator HUD")
    bool bAutoCreateCreatorHUD = false;

    /** ZOrder used when the explicitly created Creator HUD is added to the viewport. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Creator HUD")
    int32 CreatorHUDZOrder = 5;

    /** Creates the explicitly assigned Creator HUD widget if it is missing, then adds it to the viewport. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD")
    UUserWidget* CreateCreatorHUD();

    /** Removes the stored Creator HUD instance from the viewport. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD")
    void RemoveCreatorHUD();

    /** Returns the current Creator HUD instance, if one exists. */
    UFUNCTION(BlueprintPure, Category="Creator HUD")
    UUserWidget* GetCreatorHUDWidget() const { return CreatorHUDWidget.Get(); }

    /** Optional WBP subclass of UPauseMenuWidget. Nothing is created when this is empty. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Pause")
    TSubclassOf<UPauseMenuWidget> PauseMenuWidgetClass;

    /** Optional WBP subclass of USettingsMenuWidget. Nothing is created when this is empty. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Pause")
    TSubclassOf<USettingsMenuWidget> SettingsMenuWidgetClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Pause")
    int32 PauseMenuZOrder = 100;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Pause")
    int32 SettingsMenuZOrder = 110;

    /** Menu world that owns the world-selection flow. Assign the world asset directly. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Pause|Navigation")
    TSoftObjectPtr<UWorld> WorldSelectionWorld;

    /** Menu world shown when leaving the world-selection screen. Assign the world asset directly. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Pause|Navigation")
    TSoftObjectPtr<UWorld> MainMenuWorld;

    UFUNCTION(BlueprintCallable, Category="Pause")
    UUserWidget* CreatePauseMenu();

    UFUNCTION(BlueprintCallable, Category="Pause")
    UUserWidget* CreateSettingsMenu();

    UFUNCTION(BlueprintCallable, Category="Pause")
    void OpenPauseMenu();

    UFUNCTION(BlueprintCallable, Category="Pause")
    void ClosePauseMenu(bool bResumeGame = true);

    UFUNCTION(BlueprintCallable, Category="Pause")
    void ShowSettingsMenuFromPause();

    UFUNCTION(BlueprintCallable, Category="Pause")
    void ReturnToPauseMenuFromSettings();

    // Legacy wrapper. It now routes gameplay exits to the world-selection screen.
    UFUNCTION(BlueprintCallable, Category="Pause|Navigation")
    void ExitToMainWorldFromPauseMenu();

    UFUNCTION(BlueprintCallable, Category="Pause|Navigation")
    void ExitToWorldSelectionFromPauseMenu();

    /** Starts pause-menu travel and reports whether this controller now owns an accepted request. */
    bool TryExitToWorldSelectionFromPauseMenu();

    /** Returns whether a directly assigned or StartActor-registered world-selection world is available. */
    bool CanExitToWorldSelectionFromPauseMenu() const;

    /** True after an accepted request, including duplicate listeners fired by the same button click. */
    bool IsMenuWorldTravelPending() const { return bMenuWorldTravelPending; }

    // Assign this to the Back button on the world-selection widget.
    UFUNCTION(BlueprintCallable, Category="Pause|Navigation")
    void ReturnToMainMenuFromWorldSelection();

    UFUNCTION(BlueprintPure, Category="Pause")
    UUserWidget* GetPauseMenuWidget() const { return PauseMenuWidget.Get(); }

    UFUNCTION(BlueprintPure, Category="Pause")
    UUserWidget* GetSettingsMenuWidget() const { return SettingsMenuWidget.Get(); }

    /** Left/right mouse support for gameplay placement. Tool selection and save are handled by UI buttons. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Mouse")
    bool bBindMouseButtons = true;

    /** Writes a clear Output Log message so you can verify that the rebuilt C++ class is actually running. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Diagnostics")
    bool bLogInputSetup = true;

    /** Also displays the diagnostic message on screen. Disabled by default to avoid the green startup message. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Diagnostics")
    bool bShowInputSetupOnScreen = false;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Character Actions")
    TObjectPtr<UInputAction> MoveAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Character Actions")
    TObjectPtr<UInputAction> LookAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Character Actions")
    TObjectPtr<UInputAction> JumpAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Character Actions")
    TObjectPtr<UInputAction> SprintAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Character Actions")
    TObjectPtr<UInputAction> CrouchAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Character Actions")
    TObjectPtr<UInputAction> FlyAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Character Actions")
    TObjectPtr<UInputAction> RagdollAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Character Actions")
    float LookSensitivity = 1.0f;

    /** Optional action for entering/exiting vehicles. Tool buttons remain UI-only. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Vehicle Actions", meta=(DisplayName="Vehicle Enter/Exit Action"))
    TObjectPtr<UInputAction> InteractAction;

    /** Optional action for camera/character first-person toggle. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Character Actions", meta=(DisplayName="Toggle First Person Action"))
    TObjectPtr<UInputAction> ToggleFirstPersonAction;

    /** Optional action for cycling the streamed player character mesh, usually U. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Character Actions", meta=(DisplayName="Change Character Action"))
    TObjectPtr<UInputAction> ChangeCharacterAction;

    /** Axis1D action for Minecraft-style 7-slot toolbar scroll. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Creator Toolbar", meta=(DisplayName="Toolbar Scroll Action"))
    TObjectPtr<UInputAction> ToolbarScrollAction;

    /** Boolean action for opening/closing the full item list window, usually E. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Creator Toolbar", meta=(DisplayName="Toggle Item List Action"))
    TObjectPtr<UInputAction> ToggleItemListAction;

    /** Boolean action for toggling grid snap while creating/editing created objects. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Creator Toolbar", meta=(DisplayName="Snap Toggle Action"))
    TObjectPtr<UInputAction> SnapAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Vehicle Actions")
    TObjectPtr<UInputAction> VehicleMoveAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Vehicle Actions")
    TObjectPtr<UInputAction> VehicleThrottleAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Vehicle Actions")
    TObjectPtr<UInputAction> VehicleSteeringAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Vehicle Actions")
    TObjectPtr<UInputAction> VehicleStopAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Input|Enhanced Input|System")
    TObjectPtr<UInputAction> PauseAction;

    /** Boolean action for toggling the debug overlay. Assign the InputAction asset directly. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Input|Enhanced Input|System")
    TObjectPtr<UInputAction> DebugAction;

    // Assign the debug widget class directly in the editor.
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "UI")
    TSubclassOf<UUserWidget> DebugWidgetClass;

private:
    // Keep the created widget instance.
    UPROPERTY()
    TObjectPtr<UUserWidget> DebugWidget;

    UPROPERTY()
    TObjectPtr<UUserWidget> PauseMenuWidget;

    UPROPERTY()
    TObjectPtr<UUserWidget> SettingsMenuWidget;

    void BindConfiguredInputActions();
    void BindFallbackKeyInputs();
    bool ShouldBindFallbackKeyForAction(const UInputAction* ConfiguredAction) const;
    int32 CountAssignedEnhancedInputActions() const;
    int32 CountConfiguredInputMappingContexts() const;

    void FallbackMoveForwardPressed();
    void FallbackMoveForwardReleased();
    void FallbackMoveBackwardPressed();
    void FallbackMoveBackwardReleased();
    void FallbackMoveRightPressed();
    void FallbackMoveRightReleased();
    void FallbackMoveLeftPressed();
    void FallbackMoveLeftReleased();
    void UpdateFromGameUpdate(float DeltaSeconds);
    void UpdateFallbackMoveInput();
    void StopFallbackMovement();
    void StopGameplayMotionForUI();
    void LockInputForMenuWorldTravel();
    TSoftObjectPtr<UWorld> ResolveWorldSelectionWorld() const;
    void RestorePauseMenuAfterRejectedTravel();
    void ArmMenuWorldTravelWatchdog();
    void HandleMenuWorldTravelWatchdogExpired();
    void ReapplyHeldGameplayInput();
    void FallbackLookYaw(float Value);
    void FallbackLookPitch(float Value);
    bool ConsumeInputDebounce(double& LastInputTime);

    void HandleMoveTriggered(const FInputActionValue& Value);
    void HandleMoveCompleted(const FInputActionValue& Value);
    void HandleLookTriggered(const FInputActionValue& Value);
    void HandleToolbarScrollTriggered(const FInputActionValue& Value);
    void HandleVehicleMoveTriggered(const FInputActionValue& Value);
    void HandleVehicleMoveCompleted(const FInputActionValue& Value);
    void HandleVehicleThrottleTriggered(const FInputActionValue& Value);
    void HandleVehicleThrottleCompleted(const FInputActionValue& Value);
    void HandleVehicleSteeringTriggered(const FInputActionValue& Value);
    void HandleVehicleSteeringCompleted(const FInputActionValue& Value);

    bool bPrevGamePaused = false;
    bool bIsDebug = false;
    bool bUIInputMode = false;
    /** Blocks duplicate pause/back/Blueprint callbacks while a menu-world OpenLevel is pending. */
    bool bMenuWorldTravelPending = false;
    FTimerHandle MenuWorldTravelWatchdogHandle;
    static constexpr float MenuWorldTravelWatchdogSeconds = 5.0f;
    bool bFallbackMoveForward = false;
    bool bFallbackMoveBackward = false;
    bool bFallbackMoveRight = false;
    bool bFallbackMoveLeft = false;
    bool bSprintInputHeld = false;
    bool bCrouchInputHeld = false;
    bool bEnhancedInputComponentWasAvailable = false;
    bool bAnyInputMappingContextApplied = false;
    int32 GameUpdateTickHandle = INDEX_NONE;
    double LastPrimaryInputTime = -1.0;
    double LastSecondaryInputTime = -1.0;
    double LastInteractInputTime = -1.0;
    double LastToggleFirstPersonInputTime = -1.0;
    double LastChangeCharacterInputTime = -1.0;
    double LastToggleItemListInputTime = -1.0;
    double LastSnapInputTime = -1.0;
    double LastDebugInputTime = -1.0;

    UPROPERTY()
    TObjectPtr<UGameManagerSubSystem> SubSystem;


    /** Auto-created Creator HUD instance. Kept as UUserWidget so WBP subclasses are supported. */
    UPROPERTY()
    TObjectPtr<UUserWidget> CreatorHUDWidget;

    UPROPERTY()
    TObjectPtr<AGameManagerActor> CachedGameManagerActor;
};
