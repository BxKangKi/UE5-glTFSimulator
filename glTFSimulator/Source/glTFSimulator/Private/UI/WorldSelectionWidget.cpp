// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/WorldSelectionWidget.h"

#include "Components/PanelWidget.h"

namespace
{
    FString NormalizeWorldSelectionText(FString Value)
    {
        Value.TrimStartAndEndInline();
        return Value;
    }
}

void UWorldSelectionWidget::NativeConstruct()
{
    Super::NativeConstruct();

    if (IsValid(GetSelectionListPanel()))
    {
        RebuildWorldButtons();
    }
}

void UWorldSelectionWidget::SetWorldSelectionData(const TMap<FString, FString>& Values)
{
    Super::SetWorldSelectionData(Values);
    RebuildWorldButtons();
}

void UWorldSelectionWidget::SetWorldListPanel(UPanelWidget* InWorldListPanel)
{
    SetSelectionListPanel(InWorldListPanel);
    RebuildWorldButtons();
}

void UWorldSelectionWidget::RebuildWorldButtons()
{
    ClearGeneratedSelectionEntries();

    if (!IsValid(GetSelectionListPanel()))
    {
        // Blueprint commonly assigns the panel from Construct after selection data arrives.
        // Defer generation silently; SetWorldListPanel() rebuilds the list immediately.
        return;
    }

    TArray<TPair<FString, FString>> SortedWorlds;
    const TMap<FString, FString> FolderMap = GetFolderNameMap();
    for (const TPair<FString, FString>& Pair : FolderMap)
    {
        const FString FolderName = NormalizeWorldSelectionText(Pair.Key);
        if (FolderName.IsEmpty())
        {
            continue;
        }

        FString DisplayName = NormalizeWorldSelectionText(Pair.Value);
        if (DisplayName.IsEmpty())
        {
            DisplayName = FolderName;
        }
        SortedWorlds.Emplace(FolderName, DisplayName);
    }

    SortedWorlds.Sort([](const TPair<FString, FString>& Left, const TPair<FString, FString>& Right)
    {
        const int32 DisplayCompare = Left.Value.Compare(Right.Value, ESearchCase::IgnoreCase);
        return DisplayCompare != 0
            ? DisplayCompare < 0
            : Left.Key.Compare(Right.Key, ESearchCase::IgnoreCase) < 0;
    });

    for (const TPair<FString, FString>& EntryData : SortedWorlds)
    {
        if (!IsValid(AddGeneratedSelectionEntry(EntryData.Key, EntryData.Value)))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("WorldSelectionWidget failed to create a generated button for %s."),
                *EntryData.Key);
        }
    }
}

void UWorldSelectionWidget::ClearWorldButtons()
{
    ClearGeneratedSelectionEntries();
}

void UWorldSelectionWidget::OnSelectionEntryActivated(const FString& SelectionKey)
{
    // Disable before travel so a second Blueprint/parent-button callback from the same mouse
    // release cannot route the UI back to the start screen.
    SetIsEnabled(false);
    if (!OpenWorldByFolderName(SelectionKey))
    {
        SetIsEnabled(true);
    }
}
