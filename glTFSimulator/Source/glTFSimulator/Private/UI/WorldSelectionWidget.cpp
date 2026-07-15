// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/WorldSelectionWidget.h"

#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"

namespace
{
    FString NormalizeWorldSelectionText(FString Value)
    {
        Value.TrimStartAndEndInline();
        return Value;
    }
}

void UWorldSelectionButtonClickHandler::Setup(UWorldSelectionWidget* InOwnerWidget, const FString& InWorldFolderName)
{
    OwnerWidget = InOwnerWidget;
    WorldFolderName = NormalizeWorldSelectionText(InWorldFolderName);
}

void UWorldSelectionButtonClickHandler::HandleClicked()
{
    UWorldSelectionWidget* Widget = OwnerWidget.Get();
    if (!IsValid(Widget))
    {
        UE_LOG(LogTemp, Warning, TEXT("Generated world button cannot open %s because its owning WBP_LevelMenu widget is not valid."), *WorldFolderName);
        return;
    }

    Widget->OpenWorldByFolderName(WorldFolderName);
}

void UWorldSelectionWidget::NativeConstruct()
{
    Super::NativeConstruct();
    RebuildWorldButtons();
}

void UWorldSelectionWidget::NativeDestruct()
{
    ClearWorldButtons();
    Super::NativeDestruct();
}

void UWorldSelectionWidget::SetWorldSelectionData(const TMap<FString, FString>& Values)
{
    Super::SetWorldSelectionData(Values);
    RebuildWorldButtons();
}

void UWorldSelectionWidget::SetWorldListPanel(UPanelWidget* InWorldListPanel)
{
    if (WorldListPanel == InWorldListPanel)
    {
        return;
    }

    ClearWorldButtons();
    WorldListPanel = InWorldListPanel;
    RebuildWorldButtons();
}

void UWorldSelectionWidget::RebuildWorldButtons()
{
    ClearWorldButtons();

    if (!WorldListPanel)
    {
        UE_LOG(LogTemp, Warning, TEXT("WorldSelectionWidget cannot build world buttons because WorldListPanel is not assigned in WBP_LevelMenu."));
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
        if (DisplayCompare != 0)
        {
            return DisplayCompare < 0;
        }

        return Left.Key.Compare(Right.Key, ESearchCase::IgnoreCase) < 0;
    });

    for (const TPair<FString, FString>& EntryData : SortedWorlds)
    {
        UButton* Button = CreateWorldButton(EntryData.Key, EntryData.Value);
        if (!IsValid(Button))
        {
            UE_LOG(LogTemp, Warning, TEXT("WorldSelectionWidget failed to create a generated button for %s."), *EntryData.Key);
            continue;
        }

        WorldListPanel->AddChild(Button);
        GeneratedWorldButtons.Add(Button);
    }
}

void UWorldSelectionWidget::ClearWorldButtons()
{
    const int32 Count = GeneratedWorldButtons.Num();
    for (int32 Index = 0; Index < Count; ++Index)
    {
        UButton* Button = GeneratedWorldButtons[Index];
        UWorldSelectionButtonClickHandler* Handler = GeneratedClickHandlers.IsValidIndex(Index) ? GeneratedClickHandlers[Index] : nullptr;
        if (IsValid(Button) && IsValid(Handler))
        {
            Button->OnClicked.RemoveDynamic(Handler, &UWorldSelectionButtonClickHandler::HandleClicked);
        }

        if (IsValid(Button))
        {
            Button->RemoveFromParent();
        }
    }

    GeneratedWorldButtons.Empty();
    GeneratedClickHandlers.Empty();
}

UButton* UWorldSelectionWidget::CreateWorldButton(const FString& WorldFolderName, const FString& DisplayName)
{
    UButton* Button = NewObject<UButton>(this);
    UTextBlock* TextBlock = NewObject<UTextBlock>(this);
    UWorldSelectionButtonClickHandler* ClickHandler = NewObject<UWorldSelectionButtonClickHandler>(this);
    if (!IsValid(Button) || !IsValid(TextBlock) || !IsValid(ClickHandler))
    {
        return nullptr;
    }

    Button->ClearFlags(RF_Transactional);
    Button->SetFlags(RF_Transient);
    // UE 5.7 exposes focusability as a UButton property, not as a runtime setter.
    Button->IsFocusable = true;

    TextBlock->ClearFlags(RF_Transactional);
    TextBlock->SetFlags(RF_Transient);
    TextBlock->SetText(FText::FromString(DisplayName));
    TextBlock->SetJustification(ETextJustify::Center);
    TextBlock->SetColorAndOpacity(GeneratedButtonTextColor);

    if (UButtonSlot* ButtonSlot = Cast<UButtonSlot>(Button->AddChild(TextBlock)))
    {
        ButtonSlot->SetPadding(GeneratedButtonTextPadding);
        ButtonSlot->SetHorizontalAlignment(HAlign_Center);
        ButtonSlot->SetVerticalAlignment(VAlign_Center);
    }

    ClickHandler->Setup(this, WorldFolderName);
    Button->OnClicked.AddDynamic(ClickHandler, &UWorldSelectionButtonClickHandler::HandleClicked);
    GeneratedClickHandlers.Add(ClickHandler);

    return Button;
}
