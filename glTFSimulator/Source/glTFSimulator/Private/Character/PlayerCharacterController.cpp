// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Character/PlayerCharacterController.h"
#include "Character/CharacterController.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "EngineUtils.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "InputCoreTypes.h"
#include "Model/glTFStreamSubSystem.h"
#include "System/GameManagerActor.h"
#include "UI/CreatorHUDWidget.h"
#include "UI/PauseMenuWidget.h"
#include "UI/SettingsMenuWidget.h"
#include "Vehicle/VehiclePawn.h"
#include "System/GameManagerSubSystem.h"
#include "System/GameUpdateSubSystem.h"
#include "TimerManager.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Engine/Engine.h"
#include "Kismet/GameplayStatics.h"

APlayerCharacterController::APlayerCharacterController()
{
    GameManagerActorClass = nullptr;

    // InputAction assets are assigned directly in the owning Blueprint/class defaults.

    // Do not hard-load an optional debug widget from a project asset path in the native CDO.
    // Assign DebugWidgetClass in a Blueprint/defaults asset when the widget exists.
    DebugWidgetClass = nullptr;

    bAutoCreateCreatorHUD = false;
    PauseMenuWidgetClass = nullptr;
    SettingsMenuWidgetClass = nullptr;
}

void APlayerCharacterController::BeginPlay()
{
    Super::BeginPlay();
    bMenuWorldTravelPending = false;
    GetWorldTimerManager().ClearTimer(MenuWorldTravelWatchdogHandle);
    if (!IsValid(SubSystem))
    {
        SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    }
    if (bApplyInputMappingContextsOnBeginPlay)
    {
        ApplyConfiguredInputMappingContexts();
    }

    if (bAutoSpawnGameManager)
    {
        GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
        {
            if (!IsValid(SubSystem) || !SubSystem->IsWorldLoading())
            {
                GetGameManager();
            }
        }));
    }

    if (bForceGameInputModeOnBeginPlay)
    {
        if (IsValid(SubSystem) && SubSystem->IsWorldLoading())
        {
            ApplyLoadingInputMode(nullptr);
        }
        else
        {
            ApplyGameInputMode();
        }

        GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
        {
            if (IsValid(SubSystem) && SubSystem->IsWorldLoading())
            {
                ApplyLoadingInputMode(nullptr);
                return;
            }

            if (IsValid(SubSystem) && SubSystem->GetGamePaused())
            {
                return;
            }

            // Keep GameOnly input by default even while the Creator HUD is visible.
            ApplyGameInputMode();
        }));
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [this](const float DeltaSeconds)
            {
                UpdateFromGameUpdate(DeltaSeconds);
            },
            1);
    }
}


UUserWidget* APlayerCharacterController::CreateCreatorHUD()
{
    if (IsValid(SubSystem) && SubSystem->IsWorldLoading())
    {
        return nullptr;
    }

    // No native/WBP fallback class is loaded here anymore. Assign a WBP class explicitly or create the widget in Blueprint.
    if (!CreatorHUDWidgetClass)
    {
        UE_LOG(LogTemp, Verbose, TEXT("PlayerCharacterController: CreatorHUDWidgetClass is not assigned; skipping Creator HUD creation."));
        return nullptr;
    }

    // Reuse the existing HUD instance when it is still alive.
    if (IsValid(CreatorHUDWidget))
    {
        if (!CreatorHUDWidget->IsInViewport())
        {
            CreatorHUDWidget->AddToViewport(CreatorHUDZOrder);
        }
        return CreatorHUDWidget.Get();
    }

    // Only instantiate an explicitly assigned WBP class.
    CreatorHUDWidget = CreateWidget<UUserWidget>(this, CreatorHUDWidgetClass);

    // Return nullptr if widget creation fails.
    if (!IsValid(CreatorHUDWidget))
    {
        return nullptr;
    }

    // Add the HUD to the viewport.
    CreatorHUDWidget->AddToViewport(CreatorHUDZOrder);

    // Keep GameOnly input so this HUD does not interrupt crosshair-centered gameplay.
    if (!bUIInputMode)
    {
        ApplyGameInputMode();
    }

    // Return the created HUD instance.
    return CreatorHUDWidget.Get();
}

void APlayerCharacterController::RemoveCreatorHUD()
{
    // Remove the HUD from the viewport if it is valid.
    if (IsValid(CreatorHUDWidget))
    {
        CreatorHUDWidget->RemoveFromParent();
    }

    // Clear the reference so the next request can create a fresh instance.
    CreatorHUDWidget = nullptr;
}

void APlayerCharacterController::SetupInputComponent()
{
    Super::SetupInputComponent();
    if (bApplyInputMappingContextsOnBeginPlay)
    {
        ApplyConfiguredInputMappingContexts();
    }
    BindConfiguredInputActions();
    BindFallbackKeyInputs();
}

void APlayerCharacterController::BeginPlayingState()
{
    Super::BeginPlayingState();
    if (bApplyInputMappingContextsOnBeginPlay)
    {
        ApplyConfiguredInputMappingContexts();
    }
    if (bLogInputSetup)
    {
        PrintInputSetupStatus();
    }
}

void APlayerCharacterController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(MenuWorldTravelWatchdogHandle);
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
    }
    GameUpdateTickHandle = INDEX_NONE;

    Super::EndPlay(EndPlayReason);
}


void APlayerCharacterController::UpdateFromGameUpdate(float DeltaSeconds)
{
    if (bEnableFallbackKeyBindings && !bUIInputMode &&
        (bFallbackMoveForward || bFallbackMoveBackward || bFallbackMoveRight || bFallbackMoveLeft))
    {
        UpdateFallbackMoveInput();
    }
}

void APlayerCharacterController::ApplyGameInputMode()
{
    // Late UI/Blueprint callbacks must not re-enable gameplay input during level travel.
    if (bMenuWorldTravelPending)
    {
        return;
    }

    if (IsValid(SubSystem) && SubSystem->IsWorldLoading())
    {
        ApplyLoadingInputMode(nullptr);
        return;
    }

    bUIInputMode = false;
    UWidgetBlueprintLibrary::SetInputMode_GameOnly(this, false);
    bShowMouseCursor = !bHideMouseCursorDuringGameplay;
    bEnableClickEvents = false;
    bEnableMouseOverEvents = false;
    SetIgnoreLookInput(false);
    SetIgnoreMoveInput(false);
    ReapplyHeldGameplayInput();
}

void APlayerCharacterController::ApplyUIInputMode(UUserWidget* WidgetToFocus)
{
    if (bMenuWorldTravelPending)
    {
        return;
    }

    bUIInputMode = true;
    UWidgetBlueprintLibrary::SetInputMode_GameAndUIEx(
        this,
        WidgetToFocus,
        EMouseLockMode::DoNotLock,
        true,
        false);
    bShowMouseCursor = true;
    bEnableClickEvents = true;
    bEnableMouseOverEvents = true;
    StopGameplayMotionForUI();
    SetIgnoreMoveInput(true);
    SetIgnoreLookInput(true);
}

void APlayerCharacterController::ApplyLoadingInputMode(UUserWidget* WidgetToFocus)
{
    bUIInputMode = true;
    StopFallbackMovement();
    UWidgetBlueprintLibrary::SetInputMode_GameAndUIEx(
        this,
        WidgetToFocus,
        EMouseLockMode::DoNotLock,
        false,
        false);
    bShowMouseCursor = true;
    bEnableClickEvents = true;
    bEnableMouseOverEvents = true;
    StopGameplayMotionForUI();
    SetIgnoreMoveInput(true);
    SetIgnoreLookInput(true);
}

void APlayerCharacterController::StopGameplayMotionForUI()
{
    // UI/mouse-cursor mode must stop active gameplay input immediately.
    // Enhanced Input may not emit Completed events while focus is on a widget, so clear
    // the controlled pawn explicitly instead of waiting for key release callbacks.
    StopFallbackMovement();

    if (AVehiclePawn* Vehicle = Cast<AVehiclePawn>(GetPawn()))
    {
        Vehicle->ClearDriveInput();
        return;
    }

    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->ClearTransientInputState();
    }
}

void APlayerCharacterController::LockInputForMenuWorldTravel()
{
    bUIInputMode = true;
    StopGameplayMotionForUI();

    // Remove the old menu from hit testing and prevent its initiating click/key release from being
    // interpreted as gameplay input while OpenLevel is queued.
    UWidgetBlueprintLibrary::SetInputMode_GameOnly(this, false);
    SetIgnoreMoveInput(true);
    SetIgnoreLookInput(true);
    bEnableClickEvents = false;
    bEnableMouseOverEvents = false;
    bShowMouseCursor = false;
}

TSoftObjectPtr<UWorld> APlayerCharacterController::ResolveWorldSelectionWorld() const
{
    if (!WorldSelectionWorld.IsNull())
    {
        return WorldSelectionWorld;
    }

    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        return Manager->GetRegisteredWorldSelectionWorld();
    }

    return TSoftObjectPtr<UWorld>();
}

bool APlayerCharacterController::CanExitToWorldSelectionFromPauseMenu() const
{
    return !bMenuWorldTravelPending && !ResolveWorldSelectionWorld().IsNull();
}

void APlayerCharacterController::RestorePauseMenuAfterRejectedTravel()
{
    bMenuWorldTravelPending = false;
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(MenuWorldTravelWatchdogHandle);
    }

    if (!IsValid(SubSystem))
    {
        SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    }
    if (IsValid(SubSystem))
    {
        SubSystem->CancelWorldSelectionMenuTravel();
        SubSystem->SetGamePaused(true);
    }

    if (IsValid(SettingsMenuWidget))
    {
        SettingsMenuWidget->RemoveFromParent();
    }

    UUserWidget* Menu = PauseMenuWidget.Get();
    if (IsValid(Menu))
    {
        Menu->SetIsEnabled(true);
        if (!Menu->IsInViewport())
        {
            Menu->AddToViewport(PauseMenuZOrder);
        }
    }
    else
    {
        Menu = CreatePauseMenu();
    }

    if (UPauseMenuWidget* NativePauseMenu = Cast<UPauseMenuWidget>(Menu))
    {
        NativePauseMenu->ResetExitRequestState();
    }

    ApplyUIInputMode(Menu);
    bPrevGamePaused = true;
}

void APlayerCharacterController::ArmMenuWorldTravelWatchdog()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    World->GetTimerManager().ClearTimer(MenuWorldTravelWatchdogHandle);
    World->GetTimerManager().SetTimer(
        MenuWorldTravelWatchdogHandle,
        this,
        &APlayerCharacterController::HandleMenuWorldTravelWatchdogExpired,
        MenuWorldTravelWatchdogSeconds,
        false);
}

void APlayerCharacterController::HandleMenuWorldTravelWatchdogExpired()
{
    if (!bMenuWorldTravelPending || IsActorBeingDestroyed())
    {
        return;
    }

    UE_LOG(LogTemp, Error,
        TEXT("[MenuTravel] World-selection travel did not leave the gameplay world within %.1f seconds. Restoring the pause menu."),
        MenuWorldTravelWatchdogSeconds);
    RestorePauseMenuAfterRejectedTravel();
}

void APlayerCharacterController::ReapplyHeldGameplayInput()
{
    if (bUIInputMode)
    {
        return;
    }

    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        // A held sprint key does not always fire another Started event after ragdoll
        // recovery or an input-mode switch. Reapply the physical held state here.
        CharacterCtrl->Sprinting(bSprintInputHeld);
        CharacterCtrl->Crouching(bCrouchInputHeld);
    }

    if (bEnableFallbackKeyBindings && (bFallbackMoveForward || bFallbackMoveBackward || bFallbackMoveRight || bFallbackMoveLeft))
    {
        UpdateFallbackMoveInput();
    }
}

void APlayerCharacterController::ApplyConfiguredInputMappingContexts()
{
    bAnyInputMappingContextApplied = false;

    ULocalPlayer* LocalPlayer = GetLocalPlayer();
    if (!IsValid(LocalPlayer))
    {
        return;
    }

    UEnhancedInputLocalPlayerSubsystem* EnhancedInputSubsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
    if (!IsValid(EnhancedInputSubsystem))
    {
        return;
    }

    if (bClearExistingInputMappingsBeforeAdding)
    {
        EnhancedInputSubsystem->ClearAllMappings();
    }

    const auto AddContextIfValid = [EnhancedInputSubsystem](const UInputMappingContext* MappingContext, int32 Priority) -> bool
    {
        if (IsValid(MappingContext))
        {
            EnhancedInputSubsystem->AddMappingContext(MappingContext, Priority);
            return true;
        }
        return false;
    };

    bAnyInputMappingContextApplied |= AddContextIfValid(InputMappingContext.Get(), InputMappingPriority);

    for (const FPlayerInputMappingContextConfig& ContextConfig : AdditionalInputMappingContexts)
    {
        bAnyInputMappingContextApplied |= AddContextIfValid(ContextConfig.MappingContext.Get(), ContextConfig.Priority);
    }
}

void APlayerCharacterController::RefreshConfiguredEnhancedInput()
{
    ApplyConfiguredInputMappingContexts();
    PrintInputSetupStatus();
}

FString APlayerCharacterController::GetInputFixVersion() const
{
    return TEXT("GameplayInput");
}

FString APlayerCharacterController::GetInputSetupStatus() const
{
    return FString::Printf(
        TEXT("%s | Controller=%s | Class=%s | PrimaryIMC=%s | IMCCount=%d | IAAssigned=%d | EnhancedInputComponent=%s | MappingApplied=%s | GameplayMouse=%s | ManagerClass=%s"),
        TEXT("GameplayInput"),
        *GetNameSafe(this),
        *GetNameSafe(GetClass()),
        *GetNameSafe(InputMappingContext.Get()),
        CountConfiguredInputMappingContexts(),
        CountAssignedEnhancedInputActions(),
        bEnhancedInputComponentWasAvailable ? TEXT("OK") : TEXT("NO"),
        bAnyInputMappingContextApplied ? TEXT("YES") : TEXT("NO"),
        (bEnableFallbackKeyBindings && bBindMouseButtons) ? TEXT("LMB/RMB") : TEXT("OFF"),
        *GetNameSafe(GameManagerActorClass ? GameManagerActorClass.Get() : AGameManagerActor::StaticClass()));
}

void APlayerCharacterController::PrintInputSetupStatus() const
{
    const FString Status = GetInputSetupStatus();
    UE_LOG(LogTemp, Display, TEXT("[GameplayInput] %s"), *Status);

    if (bShowInputSetupOnScreen && GEngine && IsLocalController())
    {
        GEngine->AddOnScreenDebugMessage(-1, 6.0f, FColor::Green, Status);
    }
}

void APlayerCharacterController::BindConfiguredInputActions()
{
    bEnhancedInputComponentWasAvailable = false;

    UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(InputComponent);
    if (!IsValid(EnhancedInputComponent))
    {
        UE_LOG(LogTemp, Warning, TEXT("PlayerCharacterController InputComponent is not an EnhancedInputComponent. Fallback keys remain active; Blueprint Input Action events can still call the Input_* functions manually."));
        return;
    }

    bEnhancedInputComponentWasAvailable = true;

    if (MoveAction)
    {
        EnhancedInputComponent->BindAction(MoveAction.Get(), ETriggerEvent::Triggered, this, &APlayerCharacterController::HandleMoveTriggered);
        EnhancedInputComponent->BindAction(MoveAction.Get(), ETriggerEvent::Completed, this, &APlayerCharacterController::HandleMoveCompleted);
        EnhancedInputComponent->BindAction(MoveAction.Get(), ETriggerEvent::Canceled, this, &APlayerCharacterController::HandleMoveCompleted);
    }
    if (LookAction)
    {
        EnhancedInputComponent->BindAction(LookAction.Get(), ETriggerEvent::Triggered, this, &APlayerCharacterController::HandleLookTriggered);
    }
    if (JumpAction)
    {
        EnhancedInputComponent->BindAction(JumpAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_JumpStarted);
        EnhancedInputComponent->BindAction(JumpAction.Get(), ETriggerEvent::Completed, this, &APlayerCharacterController::Input_JumpCompleted);
        EnhancedInputComponent->BindAction(JumpAction.Get(), ETriggerEvent::Canceled, this, &APlayerCharacterController::Input_JumpCompleted);
    }
    if (SprintAction)
    {
        EnhancedInputComponent->BindAction(SprintAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_SprintStarted);
        EnhancedInputComponent->BindAction(SprintAction.Get(), ETriggerEvent::Completed, this, &APlayerCharacterController::Input_SprintCompleted);
        EnhancedInputComponent->BindAction(SprintAction.Get(), ETriggerEvent::Canceled, this, &APlayerCharacterController::Input_SprintCompleted);
    }
    if (CrouchAction)
    {
        EnhancedInputComponent->BindAction(CrouchAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_CrouchStarted);
        EnhancedInputComponent->BindAction(CrouchAction.Get(), ETriggerEvent::Completed, this, &APlayerCharacterController::Input_CrouchCompleted);
        EnhancedInputComponent->BindAction(CrouchAction.Get(), ETriggerEvent::Canceled, this, &APlayerCharacterController::Input_CrouchCompleted);
    }
    if (FlyAction)
    {
        EnhancedInputComponent->BindAction(FlyAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_FlyPressed);
    }
    if (RagdollAction)
    {
        EnhancedInputComponent->BindAction(RagdollAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_RagdollPressed);
    }

    if (InteractAction)
    {
        EnhancedInputComponent->BindAction(InteractAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_InteractPressed);
    }
    if (ToggleFirstPersonAction)
    {
        EnhancedInputComponent->BindAction(ToggleFirstPersonAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_ToggleFirstPersonPressed);
    }
    if (ChangeCharacterAction)
    {
        EnhancedInputComponent->BindAction(ChangeCharacterAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_ChangeCharacterPressed);
    }
    if (ToolbarScrollAction)
    {
        EnhancedInputComponent->BindAction(ToolbarScrollAction.Get(), ETriggerEvent::Triggered, this, &APlayerCharacterController::HandleToolbarScrollTriggered);
    }
    if (ToggleItemListAction)
    {
        EnhancedInputComponent->BindAction(ToggleItemListAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_ToggleItemListPressed);
    }
    if (SnapAction)
    {
        EnhancedInputComponent->BindAction(SnapAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_SnapPressed);
    }

    if (VehicleMoveAction)
    {
        EnhancedInputComponent->BindAction(VehicleMoveAction.Get(), ETriggerEvent::Triggered, this, &APlayerCharacterController::HandleVehicleMoveTriggered);
        EnhancedInputComponent->BindAction(VehicleMoveAction.Get(), ETriggerEvent::Completed, this, &APlayerCharacterController::HandleVehicleMoveCompleted);
        EnhancedInputComponent->BindAction(VehicleMoveAction.Get(), ETriggerEvent::Canceled, this, &APlayerCharacterController::HandleVehicleMoveCompleted);
    }
    if (VehicleThrottleAction)
    {
        EnhancedInputComponent->BindAction(VehicleThrottleAction.Get(), ETriggerEvent::Triggered, this, &APlayerCharacterController::HandleVehicleThrottleTriggered);
        EnhancedInputComponent->BindAction(VehicleThrottleAction.Get(), ETriggerEvent::Completed, this, &APlayerCharacterController::HandleVehicleThrottleCompleted);
        EnhancedInputComponent->BindAction(VehicleThrottleAction.Get(), ETriggerEvent::Canceled, this, &APlayerCharacterController::HandleVehicleThrottleCompleted);
    }
    if (VehicleSteeringAction)
    {
        EnhancedInputComponent->BindAction(VehicleSteeringAction.Get(), ETriggerEvent::Triggered, this, &APlayerCharacterController::HandleVehicleSteeringTriggered);
        EnhancedInputComponent->BindAction(VehicleSteeringAction.Get(), ETriggerEvent::Completed, this, &APlayerCharacterController::HandleVehicleSteeringCompleted);
        EnhancedInputComponent->BindAction(VehicleSteeringAction.Get(), ETriggerEvent::Canceled, this, &APlayerCharacterController::HandleVehicleSteeringCompleted);
    }
    if (VehicleStopAction)
    {
        EnhancedInputComponent->BindAction(VehicleStopAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_VehicleStop);
        EnhancedInputComponent->BindAction(VehicleStopAction.Get(), ETriggerEvent::Completed, this, &APlayerCharacterController::Input_VehicleStop);
        EnhancedInputComponent->BindAction(VehicleStopAction.Get(), ETriggerEvent::Canceled, this, &APlayerCharacterController::Input_VehicleStop);
    }
    if (PauseAction)
    {
        EnhancedInputComponent->BindAction(PauseAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_PausePressed);
    }
    if (DebugAction)
    {
        EnhancedInputComponent->BindAction(DebugAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_DebugPressed);
    }
}

int32 APlayerCharacterController::CountConfiguredInputMappingContexts() const
{
    int32 Count = IsValid(InputMappingContext.Get()) ? 1 : 0;
    for (const FPlayerInputMappingContextConfig& ContextConfig : AdditionalInputMappingContexts)
    {
        if (IsValid(ContextConfig.MappingContext.Get()))
        {
            ++Count;
        }
    }
    return Count;
}

int32 APlayerCharacterController::CountAssignedEnhancedInputActions() const
{
    int32 Count = 0;
    const auto CountIfValid = [&Count](const UInputAction* Action)
    {
        if (IsValid(Action))
        {
            ++Count;
        }
    };

    CountIfValid(MoveAction.Get());
    CountIfValid(LookAction.Get());
    CountIfValid(JumpAction.Get());
    CountIfValid(SprintAction.Get());
    CountIfValid(CrouchAction.Get());
    CountIfValid(FlyAction.Get());
    CountIfValid(RagdollAction.Get());
    CountIfValid(ToggleFirstPersonAction.Get());
    CountIfValid(ChangeCharacterAction.Get());
    CountIfValid(ToolbarScrollAction.Get());
    CountIfValid(ToggleItemListAction.Get());
    CountIfValid(SnapAction.Get());
    CountIfValid(InteractAction.Get());
    CountIfValid(VehicleMoveAction.Get());
    CountIfValid(VehicleThrottleAction.Get());
    CountIfValid(VehicleSteeringAction.Get());
    CountIfValid(VehicleStopAction.Get());
    CountIfValid(PauseAction.Get());
    CountIfValid(DebugAction.Get());

    return Count;
}

bool APlayerCharacterController::ShouldBindFallbackKeyForAction(const UInputAction* ConfiguredAction) const
{
    if (!bBindFallbackKeysOnlyForUnassignedInputActions)
    {
        return true;
    }

    // If Enhanced Input is not fully ready, keep legacy keys alive instead of silently losing input.
    if (!bEnhancedInputComponentWasAvailable || !bAnyInputMappingContextApplied)
    {
        return true;
    }

    return !IsValid(ConfiguredAction);
}

void APlayerCharacterController::BindFallbackKeyInputs()
{
    if (!bEnableFallbackKeyBindings || !InputComponent)
    {
        return;
    }

    // These fallback keys keep the runtime tools usable even when Enhanced Input
    // assets are missing. When an InputAction is assigned, the matching fallback
    // key is skipped by default to avoid the same key firing twice.
    if (ShouldBindFallbackKeyForAction(MoveAction.Get()))
    {
        InputComponent->BindKey(EKeys::W, IE_Pressed, this, &APlayerCharacterController::FallbackMoveForwardPressed);
        InputComponent->BindKey(EKeys::W, IE_Released, this, &APlayerCharacterController::FallbackMoveForwardReleased);
        InputComponent->BindKey(EKeys::S, IE_Pressed, this, &APlayerCharacterController::FallbackMoveBackwardPressed);
        InputComponent->BindKey(EKeys::S, IE_Released, this, &APlayerCharacterController::FallbackMoveBackwardReleased);
        InputComponent->BindKey(EKeys::D, IE_Pressed, this, &APlayerCharacterController::FallbackMoveRightPressed);
        InputComponent->BindKey(EKeys::D, IE_Released, this, &APlayerCharacterController::FallbackMoveRightReleased);
        InputComponent->BindKey(EKeys::A, IE_Pressed, this, &APlayerCharacterController::FallbackMoveLeftPressed);
        InputComponent->BindKey(EKeys::A, IE_Released, this, &APlayerCharacterController::FallbackMoveLeftReleased);
    }

    if (ShouldBindFallbackKeyForAction(LookAction.Get()))
    {
        InputComponent->BindAxisKey(EKeys::MouseX, this, &APlayerCharacterController::FallbackLookYaw);
        InputComponent->BindAxisKey(EKeys::MouseY, this, &APlayerCharacterController::FallbackLookPitch);
    }

    if (ShouldBindFallbackKeyForAction(JumpAction.Get()))
    {
        InputComponent->BindKey(EKeys::SpaceBar, IE_Pressed, this, &APlayerCharacterController::Input_JumpStarted);
        InputComponent->BindKey(EKeys::SpaceBar, IE_Released, this, &APlayerCharacterController::Input_JumpCompleted);
    }
    if (ShouldBindFallbackKeyForAction(SprintAction.Get()))
    {
        InputComponent->BindKey(EKeys::LeftShift, IE_Pressed, this, &APlayerCharacterController::Input_SprintStarted);
        InputComponent->BindKey(EKeys::LeftShift, IE_Released, this, &APlayerCharacterController::Input_SprintCompleted);
    }
    if (ShouldBindFallbackKeyForAction(CrouchAction.Get()))
    {
        InputComponent->BindKey(EKeys::LeftControl, IE_Pressed, this, &APlayerCharacterController::Input_CrouchStarted);
        InputComponent->BindKey(EKeys::LeftControl, IE_Released, this, &APlayerCharacterController::Input_CrouchCompleted);
    }
    if (ShouldBindFallbackKeyForAction(PauseAction.Get()))
    {
        InputComponent->BindKey(EKeys::Escape, IE_Pressed, this, &APlayerCharacterController::Input_PausePressed);
    }
    // Keep F3 as a hard fallback even when a debug InputAction is assigned. Some mapping contexts may not map the
    // action yet, and Input_DebugPressed is debounced so a duplicate Enhanced Input event is harmless.
    InputComponent->BindKey(EKeys::F3, IE_Pressed, this, &APlayerCharacterController::Input_DebugPressed);

    // Gameplay tool commands are UI-driven. Only mouse placement and vehicle/camera hotkeys remain here.
    if (bBindMouseButtons)
    {
        InputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &APlayerCharacterController::Input_PrimaryPressed);
        InputComponent->BindKey(EKeys::LeftMouseButton, IE_Released, this, &APlayerCharacterController::Input_PrimaryReleased);
        InputComponent->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &APlayerCharacterController::Input_SecondaryPressed);
    }

    if (ShouldBindFallbackKeyForAction(InteractAction.Get()))
    {
        InputComponent->BindKey(EKeys::F, IE_Pressed, this, &APlayerCharacterController::Input_InteractPressed);
    }
    if (ShouldBindFallbackKeyForAction(ToggleFirstPersonAction.Get()))
    {
        InputComponent->BindKey(EKeys::V, IE_Pressed, this, &APlayerCharacterController::Input_ToggleFirstPersonPressed);
    }
    if (ShouldBindFallbackKeyForAction(ChangeCharacterAction.Get()))
    {
        InputComponent->BindKey(EKeys::U, IE_Pressed, this, &APlayerCharacterController::Input_ChangeCharacterPressed);
    }
    if (ShouldBindFallbackKeyForAction(ToolbarScrollAction.Get()))
    {
        InputComponent->BindAxisKey(EKeys::MouseWheelAxis, this, &APlayerCharacterController::Input_ToolbarScroll);
    }
    if (ShouldBindFallbackKeyForAction(ToggleItemListAction.Get()))
    {
        InputComponent->BindKey(EKeys::E, IE_Pressed, this, &APlayerCharacterController::Input_ToggleItemListPressed);
    }
    if (ShouldBindFallbackKeyForAction(SnapAction.Get()))
    {
        InputComponent->BindKey(EKeys::G, IE_Pressed, this, &APlayerCharacterController::Input_SnapPressed);
    }
}

void APlayerCharacterController::FallbackMoveForwardPressed()
{
    bFallbackMoveForward = true;
    UpdateFallbackMoveInput();
}

void APlayerCharacterController::FallbackMoveForwardReleased()
{
    bFallbackMoveForward = false;
    UpdateFallbackMoveInput();
}

void APlayerCharacterController::FallbackMoveBackwardPressed()
{
    bFallbackMoveBackward = true;
    UpdateFallbackMoveInput();
}

void APlayerCharacterController::FallbackMoveBackwardReleased()
{
    bFallbackMoveBackward = false;
    UpdateFallbackMoveInput();
}

void APlayerCharacterController::FallbackMoveRightPressed()
{
    bFallbackMoveRight = true;
    UpdateFallbackMoveInput();
}

void APlayerCharacterController::FallbackMoveRightReleased()
{
    bFallbackMoveRight = false;
    UpdateFallbackMoveInput();
}

void APlayerCharacterController::FallbackMoveLeftPressed()
{
    bFallbackMoveLeft = true;
    UpdateFallbackMoveInput();
}

void APlayerCharacterController::FallbackMoveLeftReleased()
{
    bFallbackMoveLeft = false;
    UpdateFallbackMoveInput();
}

void APlayerCharacterController::UpdateFallbackMoveInput()
{
    const float X = (bFallbackMoveRight ? 1.0f : 0.0f) - (bFallbackMoveLeft ? 1.0f : 0.0f);
    const float Y = (bFallbackMoveForward ? 1.0f : 0.0f) - (bFallbackMoveBackward ? 1.0f : 0.0f);
    Input_Move(FVector2D(X, Y));
}

void APlayerCharacterController::StopFallbackMovement()
{
    bFallbackMoveForward = false;
    bFallbackMoveBackward = false;
    bFallbackMoveRight = false;
    bFallbackMoveLeft = false;

    const bool bWasUIInputMode = bUIInputMode;
    bUIInputMode = false;
    Input_Move(FVector2D::ZeroVector);
    bUIInputMode = bWasUIInputMode;
}

void APlayerCharacterController::FallbackLookYaw(float Value)
{
    if (!bUIInputMode && !FMath::IsNearlyZero(Value))
    {
        Input_Look(FVector2D(Value, 0.0f));
    }
}

void APlayerCharacterController::FallbackLookPitch(float Value)
{
    if (!bUIInputMode && !FMath::IsNearlyZero(Value))
    {
        Input_Look(FVector2D(0.0f, Value));
    }
}

bool APlayerCharacterController::ConsumeInputDebounce(double& LastInputTime)
{
    const UWorld* World = GetWorld();
    const double Now = World ? World->GetTimeSeconds() : FPlatformTime::Seconds();
    if (LastInputTime >= 0.0 && FMath::Abs(Now - LastInputTime) <= 0.05)
    {
        return false;
    }

    LastInputTime = Now;
    return true;
}

void APlayerCharacterController::HandleMoveTriggered(const FInputActionValue& Value)
{
    Input_Move(Value.Get<FVector2D>());
}

void APlayerCharacterController::HandleMoveCompleted(const FInputActionValue& Value)
{
    Input_Move(FVector2D::ZeroVector);
}

void APlayerCharacterController::HandleLookTriggered(const FInputActionValue& Value)
{
    Input_Look(Value.Get<FVector2D>());
}

void APlayerCharacterController::HandleToolbarScrollTriggered(const FInputActionValue& Value)
{
    Input_ToolbarScroll(Value.Get<float>());
}

void APlayerCharacterController::HandleVehicleMoveTriggered(const FInputActionValue& Value)
{
    Input_VehicleMove(Value.Get<FVector2D>());
}

void APlayerCharacterController::HandleVehicleMoveCompleted(const FInputActionValue& Value)
{
    Input_VehicleMove(FVector2D::ZeroVector);
}

void APlayerCharacterController::HandleVehicleThrottleTriggered(const FInputActionValue& Value)
{
    Input_VehicleThrottle(Value.Get<float>());
}

void APlayerCharacterController::HandleVehicleThrottleCompleted(const FInputActionValue& Value)
{
    Input_VehicleThrottle(0.0f);
}

void APlayerCharacterController::HandleVehicleSteeringTriggered(const FInputActionValue& Value)
{
    Input_VehicleSteering(Value.Get<float>());
}

void APlayerCharacterController::HandleVehicleSteeringCompleted(const FInputActionValue& Value)
{
    Input_VehicleSteering(0.0f);
}

void APlayerCharacterController::Input_Move(const FVector2D& MoveValue)
{
    if (bUIInputMode)
    {
        return;
    }

    if (AVehiclePawn* Vehicle = Cast<AVehiclePawn>(GetPawn()))
    {
        Vehicle->SetDriveInput(MoveValue.Y, MoveValue.X);
        return;
    }

    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->MovementInput(MoveValue.X, MoveValue.Y);
    }
}

void APlayerCharacterController::Input_Look(const FVector2D& LookValue)
{
    if (bUIInputMode)
    {
        return;
    }

    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->CameraInput(LookValue.X, LookValue.Y, LookSensitivity);
    }
    else
    {
        AddYawInput(LookValue.X * LookSensitivity);
        AddPitchInput(LookValue.Y * LookSensitivity);
    }
}

void APlayerCharacterController::Input_JumpStarted()
{
    if (bUIInputMode)
    {
        return;
    }

    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->Jumping(true);
    }
}

void APlayerCharacterController::Input_JumpCompleted()
{
    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->Jumping(false);
    }
}

void APlayerCharacterController::Input_SprintStarted()
{
    bSprintInputHeld = true;
    if (bUIInputMode)
    {
        return;
    }

    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->Sprinting(true);
    }
}

void APlayerCharacterController::Input_SprintCompleted()
{
    bSprintInputHeld = false;
    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->Sprinting(false);
    }
}

void APlayerCharacterController::Input_CrouchStarted()
{
    bCrouchInputHeld = true;
    if (bUIInputMode)
    {
        return;
    }

    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->Crouching(true);
    }
}

void APlayerCharacterController::Input_CrouchCompleted()
{
    bCrouchInputHeld = false;
    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->Crouching(false);
    }
}

void APlayerCharacterController::Input_FlyPressed()
{
    if (bUIInputMode)
    {
        return;
    }

    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->Flying();
    }
}

void APlayerCharacterController::Input_RagdollPressed()
{
    if (bUIInputMode)
    {
        return;
    }

    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->ToggleRagdoll();
    }
}

void APlayerCharacterController::Input_PrimaryPressed()
{
    if (bUIInputMode || !ConsumeInputDebounce(LastPrimaryInputTime))
    {
        return;
    }

    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->InputPrimaryPressed();
    }
}

void APlayerCharacterController::Input_PrimaryReleased()
{
    if (bUIInputMode)
    {
        return;
    }

    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->InputPrimaryReleased();
    }
}

void APlayerCharacterController::Input_SecondaryPressed()
{
    if (bUIInputMode || !ConsumeInputDebounce(LastSecondaryInputTime))
    {
        return;
    }

    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->InputSecondaryAction();
    }
}

void APlayerCharacterController::Input_InteractPressed()
{
    if (!ConsumeInputDebounce(LastInteractInputTime))
    {
        return;
    }

    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->InputInteractAction();
    }
}

void APlayerCharacterController::Input_ToggleFirstPersonPressed()
{
    if (!ConsumeInputDebounce(LastToggleFirstPersonInputTime))
    {
        return;
    }

    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->InputToggleFirstPersonAction();
    }
}

void APlayerCharacterController::Input_ChangeCharacterPressed()
{
    if (bUIInputMode || !ConsumeInputDebounce(LastChangeCharacterInputTime))
    {
        return;
    }

    if (!IsValid(SubSystem))
    {
        SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    }

    if (IsValid(SubSystem) && SubSystem->IsWorldLoading())
    {
        return;
    }

    if (UglTFStreamSubSystem* StreamSubSystem = UglTFStreamSubSystem::Get(this))
    {
        StreamSubSystem->CycleNextPlayerCharacter();
    }
}

void APlayerCharacterController::Input_ToolbarScroll(float ScrollValue)
{
    if (bUIInputMode || FMath::IsNearlyZero(ScrollValue))
    {
        return;
    }

    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->InputToolbarScrollAction(ScrollValue);
    }
}

void APlayerCharacterController::Input_ToggleItemListPressed()
{
    if (!ConsumeInputDebounce(LastToggleItemListInputTime))
    {
        return;
    }

    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->InputToggleItemListAction();
        if (Manager->IsItemListWindowOpen())
        {
            ApplyUIInputMode(nullptr);
        }
        else
        {
            ApplyGameInputMode();
        }
    }
}

void APlayerCharacterController::Input_SnapPressed()
{
    if (!ConsumeInputDebounce(LastSnapInputTime))
    {
        return;
    }

    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->InputToggleSnapModeAction();
    }
}

void APlayerCharacterController::Input_VehicleMove(const FVector2D& MoveValue)
{
    if (bUIInputMode && !MoveValue.IsNearlyZero())
    {
        return;
    }

    if (AVehiclePawn* Vehicle = Cast<AVehiclePawn>(GetPawn()))
    {
        Vehicle->SetDriveInput(MoveValue.Y, MoveValue.X);
        return;
    }

    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->InputVehicleMoveAction(MoveValue);
    }
}

void APlayerCharacterController::Input_VehicleThrottle(float Throttle)
{
    if (bUIInputMode && !FMath::IsNearlyZero(Throttle))
    {
        return;
    }

    if (AVehiclePawn* Vehicle = Cast<AVehiclePawn>(GetPawn()))
    {
        Vehicle->SetThrottleInput(Throttle);
        return;
    }

    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->InputVehicleThrottleAction(Throttle);
    }
}

void APlayerCharacterController::Input_VehicleSteering(float Steering)
{
    if (bUIInputMode && !FMath::IsNearlyZero(Steering))
    {
        return;
    }

    if (AVehiclePawn* Vehicle = Cast<AVehiclePawn>(GetPawn()))
    {
        Vehicle->SetSteeringInput(Steering);
        return;
    }

    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->InputVehicleSteeringAction(Steering);
    }
}

void APlayerCharacterController::Input_VehicleStop()
{
    if (AVehiclePawn* Vehicle = Cast<AVehiclePawn>(GetPawn()))
    {
        Vehicle->ClearDriveInput();
        return;
    }

    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->InputVehicleMoveAction(FVector2D::ZeroVector);
    }
}

void APlayerCharacterController::ClearLatchedMovementInput()
{
    StopFallbackMovement();

    if (AVehiclePawn* Vehicle = Cast<AVehiclePawn>(GetPawn()))
    {
        Vehicle->ClearDriveInput();
    }

    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->ClearTransientInputState();
    }

    ReapplyHeldGameplayInput();
}

// Debug toggle translated from the Blueprint flow into C++.
void APlayerCharacterController::Input_DebugPressed()
{
    if (!ConsumeInputDebounce(LastDebugInputTime))
    {
        return;
    }

    if (IsValid(SubSystem) && SubSystem->IsWorldLoading())
    {
        return;
    }

    if (bIsDebug)
    {
        // When bIsDebug is true, remove the widget from the screen.
        if (IsValid(DebugWidget))
        {
            DebugWidget->RemoveFromParent();
        }
        bIsDebug = false;
    }
    else
    {
        // When bIsDebug is false, create the widget and add it to the screen.
        if (DebugWidgetClass)
        {
            // Create the widget if it does not exist yet, matching Blueprint CreateWidget.
            // Cache the widget instead of recreating it every toggle.
            if (!IsValid(DebugWidget))
            {
                DebugWidget = CreateWidget<UUserWidget>(this, DebugWidgetClass);
            }

            // Add the widget to the viewport, matching Blueprint AddToViewport.
            if (IsValid(DebugWidget))
            {
                DebugWidget->AddToViewport(0); // ZOrder 0
                bIsDebug = true;
            }
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("DebugWidgetClass is not assigned in PlayerCharacterController!"));
        }
    }
}

void APlayerCharacterController::Input_PausePressed()
{
    if (bMenuWorldTravelPending)
    {
        return;
    }

    if (!IsValid(SubSystem))
    {
        SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    }

    if (!IsValid(SubSystem) || SubSystem->IsWorldLoading())
    {
        return;
    }

    if (IsValid(SettingsMenuWidget) && SettingsMenuWidget->IsInViewport())
    {
        ReturnToPauseMenuFromSettings();
        return;
    }

    if (SubSystem->GetGamePaused() || (IsValid(PauseMenuWidget) && PauseMenuWidget->IsInViewport()))
    {
        ClosePauseMenu(true);
    }
    else
    {
        OpenPauseMenu();
    }
}


UUserWidget* APlayerCharacterController::CreatePauseMenu()
{
    if (bMenuWorldTravelPending)
    {
        return nullptr;
    }

    if (!IsValid(SubSystem))
    {
        SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    }
    if (IsValid(SubSystem) && SubSystem->IsWorldLoading())
    {
        return nullptr;
    }

    if (!PauseMenuWidgetClass)
    {
        UE_LOG(LogTemp, Verbose, TEXT("PlayerCharacterController: PauseMenuWidgetClass is not assigned; skipping pause menu creation."));
        return nullptr;
    }

    if (IsValid(PauseMenuWidget))
    {
        if (!PauseMenuWidget->IsInViewport())
        {
            PauseMenuWidget->AddToViewport(PauseMenuZOrder);
        }
        return PauseMenuWidget.Get();
    }

    PauseMenuWidget = CreateWidget<UUserWidget>(this, PauseMenuWidgetClass.Get());
    if (IsValid(PauseMenuWidget))
    {
        PauseMenuWidget->AddToViewport(PauseMenuZOrder);
    }
    return PauseMenuWidget.Get();
}

UUserWidget* APlayerCharacterController::CreateSettingsMenu()
{
    if (bMenuWorldTravelPending)
    {
        return nullptr;
    }

    if (!IsValid(SubSystem))
    {
        SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    }
    if (IsValid(SubSystem) && SubSystem->IsWorldLoading())
    {
        return nullptr;
    }

    if (!SettingsMenuWidgetClass)
    {
        UE_LOG(LogTemp, Verbose, TEXT("PlayerCharacterController: SettingsMenuWidgetClass is not assigned; skipping settings menu creation."));
        return nullptr;
    }

    if (!IsValid(SettingsMenuWidget))
    {
        SettingsMenuWidget = CreateWidget<UUserWidget>(this, SettingsMenuWidgetClass.Get());
    }

    if (IsValid(SettingsMenuWidget))
    {
        if (USettingsMenuWidget* SettingsWidget = Cast<USettingsMenuWidget>(SettingsMenuWidget.Get()))
        {
            // Re-opened settings menus must show the latest saved/runtime values, not stale pending edits.
            SettingsWidget->InitializeSettingsFromSavedData();
        }

        SettingsMenuWidget->SetVisibility(ESlateVisibility::Visible);
        if (!SettingsMenuWidget->IsInViewport())
        {
            SettingsMenuWidget->AddToViewport(SettingsMenuZOrder);
        }

        return SettingsMenuWidget.Get();
    }

    UE_LOG(LogTemp, Warning, TEXT("PlayerCharacterController: failed to create settings menu widget."));
    return nullptr;
}

void APlayerCharacterController::OpenPauseMenu()
{
    if (bMenuWorldTravelPending)
    {
        return;
    }

    if (!IsValid(SubSystem))
    {
        SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    }
    if (!IsValid(SubSystem) || SubSystem->IsWorldLoading())
    {
        return;
    }

    if (IsValid(SettingsMenuWidget))
    {
        SettingsMenuWidget->RemoveFromParent();
    }

    UUserWidget* Menu = CreatePauseMenu();
    SubSystem->SetGamePaused(true);
    ApplyUIInputMode(Menu);
    bPrevGamePaused = true;
}

void APlayerCharacterController::ClosePauseMenu(bool bResumeGame)
{
    if (bMenuWorldTravelPending)
    {
        return;
    }

    if (!IsValid(SubSystem))
    {
        SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    }

    if (IsValid(SettingsMenuWidget))
    {
        SettingsMenuWidget->RemoveFromParent();
    }
    if (IsValid(PauseMenuWidget))
    {
        PauseMenuWidget->RemoveFromParent();
    }

    if (bResumeGame && IsValid(SubSystem))
    {
        SubSystem->SetGamePaused(false);
    }

    ApplyGameInputMode();
    bPrevGamePaused = IsValid(SubSystem) ? SubSystem->GetGamePaused() : false;
}

void APlayerCharacterController::ShowSettingsMenuFromPause()
{
    if (bMenuWorldTravelPending)
    {
        return;
    }

    if (!IsValid(SubSystem))
    {
        SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    }
    if (!IsValid(SubSystem) || SubSystem->IsWorldLoading())
    {
        return;
    }

    if (IsValid(PauseMenuWidget))
    {
        PauseMenuWidget->RemoveFromParent();
    }

    UUserWidget* Settings = CreateSettingsMenu();
    if (!IsValid(Settings))
    {
        // Keep the pause menu visible if the settings widget could not be built.
        CreatePauseMenu();
        ApplyUIInputMode(PauseMenuWidget.Get());
        return;
    }

    SubSystem->SetGamePaused(true);
    ApplyUIInputMode(Settings);
    bPrevGamePaused = true;
}

void APlayerCharacterController::ReturnToPauseMenuFromSettings()
{
    if (bMenuWorldTravelPending)
    {
        return;
    }

    if (IsValid(SettingsMenuWidget))
    {
        SettingsMenuWidget->RemoveFromParent();
    }

    if (!IsValid(SubSystem))
    {
        SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    }
    if (IsValid(SubSystem) && !SubSystem->IsWorldLoading())
    {
        SubSystem->SetGamePaused(true);
    }

    UUserWidget* Menu = CreatePauseMenu();
    ApplyUIInputMode(Menu);
    bPrevGamePaused = true;
}

void APlayerCharacterController::ExitToMainWorldFromPauseMenu()
{
    // Backward-compatible Blueprint entry point. Gameplay exits now land on world selection.
    ExitToWorldSelectionFromPauseMenu();
}

void APlayerCharacterController::ExitToWorldSelectionFromPauseMenu()
{
    TryExitToWorldSelectionFromPauseMenu();
}

bool APlayerCharacterController::TryExitToWorldSelectionFromPauseMenu()
{
    // Multiple listeners may be serialized on the same WBP button. Once one listener has started
    // a valid request, every later listener should treat the request as accepted rather than
    // re-enabling the pause UI or attempting a second OpenLevel.
    if (bMenuWorldTravelPending)
    {
        UE_LOG(LogTemp, Verbose, TEXT("[MenuTravel] Pause Exit is already pending on this controller."));
        return true;
    }

    const TSoftObjectPtr<UWorld> DestinationWorld = ResolveWorldSelectionWorld();
    if (DestinationWorld.IsNull())
    {
        UE_LOG(LogTemp, Error,
            TEXT("[MenuTravel] Cannot leave gameplay: no WorldSelectionWorld is assigned on the PlayerController "
                 "and no menu world was registered by StartActor."));
        return false;
    }

    if (WorldSelectionWorld.IsNull())
    {
        UE_LOG(LogTemp, Display,
            TEXT("[MenuTravel] PlayerController WorldSelectionWorld is empty; using the menu world registered by StartActor."));
    }

    bMenuWorldTravelPending = true;

    if (IsValid(SettingsMenuWidget))
    {
        SettingsMenuWidget->SetIsEnabled(false);
        SettingsMenuWidget->RemoveFromParent();
    }
    if (IsValid(PauseMenuWidget))
    {
        PauseMenuWidget->SetIsEnabled(false);
        PauseMenuWidget->RemoveFromParent();
    }

    LockInputForMenuWorldTravel();
    UE_LOG(LogTemp, Display, TEXT("[MenuTravel] Pause Exit accepted; requesting the world-selection world."));

    if (!UGameManagerSubSystem::TryOpenWorldSelectionScreen(this, DestinationWorld))
    {
        UE_LOG(LogTemp, Error, TEXT("[MenuTravel] World-selection travel request was rejected before OpenLevel."));
        RestorePauseMenuAfterRejectedTravel();
        return false;
    }

    ArmMenuWorldTravelWatchdog();
    return true;
}

void APlayerCharacterController::ReturnToMainMenuFromWorldSelection()
{
    if (bMenuWorldTravelPending)
    {
        return;
    }

    if (MainMenuWorld.IsNull())
    {
        UE_LOG(LogTemp, Error,
            TEXT("PlayerCharacterController cannot open the main menu because MainMenuWorld is not assigned."));
        return;
    }

    bMenuWorldTravelPending = true;
    if (IsValid(SettingsMenuWidget))
    {
        SettingsMenuWidget->SetIsEnabled(false);
        SettingsMenuWidget->RemoveFromParent();
    }
    if (IsValid(PauseMenuWidget))
    {
        PauseMenuWidget->SetIsEnabled(false);
        PauseMenuWidget->RemoveFromParent();
    }

    LockInputForMenuWorldTravel();
    UGameManagerSubSystem::OpenMainMenuFromWorldSelection(this, MainMenuWorld);
}

UGameManagerSubSystem* APlayerCharacterController::GetGameManager()
{
    UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this);
    if (!IsValid(Manager))
    {
        return nullptr;
    }

    SubSystem = Manager;

    const UClass* DesiredActorClass = GameManagerActorClass ? GameManagerActorClass.Get() : AGameManagerActor::StaticClass();

    if (IsValid(CachedGameManagerActor) && CachedGameManagerActor->IsA(DesiredActorClass))
    {
        Manager->StartGameManager(CachedGameManagerActor.Get());
        return Manager;
    }
    CachedGameManagerActor = nullptr;

    UWorld* World = GetWorld();
    if (!World)
    {
        return Manager;
    }

    AGameManagerActor* FirstCompatibleActor = nullptr;
    for (TActorIterator<AGameManagerActor> It(World); It; ++It)
    {
        AGameManagerActor* ExistingActor = *It;
        if (!IsValid(ExistingActor))
        {
            continue;
        }

        if (ExistingActor->IsA(DesiredActorClass))
        {
            CachedGameManagerActor = ExistingActor;
            Manager->StartGameManager(ExistingActor);
            return Manager;
        }

        if (!FirstCompatibleActor && DesiredActorClass == AGameManagerActor::StaticClass())
        {
            FirstCompatibleActor = ExistingActor;
        }
    }

    if (FirstCompatibleActor)
    {
        CachedGameManagerActor = FirstCompatibleActor;
        Manager->StartGameManager(FirstCompatibleActor);
        return Manager;
    }

    if (Manager->IsWorldLoading())
    {
        return Manager;
    }

    FActorSpawnParameters Params;
    Params.Owner = this;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    UClass* ManagerSpawnClass = GameManagerActorClass ? GameManagerActorClass.Get() : AGameManagerActor::StaticClass();
    CachedGameManagerActor = World->SpawnActor<AGameManagerActor>(ManagerSpawnClass, FTransform::Identity, Params);
    if (IsValid(CachedGameManagerActor))
    {
        Manager->StartGameManager(CachedGameManagerActor.Get());
    }
    return Manager;
}
