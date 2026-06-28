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
#include "Runtime/RuntimeGameplayManager.h"
#include "Runtime/RuntimeCreatorHUDWidget.h"
#include "Runtime/RuntimePauseMenuWidget.h"
#include "Runtime/RuntimeSettingsMenuWidget.h"
#include "Runtime/RuntimeVehiclePawn.h"
#include "System/GameManagerSubSystem.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Engine/Engine.h"
#include "Kismet/GameplayStatics.h"

APlayerCharacterController::APlayerCharacterController()
{
    RuntimeGameplayManagerClass = nullptr;

    static ConstructorHelpers::FObjectFinder<UInputAction> DebugActionFinder(TEXT("/Game/Input/Actions/IA_Debug.IA_Debug"));
    if (DebugActionFinder.Succeeded())
    {
        DebugAction = DebugActionFinder.Object;
    }

    static ConstructorHelpers::FClassFinder<UUserWidget> DebugWidgetFinder(TEXT("/Game/Blueprints/MainWorld/WBP_Debug"));
    if (DebugWidgetFinder.Succeeded())
    {
        DebugWidgetClass = DebugWidgetFinder.Class;
    }

    bAutoCreateRuntimeCreatorHUD = false;
    PauseMenuWidgetClass = nullptr;
    SettingsMenuWidgetClass = nullptr;
}

void APlayerCharacterController::BeginPlay()
{
    Super::BeginPlay();
    if (!IsValid(SubSystem))
    {
        SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    }
    if (bApplyInputMappingContextsOnBeginPlay)
    {
        ApplyConfiguredInputMappingContexts();
    }

    if (bAutoSpawnRuntimeGameplayManager)
    {
        GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
        {
            if (!IsValid(SubSystem) || !SubSystem->IsWorldLoading())
            {
                GetRuntimeGameplayManager();
            }
        }));
    }

    if (bForceGameInputModeOnBeginPlay)
    {
        ApplyGameInputMode();
        GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
        {
            if (IsValid(SubSystem) && (SubSystem->GetGamePaused() || SubSystem->IsWorldLoading()))
            {
                return;
            }

            // Keep GameOnly input by default even while the Creator HUD is visible.
            ApplyGameInputMode();
        }));
    }

}


UUserWidget* APlayerCharacterController::CreateRuntimeCreatorHUD()
{
    if (IsValid(SubSystem) && SubSystem->IsWorldLoading())
    {
        return nullptr;
    }

    // No native/WBP fallback class is loaded here anymore. Assign a WBP class explicitly or create the widget in Blueprint.
    if (!RuntimeCreatorHUDWidgetClass)
    {
        UE_LOG(LogTemp, Verbose, TEXT("PlayerCharacterController: RuntimeCreatorHUDWidgetClass is not assigned; skipping Creator HUD creation."));
        return nullptr;
    }

    // Reuse the existing HUD instance when it is still alive.
    if (IsValid(RuntimeCreatorHUDWidget))
    {
        if (!RuntimeCreatorHUDWidget->IsInViewport())
        {
            RuntimeCreatorHUDWidget->AddToViewport(RuntimeCreatorHUDZOrder);
        }
        return RuntimeCreatorHUDWidget.Get();
    }

    // Only instantiate an explicitly assigned WBP class.
    RuntimeCreatorHUDWidget = CreateWidget<UUserWidget>(this, RuntimeCreatorHUDWidgetClass);

    // Return nullptr if widget creation fails.
    if (!IsValid(RuntimeCreatorHUDWidget))
    {
        return nullptr;
    }

    // Add the HUD to the viewport.
    RuntimeCreatorHUDWidget->AddToViewport(RuntimeCreatorHUDZOrder);

    // Keep GameOnly input so this HUD does not interrupt crosshair-centered gameplay.
    if (!bRuntimeUIInputMode)
    {
        ApplyGameInputMode();
    }

    // Return the created HUD instance.
    return RuntimeCreatorHUDWidget.Get();
}

void APlayerCharacterController::RemoveRuntimeCreatorHUD()
{
    // Remove the HUD from the viewport if it is valid.
    if (IsValid(RuntimeCreatorHUDWidget))
    {
        RuntimeCreatorHUDWidget->RemoveFromParent();
    }

    // Clear the reference so the next request can create a fresh instance.
    RuntimeCreatorHUDWidget = nullptr;
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
    if (bLogRuntimeInputSetup)
    {
        PrintRuntimeInputSetupStatus();
    }
}

void APlayerCharacterController::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    if (bEnableFallbackKeyBindings && !bRuntimeUIInputMode &&
        (bFallbackMoveForward || bFallbackMoveBackward || bFallbackMoveRight || bFallbackMoveLeft))
    {
        UpdateFallbackMoveInput();
    }
}

void APlayerCharacterController::ApplyGameInputMode()
{
    bRuntimeUIInputMode = false;
    UWidgetBlueprintLibrary::SetInputMode_GameOnly(this, false);
    bShowMouseCursor = !bHideMouseCursorDuringGameplay;
    bEnableClickEvents = false;
    bEnableMouseOverEvents = false;
    SetIgnoreLookInput(false);
    SetIgnoreMoveInput(false);
}

void APlayerCharacterController::ApplyUIInputMode(UUserWidget* WidgetToFocus)
{
    bRuntimeUIInputMode = true;
    UWidgetBlueprintLibrary::SetInputMode_GameAndUIEx(
        this,
        WidgetToFocus,
        EMouseLockMode::DoNotLock,
        true,
        false);
    bShowMouseCursor = true;
    bEnableClickEvents = true;
    bEnableMouseOverEvents = true;
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

    bAnyInputMappingContextApplied |= AddContextIfValid(RuntimeInputMappingContext.Get(), RuntimeInputMappingPriority);

    for (const FPlayerInputMappingContextConfig& ContextConfig : AdditionalInputMappingContexts)
    {
        bAnyInputMappingContextApplied |= AddContextIfValid(ContextConfig.MappingContext.Get(), ContextConfig.Priority);
    }
}

void APlayerCharacterController::RefreshConfiguredEnhancedInput()
{
    ApplyConfiguredInputMappingContexts();
    PrintRuntimeInputSetupStatus();
}

FString APlayerCharacterController::GetRuntimeInputFixVersion() const
{
    return TEXT("RuntimeInput");
}

FString APlayerCharacterController::GetRuntimeInputSetupStatus() const
{
    return FString::Printf(
        TEXT("%s | Controller=%s | Class=%s | PrimaryIMC=%s | IMCCount=%d | IAAssigned=%d | EnhancedInputComponent=%s | MappingApplied=%s | RuntimeMouse=%s | ManagerClass=%s"),
        TEXT("RuntimeInput"),
        *GetNameSafe(this),
        *GetNameSafe(GetClass()),
        *GetNameSafe(RuntimeInputMappingContext.Get()),
        CountConfiguredInputMappingContexts(),
        CountAssignedEnhancedInputActions(),
        bEnhancedInputComponentWasAvailable ? TEXT("OK") : TEXT("NO"),
        bAnyInputMappingContextApplied ? TEXT("YES") : TEXT("NO"),
        (bEnableFallbackKeyBindings && bBindRuntimeMouseButtons) ? TEXT("LMB/RMB") : TEXT("OFF"),
        *GetNameSafe(RuntimeGameplayManagerClass ? RuntimeGameplayManagerClass.Get() : ARuntimeGameplayManager::StaticClass()));
}

void APlayerCharacterController::PrintRuntimeInputSetupStatus() const
{
    const FString Status = GetRuntimeInputSetupStatus();
    UE_LOG(LogTemp, Display, TEXT("[RuntimeInput] %s"), *Status);

    if (bShowRuntimeInputSetupOnScreen && GEngine && IsLocalController())
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

    if (RuntimeInteractAction)
    {
        EnhancedInputComponent->BindAction(RuntimeInteractAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_RuntimeInteractPressed);
    }
    if (RuntimeToggleFirstPersonAction)
    {
        EnhancedInputComponent->BindAction(RuntimeToggleFirstPersonAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_RuntimeToggleFirstPersonPressed);
    }
    if (RuntimeToolbarScrollAction)
    {
        EnhancedInputComponent->BindAction(RuntimeToolbarScrollAction.Get(), ETriggerEvent::Triggered, this, &APlayerCharacterController::HandleToolbarScrollTriggered);
    }
    if (RuntimeToggleItemListAction)
    {
        EnhancedInputComponent->BindAction(RuntimeToggleItemListAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_RuntimeToggleItemListPressed);
    }
    if (RuntimeSnapAction)
    {
        EnhancedInputComponent->BindAction(RuntimeSnapAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_RuntimeSnapPressed);
    }

    if (RuntimeVehicleMoveAction)
    {
        EnhancedInputComponent->BindAction(RuntimeVehicleMoveAction.Get(), ETriggerEvent::Triggered, this, &APlayerCharacterController::HandleVehicleMoveTriggered);
        EnhancedInputComponent->BindAction(RuntimeVehicleMoveAction.Get(), ETriggerEvent::Completed, this, &APlayerCharacterController::HandleVehicleMoveCompleted);
        EnhancedInputComponent->BindAction(RuntimeVehicleMoveAction.Get(), ETriggerEvent::Canceled, this, &APlayerCharacterController::HandleVehicleMoveCompleted);
    }
    if (RuntimeVehicleThrottleAction)
    {
        EnhancedInputComponent->BindAction(RuntimeVehicleThrottleAction.Get(), ETriggerEvent::Triggered, this, &APlayerCharacterController::HandleVehicleThrottleTriggered);
        EnhancedInputComponent->BindAction(RuntimeVehicleThrottleAction.Get(), ETriggerEvent::Completed, this, &APlayerCharacterController::HandleVehicleThrottleCompleted);
        EnhancedInputComponent->BindAction(RuntimeVehicleThrottleAction.Get(), ETriggerEvent::Canceled, this, &APlayerCharacterController::HandleVehicleThrottleCompleted);
    }
    if (RuntimeVehicleSteeringAction)
    {
        EnhancedInputComponent->BindAction(RuntimeVehicleSteeringAction.Get(), ETriggerEvent::Triggered, this, &APlayerCharacterController::HandleVehicleSteeringTriggered);
        EnhancedInputComponent->BindAction(RuntimeVehicleSteeringAction.Get(), ETriggerEvent::Completed, this, &APlayerCharacterController::HandleVehicleSteeringCompleted);
        EnhancedInputComponent->BindAction(RuntimeVehicleSteeringAction.Get(), ETriggerEvent::Canceled, this, &APlayerCharacterController::HandleVehicleSteeringCompleted);
    }
    if (RuntimeVehicleStopAction)
    {
        EnhancedInputComponent->BindAction(RuntimeVehicleStopAction.Get(), ETriggerEvent::Started, this, &APlayerCharacterController::Input_RuntimeVehicleStop);
        EnhancedInputComponent->BindAction(RuntimeVehicleStopAction.Get(), ETriggerEvent::Completed, this, &APlayerCharacterController::Input_RuntimeVehicleStop);
        EnhancedInputComponent->BindAction(RuntimeVehicleStopAction.Get(), ETriggerEvent::Canceled, this, &APlayerCharacterController::Input_RuntimeVehicleStop);
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
    int32 Count = IsValid(RuntimeInputMappingContext.Get()) ? 1 : 0;
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
    CountIfValid(RuntimeToggleFirstPersonAction.Get());
    CountIfValid(RuntimeToolbarScrollAction.Get());
    CountIfValid(RuntimeToggleItemListAction.Get());
    CountIfValid(RuntimeSnapAction.Get());
    CountIfValid(RuntimeInteractAction.Get());
    CountIfValid(RuntimeVehicleMoveAction.Get());
    CountIfValid(RuntimeVehicleThrottleAction.Get());
    CountIfValid(RuntimeVehicleSteeringAction.Get());
    CountIfValid(RuntimeVehicleStopAction.Get());
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
    // Keep F3 as a hard fallback even when IA_Debug exists. Some project IMC assets may not map the
    // action yet, and Input_DebugPressed is debounced so a duplicate Enhanced Input event is harmless.
    InputComponent->BindKey(EKeys::F3, IE_Pressed, this, &APlayerCharacterController::Input_DebugPressed);

    // Runtime tool commands are UI-driven. Only mouse placement and vehicle/camera hotkeys remain here.
    if (bBindRuntimeMouseButtons)
    {
        InputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &APlayerCharacterController::Input_RuntimePrimaryPressed);
        InputComponent->BindKey(EKeys::LeftMouseButton, IE_Released, this, &APlayerCharacterController::Input_RuntimePrimaryReleased);
        InputComponent->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &APlayerCharacterController::Input_RuntimeSecondaryPressed);
    }

    if (ShouldBindFallbackKeyForAction(RuntimeInteractAction.Get()))
    {
        InputComponent->BindKey(EKeys::F, IE_Pressed, this, &APlayerCharacterController::Input_RuntimeInteractPressed);
    }
    if (ShouldBindFallbackKeyForAction(RuntimeToggleFirstPersonAction.Get()))
    {
        InputComponent->BindKey(EKeys::V, IE_Pressed, this, &APlayerCharacterController::Input_RuntimeToggleFirstPersonPressed);
    }
    if (ShouldBindFallbackKeyForAction(RuntimeToolbarScrollAction.Get()))
    {
        InputComponent->BindAxisKey(EKeys::MouseWheelAxis, this, &APlayerCharacterController::Input_RuntimeToolbarScroll);
    }
    if (ShouldBindFallbackKeyForAction(RuntimeToggleItemListAction.Get()))
    {
        InputComponent->BindKey(EKeys::E, IE_Pressed, this, &APlayerCharacterController::Input_RuntimeToggleItemListPressed);
    }
    if (ShouldBindFallbackKeyForAction(RuntimeSnapAction.Get()))
    {
        InputComponent->BindKey(EKeys::G, IE_Pressed, this, &APlayerCharacterController::Input_RuntimeSnapPressed);
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

    const bool bWasRuntimeUIInputMode = bRuntimeUIInputMode;
    bRuntimeUIInputMode = false;
    Input_Move(FVector2D::ZeroVector);
    bRuntimeUIInputMode = bWasRuntimeUIInputMode;
}

void APlayerCharacterController::FallbackLookYaw(float Value)
{
    if (!bRuntimeUIInputMode && !FMath::IsNearlyZero(Value))
    {
        Input_Look(FVector2D(Value, 0.0f));
    }
}

void APlayerCharacterController::FallbackLookPitch(float Value)
{
    if (!bRuntimeUIInputMode && !FMath::IsNearlyZero(Value))
    {
        Input_Look(FVector2D(0.0f, Value));
    }
}

bool APlayerCharacterController::ConsumeRuntimeInput(double& LastInputTime)
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
    Input_RuntimeToolbarScroll(Value.Get<float>());
}

void APlayerCharacterController::HandleVehicleMoveTriggered(const FInputActionValue& Value)
{
    Input_RuntimeVehicleMove(Value.Get<FVector2D>());
}

void APlayerCharacterController::HandleVehicleMoveCompleted(const FInputActionValue& Value)
{
    Input_RuntimeVehicleMove(FVector2D::ZeroVector);
}

void APlayerCharacterController::HandleVehicleThrottleTriggered(const FInputActionValue& Value)
{
    Input_RuntimeVehicleThrottle(Value.Get<float>());
}

void APlayerCharacterController::HandleVehicleThrottleCompleted(const FInputActionValue& Value)
{
    Input_RuntimeVehicleThrottle(0.0f);
}

void APlayerCharacterController::HandleVehicleSteeringTriggered(const FInputActionValue& Value)
{
    Input_RuntimeVehicleSteering(Value.Get<float>());
}

void APlayerCharacterController::HandleVehicleSteeringCompleted(const FInputActionValue& Value)
{
    Input_RuntimeVehicleSteering(0.0f);
}

void APlayerCharacterController::Input_Move(const FVector2D& MoveValue)
{
    if (bRuntimeUIInputMode)
    {
        return;
    }

    if (ARuntimeVehiclePawn* Vehicle = Cast<ARuntimeVehiclePawn>(GetPawn()))
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
    if (bRuntimeUIInputMode)
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
    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->Sprinting(true);
    }
}

void APlayerCharacterController::Input_SprintCompleted()
{
    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->Sprinting(false);
    }
}

void APlayerCharacterController::Input_CrouchStarted()
{
    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->Crouching(true);
    }
}

void APlayerCharacterController::Input_CrouchCompleted()
{
    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->Crouching(false);
    }
}

void APlayerCharacterController::Input_FlyPressed()
{
    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->Flying();
    }
}

void APlayerCharacterController::Input_RagdollPressed()
{
    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->ToggleRagdoll();
    }
}

void APlayerCharacterController::Input_RuntimePrimaryPressed()
{
    if (bRuntimeUIInputMode || !ConsumeRuntimeInput(LastRuntimePrimaryInputTime))
    {
        return;
    }

    if (ARuntimeGameplayManager* Manager = GetRuntimeGameplayManager())
    {
        Manager->InputPrimaryPressed();
    }
}

void APlayerCharacterController::Input_RuntimePrimaryReleased()
{
    if (bRuntimeUIInputMode)
    {
        return;
    }

    if (ARuntimeGameplayManager* Manager = GetRuntimeGameplayManager())
    {
        Manager->InputPrimaryReleased();
    }
}

void APlayerCharacterController::Input_RuntimeSecondaryPressed()
{
    if (bRuntimeUIInputMode || !ConsumeRuntimeInput(LastRuntimeSecondaryInputTime))
    {
        return;
    }

    if (ARuntimeGameplayManager* Manager = GetRuntimeGameplayManager())
    {
        Manager->InputSecondaryAction();
    }
}

void APlayerCharacterController::Input_RuntimeInteractPressed()
{
    if (!ConsumeRuntimeInput(LastRuntimeInteractInputTime))
    {
        return;
    }

    if (ARuntimeGameplayManager* Manager = GetRuntimeGameplayManager())
    {
        Manager->InputInteractAction();
    }
}

void APlayerCharacterController::Input_RuntimeToggleFirstPersonPressed()
{
    if (!ConsumeRuntimeInput(LastRuntimeToggleFirstPersonInputTime))
    {
        return;
    }

    if (ARuntimeGameplayManager* Manager = GetRuntimeGameplayManager())
    {
        Manager->InputToggleFirstPersonAction();
    }
}

void APlayerCharacterController::Input_RuntimeToolbarScroll(float ScrollValue)
{
    if (bRuntimeUIInputMode || FMath::IsNearlyZero(ScrollValue))
    {
        return;
    }

    if (ARuntimeGameplayManager* Manager = GetRuntimeGameplayManager())
    {
        Manager->InputToolbarScrollAction(ScrollValue);
    }
}

void APlayerCharacterController::Input_RuntimeToggleItemListPressed()
{
    if (!ConsumeRuntimeInput(LastRuntimeToggleItemListInputTime))
    {
        return;
    }

    if (ARuntimeGameplayManager* Manager = GetRuntimeGameplayManager())
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

void APlayerCharacterController::Input_RuntimeSnapPressed()
{
    if (!ConsumeRuntimeInput(LastRuntimeSnapInputTime))
    {
        return;
    }

    if (ARuntimeGameplayManager* Manager = GetRuntimeGameplayManager())
    {
        Manager->InputToggleSnapModeAction();
    }
}

void APlayerCharacterController::Input_RuntimeVehicleMove(const FVector2D& MoveValue)
{
    if (ARuntimeVehiclePawn* Vehicle = Cast<ARuntimeVehiclePawn>(GetPawn()))
    {
        Vehicle->SetDriveInput(MoveValue.Y, MoveValue.X);
        return;
    }

    if (ARuntimeGameplayManager* Manager = GetRuntimeGameplayManager())
    {
        Manager->InputVehicleMoveAction(MoveValue);
    }
}

void APlayerCharacterController::Input_RuntimeVehicleThrottle(float Throttle)
{
    if (ARuntimeVehiclePawn* Vehicle = Cast<ARuntimeVehiclePawn>(GetPawn()))
    {
        Vehicle->SetThrottleInput(Throttle);
        return;
    }

    if (ARuntimeGameplayManager* Manager = GetRuntimeGameplayManager())
    {
        Manager->InputVehicleThrottleAction(Throttle);
    }
}

void APlayerCharacterController::Input_RuntimeVehicleSteering(float Steering)
{
    if (ARuntimeVehiclePawn* Vehicle = Cast<ARuntimeVehiclePawn>(GetPawn()))
    {
        Vehicle->SetSteeringInput(Steering);
        return;
    }

    if (ARuntimeGameplayManager* Manager = GetRuntimeGameplayManager())
    {
        Manager->InputVehicleSteeringAction(Steering);
    }
}

void APlayerCharacterController::Input_RuntimeVehicleStop()
{
    if (ARuntimeVehiclePawn* Vehicle = Cast<ARuntimeVehiclePawn>(GetPawn()))
    {
        Vehicle->ClearDriveInput();
        return;
    }

    if (ARuntimeGameplayManager* Manager = GetRuntimeGameplayManager())
    {
        Manager->InputVehicleMoveAction(FVector2D::ZeroVector);
    }
}

void APlayerCharacterController::ClearLatchedMovementInput()
{
    StopFallbackMovement();

    if (ARuntimeVehiclePawn* Vehicle = Cast<ARuntimeVehiclePawn>(GetPawn()))
    {
        Vehicle->ClearDriveInput();
    }

    if (ACharacterController* CharacterCtrl = Cast<ACharacterController>(GetPawn()))
    {
        CharacterCtrl->ClearTransientInputState();
    }
}

// Debug toggle translated from the Blueprint flow into C++.
void APlayerCharacterController::Input_DebugPressed()
{
    if (!ConsumeRuntimeInput(LastRuntimeDebugInputTime))
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

void APlayerCharacterController::ExitToStartWorldFromPauseMenu()
{
    ARuntimeGameplayManager* Manager = GetRuntimeGameplayManager();
    if (IsValid(Manager))
    {
        Manager->SaveRuntimeScene();
    }

    if (IsValid(SettingsMenuWidget))
    {
        SettingsMenuWidget->RemoveFromParent();
    }
    if (IsValid(PauseMenuWidget))
    {
        PauseMenuWidget->RemoveFromParent();
    }

    if (!IsValid(SubSystem))
    {
        SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    }
    if (IsValid(SubSystem))
    {
        SubSystem->SetGamePaused(false);
    }

    ApplyGameInputMode();
    if (ExitLevelName != NAME_None)
    {
        UGameplayStatics::OpenLevel(this, ExitLevelName);
    }
}

ARuntimeGameplayManager *APlayerCharacterController::GetRuntimeGameplayManager()
{
    const UClass* DesiredManagerClass = RuntimeGameplayManagerClass ? RuntimeGameplayManagerClass.Get() : ARuntimeGameplayManager::StaticClass();

    if (IsValid(CachedRuntimeGameplayManager))
    {
        if (CachedRuntimeGameplayManager->IsA(DesiredManagerClass))
        {
            return CachedRuntimeGameplayManager;
        }
        CachedRuntimeGameplayManager = nullptr;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return nullptr;
    }

    ARuntimeGameplayManager* FirstCompatibleManager = nullptr;
    for (TActorIterator<ARuntimeGameplayManager> It(World); It; ++It)
    {
        ARuntimeGameplayManager* ExistingManager = *It;
        if (!IsValid(ExistingManager))
        {
            continue;
        }

        if (ExistingManager->IsA(DesiredManagerClass))
        {
            CachedRuntimeGameplayManager = ExistingManager;
            return CachedRuntimeGameplayManager;
        }

        if (!FirstCompatibleManager && DesiredManagerClass == ARuntimeGameplayManager::StaticClass())
        {
            FirstCompatibleManager = ExistingManager;
        }
    }

    if (FirstCompatibleManager)
    {
        CachedRuntimeGameplayManager = FirstCompatibleManager;
        return CachedRuntimeGameplayManager;
    }

    if (IsValid(SubSystem) && SubSystem->IsWorldLoading())
    {
        return nullptr;
    }

    FActorSpawnParameters Params;
    Params.Owner = this;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    UClass* ManagerSpawnClass = RuntimeGameplayManagerClass ? RuntimeGameplayManagerClass.Get() : ARuntimeGameplayManager::StaticClass();
    CachedRuntimeGameplayManager = World->SpawnActor<ARuntimeGameplayManager>(ManagerSpawnClass, FTransform::Identity, Params);
    return CachedRuntimeGameplayManager;
}
