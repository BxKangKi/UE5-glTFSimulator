// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/PauseMenuWidget.h"
#include "Character/PlayerCharacterController.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"

void UPauseMenuWidget::NativeConstruct()
{
    Super::NativeConstruct();
    BindButtonEvents();
}

void UPauseMenuWidget::NativeDestruct()
{
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
        InButton->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::ExitFromUI);
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
        Button->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::ExitFromUI);
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
    if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(GetOwningPlayer()))
    {
        PlayerController->ExitToWorldSelectionFromPauseMenu();
    }
}
