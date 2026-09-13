// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/MenuButtonWidget.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"

void UMenuButtonWidget::NativeDestruct()
{
    if (UButton* Button = AssignedButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UMenuButtonWidget::HandleNativeButtonClicked);
    }

    AssignedButton.Reset();
    AssignedLabel.Reset();
    OnButtonClicked.Clear();
    Super::NativeDestruct();
}

void UMenuButtonWidget::SetButton(UButton* InButton)
{
    if (UButton* ExistingButton = AssignedButton.Get())
    {
        ExistingButton->OnClicked.RemoveDynamic(this, &UMenuButtonWidget::HandleNativeButtonClicked);
    }

    AssignedButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UMenuButtonWidget::HandleNativeButtonClicked);
        InButton->OnClicked.AddDynamic(this, &UMenuButtonWidget::HandleNativeButtonClicked);
    }
}

void UMenuButtonWidget::SetLabelTextBlock(UTextBlock* InTextBlock)
{
    AssignedLabel = InTextBlock;
    if (IsValid(InTextBlock))
    {
        InTextBlock->SetText(FText::FromString(DisplayLabel));
    }
}

void UMenuButtonWidget::ConfigureButton(const FString& InButtonKey, const FString& InDisplayLabel)
{
    ButtonKey = InButtonKey.TrimStartAndEnd();
    DisplayLabel = InDisplayLabel.TrimStartAndEnd();
    if (DisplayLabel.IsEmpty())
    {
        DisplayLabel = ButtonKey;
    }

    if (UTextBlock* Label = AssignedLabel.Get())
    {
        Label->SetText(FText::FromString(DisplayLabel));
    }

    BP_OnButtonConfigured(ButtonKey, DisplayLabel);
}

void UMenuButtonWidget::TriggerClick()
{
    if (!ButtonKey.IsEmpty())
    {
        OnButtonClicked.Broadcast(ButtonKey);
    }
}

void UMenuButtonWidget::HandleNativeButtonClicked()
{
    TriggerClick();
}
