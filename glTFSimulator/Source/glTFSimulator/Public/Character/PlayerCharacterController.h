// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file PlayerCharacterController.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "UI/CreatorHUDWidget.h"
#include "UI/PauseMenuWidget.h"
#include "UI/SettingsMenuWidget.h"
#include "TimerManager.h"
#include "PlayerCharacterController.generated.h"

class APawn;
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
    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input")
    TObjectPtr<UInputMappingContext> MappingContext = nullptr;

    /** Higher priorities override lower priorities when contexts conflict. */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input")
    int32 Priority = 50;
};

/**
 * Project-level PlayerController input router.
 *
 * Character movement, camera input, vehicle input, and pause can be received from
 * Enhanced Input InputAction assets. Gameplay tool selection, static/weapon selection,
 * snap, and scene saving are intentionally handled by a Blueprint UserWidget
 * instead of separate InputAction fields. World placement uses the primary mouse press.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API APlayerCharacterController : public APlayerController
{
    GENERATED_BODY()

public:
    APlayerCharacterController();

    /** Console frontend for weather/time/tp. Parsing/execution lives in SimulatorCommandSubsystem. */
    virtual bool ProcessConsoleExec(const TCHAR* Cmd, FOutputDevice& Ar, UObject* Executor) override;

    UFUNCTION(Server, Reliable)
    void ServerExecuteSimulatorCommand(const FString& CommandLine);

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

    /** Replaces the old GameManager RightMouseButton BindKey path. */
    UFUNCTION(BlueprintCallable, Category="Input|Mouse")
    void Input_SecondaryPressed();

    /** Replaces the old GameManager F BindKey path. */
    UFUNCTION(BlueprintCallable, Category="Input|Vehicle")
    void Input_InteractPressed();

    /** Replaces the old GameManager V BindKey path. */
    UFUNCTION(BlueprintCallable, Category="Input|Character")
    void Input_ToggleFirstPersonPressed();

    /** U key or assigned InputAction. Cycles through built character records in the active .gworld. */
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

    /** Reasserts gameplay input after the controller possesses a newly loaded or replaced pawn. */
    virtual void OnPossess(APawn* InPawn) override;
    /** Owning-client counterpart to OnPossess for replicated Pawn assignment. */
    virtual void AcknowledgePossession(APawn* InPawn) override;

    /** Registers only this world's first player's character with shared streaming/save systems. */
    void RegisterPrimaryCharacterPawn(APawn* InPawn);

public:
    /** Primary IMC slot. Assign the project's main mapping context in a Blueprint child. */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Mapping", meta=(DisplayName="Primary Input Mapping Context"))
    TObjectPtr<UInputMappingContext> InputMappingContext;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Mapping", meta=(DisplayName="Primary Input Mapping Priority"))
    int32 InputMappingPriority = 50;

    /** Optional extra IMCs. Useful when Character, Vehicle, and System actions are separated. */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Mapping")
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

    /** Optional runtime instance injection. Missing top-level UI is auto-created from AssetRegistryClass in BeginPlay. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD")
    void SetCreatorHUDWidget(UCreatorHUDWidget* InWidget);

    UFUNCTION(BlueprintCallable, Category="UI")
    void SetDebugWidget(UUserWidget* InWidget);

    UFUNCTION(BlueprintCallable, Category="Pause")
    void SetPauseMenuWidget(UPauseMenuWidget* InWidget);

    UFUNCTION(BlueprintCallable, Category="Pause")
    void SetSettingsMenuWidget(USettingsMenuWidget* InWidget);

    /** Hides the registered Creator HUD without destroying the Blueprint-owned instance. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD")
    void RemoveCreatorHUD();

    UFUNCTION(BlueprintPure, Category="Creator HUD")
    UUserWidget* GetCreatorHUDWidget() const { return CreatorHUDWidget.Get(); }

    UFUNCTION(BlueprintCallable, Category="Pause")
    void OpenPauseMenu();

    UFUNCTION(BlueprintCallable, Category="Pause")
    void ClosePauseMenu(bool bResumeGame = true);

    UFUNCTION(BlueprintCallable, Category="Pause")
    void ShowSettingsMenuFromPause();

    UFUNCTION(BlueprintCallable, Category="Pause")
    void ReturnToPauseMenuFromSettings();

    UFUNCTION(BlueprintCallable, Category="Pause|Navigation")
    void ExitToWorldSelectionFromPauseMenu();

    /** Starts pause-menu travel and reports whether this controller now owns an accepted request. */
    bool TryExitToWorldSelectionFromPauseMenu();

    /** True after an accepted request, including duplicate listeners fired by the same button click. */
    bool IsMenuWorldTravelPending() const { return bMenuWorldTravelPending; }

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

    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Character Actions")
    TObjectPtr<UInputAction> MoveAction;

    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Character Actions")
    TObjectPtr<UInputAction> LookAction;

    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Character Actions")
    TObjectPtr<UInputAction> JumpAction;

    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Character Actions")
    TObjectPtr<UInputAction> SprintAction;

    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Character Actions")
    TObjectPtr<UInputAction> CrouchAction;

    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Character Actions")
    TObjectPtr<UInputAction> FlyAction;

    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Character Actions")
    TObjectPtr<UInputAction> RagdollAction;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Input|Enhanced Input|Character Actions")
    float LookSensitivity = 1.0f;

    /** Optional action for entering/exiting vehicles. Tool buttons remain UI-only. */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Vehicle Actions", meta=(DisplayName="Vehicle Enter/Exit Action"))
    TObjectPtr<UInputAction> InteractAction;

    /** Optional action for camera/character first-person toggle. */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Character Actions", meta=(DisplayName="Toggle First Person Action"))
    TObjectPtr<UInputAction> ToggleFirstPersonAction;

    /** Optional action for cycling the streamed player character mesh, usually U. */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Character Actions", meta=(DisplayName="Change Character Action"))
    TObjectPtr<UInputAction> ChangeCharacterAction;

    /** Axis1D action for Minecraft-style 7-slot toolbar scroll. */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Creator Toolbar", meta=(DisplayName="Toolbar Scroll Action"))
    TObjectPtr<UInputAction> ToolbarScrollAction;

    /** Boolean action for opening/closing the full item list window, usually E. */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Creator Toolbar", meta=(DisplayName="Toggle Item List Action"))
    TObjectPtr<UInputAction> ToggleItemListAction;

    /** Boolean action for toggling grid snap while creating/editing created objects. */
    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Creator Toolbar", meta=(DisplayName="Snap Toggle Action"))
    TObjectPtr<UInputAction> SnapAction;

    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Vehicle Actions")
    TObjectPtr<UInputAction> VehicleMoveAction;

    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Vehicle Actions")
    TObjectPtr<UInputAction> VehicleThrottleAction;

    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Vehicle Actions")
    TObjectPtr<UInputAction> VehicleSteeringAction;

    UPROPERTY(Transient, BlueprintReadOnly, Category="Input|Enhanced Input|Vehicle Actions")
    TObjectPtr<UInputAction> VehicleStopAction;

    UPROPERTY(Transient, BlueprintReadOnly, Category = "Input|Enhanced Input|System")
    TObjectPtr<UInputAction> PauseAction;

    /** Boolean action for toggling the debug overlay. Assign the InputAction asset directly. */
    UPROPERTY(Transient, BlueprintReadOnly, Category = "Input|Enhanced Input|System")
    TObjectPtr<UInputAction> DebugAction;

private:
    /** Registry-created or explicitly overridden top-level widget instances. */
    UPROPERTY(Transient)
    TObjectPtr<UUserWidget> DebugWidget;

    UPROPERTY(Transient)
    TObjectPtr<UPauseMenuWidget> PauseMenuWidget;

    UPROPERTY(Transient)
    TObjectPtr<USettingsMenuWidget> SettingsMenuWidget;

    void ResolveCentralAssets();
    /** Creates missing gameplay UI directly from the central registry and registers the loading widget with GameManager. */
    void InitializeRegistryDrivenUI();
    void BindConfiguredInputActions();
    /** Direct placement mouse buttons are intentional gameplay input, independent of Enhanced Input mappings. */
    void BindDirectMouseInputs();
    int32 CountAssignedEnhancedInputActions() const;
    int32 CountConfiguredInputMappingContexts() const;

    void UpdateFromGameUpdate(float DeltaSeconds);
    void StopGameplayMotionForUI();

    /** Applies ignore-move/look exactly once so repeated loading/UI callbacks cannot stack controller locks. */
    void SetGameplayInputSuppressed(bool bSuppress);

    /** Reasserts viewport focus, mapping contexts, and controller input one tick after UI removal. */
    void FinalizeGameplayInputRecovery();
    void LockInputForMenuWorldTravel();
    TSoftObjectPtr<UWorld> ResolveMainWorld() const;
    void RestorePauseMenuAfterRejectedTravel();
    void ArmMenuWorldTravelWatchdog();
    void HandleMenuWorldTravelWatchdogExpired();
    void ReapplyHeldGameplayInput();
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

    /** Previous subsystem pause state used to detect the exact frame gameplay resumes. */
    bool bPrevGamePaused = false;

    /** Previous subsystem loading state used to recover keyboard/look input after async world loading. */
    bool bPrevWorldLoading = false;
    bool bIsDebug = false;
    bool bUIInputMode = false;
    /** Blocks duplicate pause/back/Blueprint callbacks while a menu-world OpenLevel is pending. */
    bool bMenuWorldTravelPending = false;
    FTimerHandle MenuWorldTravelWatchdogHandle;
    FTimerHandle GameplayInputRecoveryHandle;
    static constexpr float MenuWorldTravelWatchdogSeconds = 5.0f;
    bool bSprintInputHeld = false;
    bool bCrouchInputHeld = false;
    bool bEnhancedInputComponentWasAvailable = false;
    bool bAnyInputMappingContextApplied = false;
    /** Prevents SetIgnoreMoveInput/SetIgnoreLookInput from accumulating an unmatched stack. */
    bool bGameplayInputSuppressed = false;
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


    /** Registry-created Creator HUD, or an explicitly registered override. */
    UPROPERTY(Transient)
    TObjectPtr<UCreatorHUDWidget> CreatorHUDWidget;

};
