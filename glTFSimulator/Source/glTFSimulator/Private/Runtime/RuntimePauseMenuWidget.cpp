// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Runtime/RuntimePauseMenuWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Character/PlayerCharacterController.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"

void URuntimePauseMenuWidget::NativeConstruct()
{
    Super::NativeConstruct();
    CacheUserWidgetReferences();
    BindButtonEvents();
}

void URuntimePauseMenuWidget::CacheUserWidgetReferences()
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
            TitleText = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("RuntimePause_TitleText")));
        }
    }
    if (!ContinueButton)
    {
        ContinueButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("ContinueButton")));
        if (!ContinueButton)
        {
            ContinueButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("RuntimePause_ContinueButton")));
        }
    }
    if (!SettingsButton)
    {
        SettingsButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("SettingsButton")));
        if (!SettingsButton)
        {
            SettingsButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("RuntimePause_SettingsButton")));
        }
    }
    if (!ExitButton)
    {
        ExitButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("ExitButton")));
        if (!ExitButton)
        {
            ExitButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("RuntimePause_ExitButton")));
        }
    }
}

void URuntimePauseMenuWidget::BindButtonEvents()
{
    if (ContinueButton)
    {
        ContinueButton->OnClicked.RemoveAll(this);
        ContinueButton->OnClicked.AddDynamic(this, &URuntimePauseMenuWidget::ContinueFromUI);
    }
    if (SettingsButton)
    {
        SettingsButton->OnClicked.RemoveAll(this);
        SettingsButton->OnClicked.AddDynamic(this, &URuntimePauseMenuWidget::OpenSettingsFromUI);
    }
    if (ExitButton)
    {
        ExitButton->OnClicked.RemoveAll(this);
        ExitButton->OnClicked.AddDynamic(this, &URuntimePauseMenuWidget::ExitFromUI);
    }
}

void URuntimePauseMenuWidget::ContinueFromUI()
{
    if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(GetOwningPlayer()))
    {
        PlayerController->ClosePauseMenu(true);
    }
}

void URuntimePauseMenuWidget::OpenSettingsFromUI()
{
    if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(GetOwningPlayer()))
    {
        PlayerController->ShowSettingsMenuFromPause();
    }
}

void URuntimePauseMenuWidget::ExitFromUI()
{
    if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(GetOwningPlayer()))
    {
        PlayerController->ExitToStartWorldFromPauseMenu();
    }
}
