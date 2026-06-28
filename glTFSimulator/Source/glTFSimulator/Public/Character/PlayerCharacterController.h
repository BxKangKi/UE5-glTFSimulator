// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Runtime/RuntimeCreatorHUDWidget.h"
#include "PlayerCharacterController.generated.h"

class ARuntimeGameplayManager;
class UGameManagerSubSystem;
class UInputAction;
class UInputMappingContext;
struct FInputActionValue;
class UUserWidget;
class URuntimePauseMenuWidget;
class URuntimeSettingsMenuWidget;

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
 * Enhanced Input InputAction assets. Runtime tool selection, prefab/weapon selection,
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

    /** 2D axis. X = right/left, Y = forward/back. Also drives the runtime vehicle when possessed. */
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

    /** Replaces the old RuntimeGameplayManager LeftMouseButton pressed path. */
    UFUNCTION(BlueprintCallable, Category="Input|Runtime Mouse")
    void Input_RuntimePrimaryPressed();

    /** LeftMouseButton released. Required for click-vs-hold vertex editing. */
    UFUNCTION(BlueprintCallable, Category="Input|Runtime Mouse")
    void Input_RuntimePrimaryReleased();

    /** Replaces the old RuntimeGameplayManager RightMouseButton BindKey path. */
    UFUNCTION(BlueprintCallable, Category="Input|Runtime Mouse")
    void Input_RuntimeSecondaryPressed();

    /** Replaces the old RuntimeGameplayManager F BindKey path. */
    UFUNCTION(BlueprintCallable, Category="Input|Runtime Vehicle")
    void Input_RuntimeInteractPressed();

    /** Replaces the old RuntimeGameplayManager V BindKey path. */
    UFUNCTION(BlueprintCallable, Category="Input|Character")
    void Input_RuntimeToggleFirstPersonPressed();

    /** Mouse wheel / Axis1D. Positive wheel selects the previous toolbar slot, negative selects the next slot. */
    UFUNCTION(BlueprintCallable, Category="Input|Runtime Toolbar")
    void Input_RuntimeToolbarScroll(float ScrollValue);

    /** E key or assigned InputAction. Opens/closes the full item list window for your BP UserWidget. */
    UFUNCTION(BlueprintCallable, Category="Input|Runtime Toolbar")
    void Input_RuntimeToggleItemListPressed();

    /** Assigned snap InputAction or fallback G. Toggles runtime grid snap while editing/placing. */
    UFUNCTION(BlueprintCallable, Category="Input|Runtime Toolbar")
    void Input_RuntimeSnapPressed();

    /** Optional 2D vehicle axis. X = steering, Y = throttle. */
    UFUNCTION(BlueprintCallable, Category="Input|Runtime Vehicle")
    void Input_RuntimeVehicleMove(const FVector2D& MoveValue);

    UFUNCTION(BlueprintCallable, Category="Input|Runtime Vehicle")
    void Input_RuntimeVehicleThrottle(float Throttle);

    UFUNCTION(BlueprintCallable, Category="Input|Runtime Vehicle")
    void Input_RuntimeVehicleSteering(float Steering);

    UFUNCTION(BlueprintCallable, Category="Input|Runtime Vehicle")
    void Input_RuntimeVehicleStop();

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

    /** Applies all Enhanced Input Mapping Context assets assigned in this controller or its Blueprint subclass. */
    UFUNCTION(BlueprintCallable, Category="Input|Enhanced Input")
    void ApplyConfiguredInputMappingContexts();

    /** Re-applies assigned mapping contexts and prints a diagnostic message. Call this from BP after changing IA/IMC values at runtime. */
    UFUNCTION(BlueprintCallable, Category="Input|Enhanced Input")
    void RefreshConfiguredEnhancedInput();

    /** Human-readable status showing which controller, mapping contexts, and action bindings are active. */
    UFUNCTION(BlueprintPure, Category="Input|Enhanced Input")
    FString GetRuntimeInputSetupStatus() const;

    UFUNCTION(BlueprintCallable, Category="Input|Enhanced Input")
    void PrintRuntimeInputSetupStatus() const;

    UFUNCTION(BlueprintPure, Category="Input|Enhanced Input")
    FString GetRuntimeInputFixVersion() const;

    UFUNCTION(BlueprintCallable, Category="Runtime Gameplay")
    ARuntimeGameplayManager* GetRuntimeGameplayManager();

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void SetupInputComponent() override;
    virtual void BeginPlayingState() override;

public:
    /** Backward-compatible single IMC slot. Assign your main IMC here in a Blueprint child. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Mapping", meta=(DisplayName="Primary Input Mapping Context"))
    TObjectPtr<UInputMappingContext> RuntimeInputMappingContext;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Mapping", meta=(DisplayName="Primary Input Mapping Priority"))
    int32 RuntimeInputMappingPriority = 50;

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
    bool bAutoSpawnRuntimeGameplayManager = true;

    /** BP subclass of ARuntimeGameplayManager to spawn when no manager is placed in the level. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Runtime Gameplay")
    TSubclassOf<ARuntimeGameplayManager> RuntimeGameplayManagerClass;

    /** Optional WBP subclass of URuntimeCreatorHUDWidget. Nothing is created when this is empty. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Runtime Creator HUD")
    TSubclassOf<URuntimeCreatorHUDWidget> RuntimeCreatorHUDWidgetClass;

    /** Disabled by default because the Creator HUD is now expected to be created explicitly by your own WBP/Blueprint flow. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Runtime Creator HUD")
    bool bAutoCreateRuntimeCreatorHUD = false;

    /** ZOrder used when the explicitly created Creator HUD is added to the viewport. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Runtime Creator HUD")
    int32 RuntimeCreatorHUDZOrder = 5;

    /** Creates the explicitly assigned Creator HUD widget if it is missing, then adds it to the viewport. */
    UFUNCTION(BlueprintCallable, Category="Runtime Creator HUD")
    UUserWidget* CreateRuntimeCreatorHUD();

    /** Removes the stored Creator HUD instance from the viewport. */
    UFUNCTION(BlueprintCallable, Category="Runtime Creator HUD")
    void RemoveRuntimeCreatorHUD();

    /** Returns the current Creator HUD instance, if one exists. */
    UFUNCTION(BlueprintPure, Category="Runtime Creator HUD")
    UUserWidget* GetRuntimeCreatorHUDWidget() const { return RuntimeCreatorHUDWidget.Get(); }

    /** Optional WBP subclass of URuntimePauseMenuWidget. Nothing is created when this is empty. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Runtime Pause")
    TSubclassOf<URuntimePauseMenuWidget> PauseMenuWidgetClass;

    /** Optional WBP subclass of URuntimeSettingsMenuWidget. Nothing is created when this is empty. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Runtime Pause")
    TSubclassOf<URuntimeSettingsMenuWidget> SettingsMenuWidgetClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Runtime Pause")
    int32 PauseMenuZOrder = 100;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Runtime Pause")
    int32 SettingsMenuZOrder = 110;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Runtime Pause")
    FName ExitLevelName = TEXT("StartWorld");

    UFUNCTION(BlueprintCallable, Category="Runtime Pause")
    UUserWidget* CreatePauseMenu();

    UFUNCTION(BlueprintCallable, Category="Runtime Pause")
    UUserWidget* CreateSettingsMenu();

    UFUNCTION(BlueprintCallable, Category="Runtime Pause")
    void OpenPauseMenu();

    UFUNCTION(BlueprintCallable, Category="Runtime Pause")
    void ClosePauseMenu(bool bResumeGame = true);

    UFUNCTION(BlueprintCallable, Category="Runtime Pause")
    void ShowSettingsMenuFromPause();

    UFUNCTION(BlueprintCallable, Category="Runtime Pause")
    void ReturnToPauseMenuFromSettings();

    UFUNCTION(BlueprintCallable, Category="Runtime Pause")
    void ExitToStartWorldFromPauseMenu();

    UFUNCTION(BlueprintPure, Category="Runtime Pause")
    UUserWidget* GetPauseMenuWidget() const { return PauseMenuWidget.Get(); }

    UFUNCTION(BlueprintPure, Category="Runtime Pause")
    UUserWidget* GetSettingsMenuWidget() const { return SettingsMenuWidget.Get(); }

    /** Left/right mouse support for runtime placement. Tool selection and save are handled by UI buttons. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Runtime Mouse")
    bool bBindRuntimeMouseButtons = true;

    /** Writes a clear Output Log message so you can verify that the rebuilt C++ class is actually running. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Diagnostics")
    bool bLogRuntimeInputSetup = true;

    /** Also displays the diagnostic message on screen. Disabled by default to avoid the green startup message. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Diagnostics")
    bool bShowRuntimeInputSetupOnScreen = false;

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

    /** Optional action for entering/exiting runtime vehicles. Tool buttons remain UI-only. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Vehicle Actions", meta=(DisplayName="Vehicle Enter/Exit Action"))
    TObjectPtr<UInputAction> RuntimeInteractAction;

    /** Optional action for camera/character first-person toggle. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Character Actions", meta=(DisplayName="Toggle First Person Action"))
    TObjectPtr<UInputAction> RuntimeToggleFirstPersonAction;

    /** Axis1D action for Minecraft-style 7-slot toolbar scroll. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Creator Toolbar", meta=(DisplayName="Toolbar Scroll Action"))
    TObjectPtr<UInputAction> RuntimeToolbarScrollAction;

    /** Boolean action for opening/closing the full item list window, usually E. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Creator Toolbar", meta=(DisplayName="Toggle Item List Action"))
    TObjectPtr<UInputAction> RuntimeToggleItemListAction;

    /** Boolean action for toggling grid snap while creating/editing runtime objects. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Creator Toolbar", meta=(DisplayName="Snap Toggle Action"))
    TObjectPtr<UInputAction> RuntimeSnapAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Vehicle Actions")
    TObjectPtr<UInputAction> RuntimeVehicleMoveAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Vehicle Actions")
    TObjectPtr<UInputAction> RuntimeVehicleThrottleAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Vehicle Actions")
    TObjectPtr<UInputAction> RuntimeVehicleSteeringAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Vehicle Actions")
    TObjectPtr<UInputAction> RuntimeVehicleStopAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Input|Enhanced Input|System")
    TObjectPtr<UInputAction> PauseAction;

    /** Boolean action for toggling the debug overlay. Defaults to /Game/Input/Actions/IA_Debug when available. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Input|Enhanced Input|System")
    TObjectPtr<UInputAction> DebugAction;

    // Assign the WBP_Debug class in the editor.
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
    void UpdateFallbackMoveInput();
    void StopFallbackMovement();
    void FallbackLookYaw(float Value);
    void FallbackLookPitch(float Value);
    bool ConsumeRuntimeInput(double& LastInputTime);

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
    bool bRuntimeUIInputMode = false;
    bool bFallbackMoveForward = false;
    bool bFallbackMoveBackward = false;
    bool bFallbackMoveRight = false;
    bool bFallbackMoveLeft = false;
    bool bEnhancedInputComponentWasAvailable = false;
    bool bAnyInputMappingContextApplied = false;
    double LastRuntimePrimaryInputTime = -1.0;
    double LastRuntimeSecondaryInputTime = -1.0;
    double LastRuntimeInteractInputTime = -1.0;
    double LastRuntimeToggleFirstPersonInputTime = -1.0;
    double LastRuntimeToggleItemListInputTime = -1.0;
    double LastRuntimeSnapInputTime = -1.0;
    double LastRuntimeDebugInputTime = -1.0;

    UPROPERTY()
    TObjectPtr<UGameManagerSubSystem> SubSystem;


    /** Auto-created Creator HUD instance. Kept as UUserWidget so WBP subclasses are supported. */
    UPROPERTY()
    TObjectPtr<UUserWidget> RuntimeCreatorHUDWidget;

    UPROPERTY()
    TObjectPtr<ARuntimeGameplayManager> CachedRuntimeGameplayManager;
};
