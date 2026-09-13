// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file PauseMenuWidget.cpp
 * 역할: 일시정지 메뉴의 공통 UI 동작을 제공합니다.
 * 핵심 기능: 버튼 바인딩, 재개·설정·월드 선택 이동.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "UI/PauseMenuWidget.h"
#include "Character/PlayerCharacterController.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Kismet/GameplayStatics.h"

void UPauseMenuWidget::NativeConstruct()
{
    Super::NativeConstruct();
    ResetExitRequestState();
    BindButtonEvents();
}

void UPauseMenuWidget::NativeDestruct()
{
    bExitRequestInProgress = false;
    UnbindButtonEvents();
    AssignedTitleText.Reset();
    AssignedContinueButton.Reset();
    AssignedSettingsButton.Reset();
    AssignedExitButton.Reset();
    Super::NativeDestruct();
}

void UPauseMenuWidget::SetTitleText(UTextBlock* InTitleText)
{
    AssignedTitleText = InTitleText;
}

void UPauseMenuWidget::SetContinueButton(UButton* InButton)
{
    if (UButton* Button = AssignedContinueButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::ContinueFromUI);
    }

    AssignedContinueButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::ContinueFromUI);
        InButton->OnClicked.AddDynamic(this, &UPauseMenuWidget::ContinueFromUI);
    }
}

void UPauseMenuWidget::SetSettingsButton(UButton* InButton)
{
    if (UButton* Button = AssignedSettingsButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::OpenSettingsFromUI);
    }

    AssignedSettingsButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::OpenSettingsFromUI);
        InButton->OnClicked.AddDynamic(this, &UPauseMenuWidget::OpenSettingsFromUI);
    }
}

void UPauseMenuWidget::SetExitButton(UButton* InButton)
{
    if (UButton* Button = AssignedExitButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::ExitFromUI);
    }

    AssignedExitButton = InButton;
    if (IsValid(InButton))
    {
        // Exit travel has one owner. Legacy WBP OnClicked navigation is removed so a carried click
        // cannot issue another OpenLevel after the destination menu has loaded.
        InButton->OnClicked.Clear();
        InButton->OnClicked.AddDynamic(this, &UPauseMenuWidget::ExitFromUI);
    }
}

void UPauseMenuWidget::BindButtonEvents()
{
    if (UButton* Button = AssignedContinueButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::ContinueFromUI);
        Button->OnClicked.AddDynamic(this, &UPauseMenuWidget::ContinueFromUI);
    }
    if (UButton* Button = AssignedSettingsButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::OpenSettingsFromUI);
        Button->OnClicked.AddDynamic(this, &UPauseMenuWidget::OpenSettingsFromUI);
    }
    if (UButton* Button = AssignedExitButton.Get())
    {
        Button->OnClicked.Clear();
        Button->OnClicked.AddDynamic(this, &UPauseMenuWidget::ExitFromUI);
    }
}

void UPauseMenuWidget::UnbindButtonEvents()
{
    if (UButton* Button = AssignedContinueButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::ContinueFromUI);
    }
    if (UButton* Button = AssignedSettingsButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::OpenSettingsFromUI);
    }
    if (UButton* Button = AssignedExitButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::ExitFromUI);
    }
}

void UPauseMenuWidget::ResetExitRequestState()
{
    bExitRequestInProgress = false;
    SetIsEnabled(true);
    if (UButton* Button = AssignedExitButton.Get())
    {
        Button->SetIsEnabled(true);
    }
}

void UPauseMenuWidget::ContinueFromUI()
{
    if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(GetOwningPlayer()))
    {
        PlayerController->ClosePauseMenu(true);
    }
}

void UPauseMenuWidget::OpenSettingsFromUI()
{
    if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(GetOwningPlayer()))
    {
        PlayerController->ShowSettingsMenuFromPause();
    }
}

void UPauseMenuWidget::ExitFromUI()
{
    if (bExitRequestInProgress)
    {
        UE_LOG(LogTemp, Verbose, TEXT("[MenuTravel] Ignored a duplicate pause-menu Exit click."));
        return;
    }

    APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(GetOwningPlayer());
    if (!IsValid(PlayerController))
    {
        PlayerController = Cast<APlayerCharacterController>(UGameplayStatics::GetPlayerController(this, 0));
    }
    if (!IsValid(PlayerController))
    {
        UE_LOG(LogTemp, Error, TEXT("[MenuTravel] Pause Exit has no valid APlayerCharacterController owner."));
        return;
    }

    // Mark the click first so another serialized Blueprint listener on the same button cannot start
    // a second native request. The controller returns false without tearing down the UI when the
    // destination is not configured or the request is otherwise rejected.
    bExitRequestInProgress = true;
    if (!PlayerController->TryExitToWorldSelectionFromPauseMenu())
    {
        ResetExitRequestState();
        UE_LOG(LogTemp, Warning, TEXT("[MenuTravel] Pause Exit was rejected; the pause menu remains usable."));
        return;
    }

    SetIsEnabled(false);
    if (UButton* Button = AssignedExitButton.Get())
    {
        Button->SetIsEnabled(false);
    }
}
