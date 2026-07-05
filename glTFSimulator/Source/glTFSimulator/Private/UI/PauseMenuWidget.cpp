// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/PauseMenuWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Character/PlayerCharacterController.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"

void UPauseMenuWidget::NativeConstruct()
{
    Super::NativeConstruct();
    CacheUserWidgetReferences();
    BindButtonEvents();
}

void UPauseMenuWidget::CacheUserWidgetReferences()
{
    if (!WidgetTree)
    {
        return;
    }

    if (!TitleText)
    {
        TitleText = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("TitleText")));
        if (!TitleText)
        {
            TitleText = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("Pause_TitleText")));
        }
    }
    if (!ContinueButton)
    {
        ContinueButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("ContinueButton")));
        if (!ContinueButton)
        {
            ContinueButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("Pause_ContinueButton")));
        }
    }
    if (!SettingsButton)
    {
        SettingsButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("SettingsButton")));
        if (!SettingsButton)
        {
            SettingsButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("Pause_SettingsButton")));
        }
    }
    if (!ExitButton)
    {
        ExitButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("ExitButton")));
        if (!ExitButton)
        {
            ExitButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("Pause_ExitButton")));
        }
    }
}

void UPauseMenuWidget::BindButtonEvents()
{
    if (ContinueButton)
    {
        ContinueButton->OnClicked.RemoveAll(this);
        ContinueButton->OnClicked.AddDynamic(this, &UPauseMenuWidget::ContinueFromUI);
    }
    if (SettingsButton)
    {
        SettingsButton->OnClicked.RemoveAll(this);
        SettingsButton->OnClicked.AddDynamic(this, &UPauseMenuWidget::OpenSettingsFromUI);
    }
    if (ExitButton)
    {
        ExitButton->OnClicked.RemoveAll(this);
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
