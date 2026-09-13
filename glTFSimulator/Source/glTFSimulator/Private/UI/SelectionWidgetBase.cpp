// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/SelectionWidgetBase.h"

#include "Components/PanelWidget.h"
#include "UI/MenuButtonWidget.h"

void USelectionWidgetBase::NativeDestruct()
{
    ClearGeneratedSelectionEntries();
    SelectionListPanel.Reset();
    Super::NativeDestruct();
}

void USelectionWidgetBase::SetSelectionButtonWidgetClass(TSubclassOf<UMenuButtonWidget> InWidgetClass)
{
    if (SelectionButtonWidgetClass == InWidgetClass)
    {
        return;
    }

    SelectionButtonWidgetClass = InWidgetClass;
    ClearGeneratedSelectionEntries();
}

void USelectionWidgetBase::HandleSelectionButtonClicked(const FString& SelectionKey)
{
    const FString NormalizedKey = SelectionKey.TrimStartAndEnd();
    if (!NormalizedKey.IsEmpty())
    {
        OnSelectionEntryActivated(NormalizedKey);
    }
}

void USelectionWidgetBase::SetSelectionListPanel(UPanelWidget* InPanel)
{
    if (SelectionListPanel.Get() == InPanel)
    {
        return;
    }

    ClearGeneratedSelectionEntries();
    SelectionListPanel = InPanel;
}

UWidget* USelectionWidgetBase::AddGeneratedSelectionEntry(
    const FString& SelectionKey,
    const FString& DisplayName)
{
    UPanelWidget* const Panel = SelectionListPanel.Get();
    const FString NormalizedKey = SelectionKey.TrimStartAndEnd();
    if (!IsValid(Panel) || NormalizedKey.IsEmpty())
    {
        return nullptr;
    }

    if (!SelectionButtonWidgetClass)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("SelectionWidgetBase cannot create '%s' because SelectionButtonWidgetClass is not assigned."),
            *NormalizedKey);
        return nullptr;
    }

    UMenuButtonWidget* const EntryWidget = Cast<UMenuButtonWidget>(
        UUserWidget::CreateWidgetInstance(*this, SelectionButtonWidgetClass, NAME_None));

    if (!IsValid(EntryWidget))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("SelectionWidgetBase failed to create SelectionButtonWidgetClass for '%s'."),
            *NormalizedKey);
        return nullptr;
    }

    FString NormalizedDisplayName = DisplayName.TrimStartAndEnd();
    if (NormalizedDisplayName.IsEmpty())
    {
        NormalizedDisplayName = NormalizedKey;
    }

    EntryWidget->ConfigureButton(NormalizedKey, NormalizedDisplayName);
    EntryWidget->OnButtonClicked.RemoveDynamic(this, &USelectionWidgetBase::HandleSelectionButtonClicked);
    EntryWidget->OnButtonClicked.AddDynamic(this, &USelectionWidgetBase::HandleSelectionButtonClicked);
    Panel->AddChild(EntryWidget);

    GeneratedSelectionEntries.Add(EntryWidget);
    return EntryWidget;
}

void USelectionWidgetBase::ClearGeneratedSelectionEntries()
{
    for (UMenuButtonWidget* EntryWidget : GeneratedSelectionEntries)
    {
        if (!IsValid(EntryWidget))
        {
            continue;
        }

        EntryWidget->OnButtonClicked.RemoveDynamic(this, &USelectionWidgetBase::HandleSelectionButtonClicked);
        EntryWidget->RemoveFromParent();
    }

    GeneratedSelectionEntries.Reset();
}

void USelectionWidgetBase::OnSelectionEntryActivated(const FString& SelectionKey)
{
    UE_LOG(LogTemp, Warning,
        TEXT("SelectionWidgetBase received an unhandled selection: %s"), *SelectionKey);
}
