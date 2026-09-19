// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/SelectionWidgetBase.h"

#include "Components/PanelWidget.h"
#include "System/GlTFSimulatorAssetRegistry.h"
#include "System/GlTFSimulatorGameInstance.h"
#include "UI/MenuButtonWidget.h"

void USelectionWidgetBase::NativeDestruct()
{
    ClearGeneratedSelectionEntries();
    SelectionListPanel.Reset();
    ResolvedSelectionEntryWidgetClass = nullptr;
    Super::NativeDestruct();
}

UClass* USelectionWidgetBase::ResolveSelectionEntryWidgetClass()
{
    if (IsValid(ResolvedSelectionEntryWidgetClass))
    {
        return ResolvedSelectionEntryWidgetClass.Get();
    }

    UGlTFSimulatorAssetRegistry* Registry = UGlTFSimulatorGameInstance::GetAssetRegistryFromContext(this);
    if (!IsValid(Registry) || Registry->SelectionEntryWidgetClass.IsNull())
    {
        return nullptr;
    }

    UClass* LoadedClass = Registry->SelectionEntryWidgetClass.LoadSynchronous();
    if (!IsValid(LoadedClass) || !LoadedClass->IsChildOf(UMenuButtonWidget::StaticClass()))
    {
        UE_LOG(LogTemp, Error, TEXT("AssetRegistry SelectionEntryWidgetClass is invalid."));
        return nullptr;
    }

    ResolvedSelectionEntryWidgetClass = LoadedClass;
    return LoadedClass;
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

    UClass* const EntryClass = ResolveSelectionEntryWidgetClass();
    if (!IsValid(EntryClass))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("SelectionWidgetBase cannot create '%s' because AssetRegistry.SelectionEntryWidgetClass is not assigned."),
            *NormalizedKey);
        return nullptr;
    }

    UMenuButtonWidget* const EntryWidget = Cast<UMenuButtonWidget>(
        UUserWidget::CreateWidgetInstance(*this, EntryClass, NAME_None));

    if (!IsValid(EntryWidget))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("SelectionWidgetBase failed to create SelectionEntryWidgetClass for '%s'."),
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
