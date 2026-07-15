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

void UPauseMenuWidget::BindButtonEvents()
{
    if (ContinueButton)
    {
        ContinueButton->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::ContinueFromUI);
        ContinueButton->OnClicked.AddDynamic(this, &UPauseMenuWidget::ContinueFromUI);
    }
    if (SettingsButton)
    {
        SettingsButton->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::OpenSettingsFromUI);
        SettingsButton->OnClicked.AddDynamic(this, &UPauseMenuWidget::OpenSettingsFromUI);
    }
    if (ExitButton)
    {
        ExitButton->OnClicked.RemoveDynamic(this, &UPauseMenuWidget::ExitFromUI);
        ExitButton->OnClicked.AddDynamic(this, &UPauseMenuWidget::ExitFromUI);
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
