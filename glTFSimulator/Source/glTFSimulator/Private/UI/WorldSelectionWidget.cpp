// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/WorldSelectionWidget.h"

#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/GridSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBoxSlot.h"

namespace
{
    FString NormalizeWorldSelectionText(FString Value)
    {
        Value.TrimStartAndEndInline();
        return Value;
    }

    FVector2D MakeCanvasAlignment(EHorizontalAlignment HorizontalAlignment, EVerticalAlignment VerticalAlignment)
    {
        float X = 0.0f;
        if (HorizontalAlignment == HAlign_Center)
        {
            X = 0.5f;
        }
        else if (HorizontalAlignment == HAlign_Right)
        {
            X = 1.0f;
        }

        float Y = 0.0f;
        if (VerticalAlignment == VAlign_Center)
        {
            Y = 0.5f;
        }
        else if (VerticalAlignment == VAlign_Bottom)
        {
            Y = 1.0f;
        }

        return FVector2D(X, Y);
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
        UE_LOG(LogTemp, Warning, TEXT("Generated world button cannot open %s because its owning selection widget is not valid."), *WorldFolderName);
        return;
    }

    // Disable this widget before starting travel so a second Blueprint/parent-button callback from
    // the same mouse release cannot route the UI back to the MainWorld start screen.
    Widget->SetIsEnabled(false);
    if (!Widget->OpenWorldByFolderName(WorldFolderName))
    {
        Widget->SetIsEnabled(true);
    }
}

void UWorldSelectionWidget::NativeConstruct()
{
    Super::NativeConstruct();

    if (AssignedWorldListPanel.IsValid())
    {
        RebuildWorldButtons();
    }
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
    if (AssignedWorldListPanel.Get() == InWorldListPanel)
    {
        return;
    }

    ClearWorldButtons();
    AssignedWorldListPanel = InWorldListPanel;
    RebuildWorldButtons();
}

void UWorldSelectionWidget::RebuildWorldButtons()
{
    ClearWorldButtons();

    UPanelWidget* const ListPanel = AssignedWorldListPanel.Get();
    if (!IsValid(ListPanel))
    {
        UE_LOG(LogTemp, Warning, TEXT("WorldSelectionWidget cannot build world buttons because the list panel is not assigned. Pass the panel reference from the widget construct event."));
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
        UButton* Button = nullptr;
        UTextBlock* Label = nullptr;
        UWidget* EntryWidget = CreateWorldButtonEntry(EntryData.Key, EntryData.Value, Button, Label);
        if (!IsValid(EntryWidget) || !IsValid(Button) || !IsValid(Label))
        {
            UE_LOG(LogTemp, Warning, TEXT("WorldSelectionWidget failed to create a generated button for %s."), *EntryData.Key);
            continue;
        }

        ListPanel->AddChild(EntryWidget);
        ApplyGeneratedEntrySlotLayout(EntryWidget);

        GeneratedWorldButtonEntries.Add(EntryWidget);
        GeneratedWorldButtons.Add(Button);
        GeneratedWorldButtonLabels.Add(Label);
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
    }

    for (UWidget* EntryWidget : GeneratedWorldButtonEntries)
    {
        if (IsValid(EntryWidget))
        {
            EntryWidget->RemoveFromParent();
        }
    }

    GeneratedWorldButtonEntries.Empty();
    GeneratedWorldButtons.Empty();
    GeneratedWorldButtonLabels.Empty();
    GeneratedWorldButtonSizeBoxes.Empty();
    GeneratedClickHandlers.Empty();
}

void UWorldSelectionWidget::SetGeneratedButtonSize(const FVector2D& InButtonSize)
{
    GeneratedButtonSize.X = FMath::Max(0.0f, InButtonSize.X);
    GeneratedButtonSize.Y = FMath::Max(0.0f, InButtonSize.Y);
    RefreshGeneratedButtonLayout();
}

void UWorldSelectionWidget::SetGeneratedButtonPanelAlignment(EHorizontalAlignment InHorizontalAlignment, EVerticalAlignment InVerticalAlignment)
{
    GeneratedButtonPanelHorizontalAlignment = InHorizontalAlignment;
    GeneratedButtonPanelVerticalAlignment = InVerticalAlignment;
    RefreshGeneratedButtonLayout();
}

void UWorldSelectionWidget::SetGeneratedButtonContentAlignment(EHorizontalAlignment InHorizontalAlignment, EVerticalAlignment InVerticalAlignment)
{
    GeneratedButtonContentHorizontalAlignment = InHorizontalAlignment;
    GeneratedButtonContentVerticalAlignment = InVerticalAlignment;
    RefreshGeneratedButtonLayout();
}

void UWorldSelectionWidget::SetGeneratedButtonStyleOverrideEnabled(bool bInOverrideStyle)
{
    bOverrideGeneratedButtonWidgetStyle = bInOverrideStyle;
    RefreshGeneratedButtonLayout();
}

void UWorldSelectionWidget::SetGeneratedButtonWidgetStyle(const FButtonStyle& InButtonStyle)
{
    GeneratedButtonWidgetStyle = InButtonStyle;
    bOverrideGeneratedButtonWidgetStyle = true;
    RefreshGeneratedButtonLayout();
}

void UWorldSelectionWidget::SetGeneratedButtonColors(const FLinearColor& InColorAndOpacity, const FLinearColor& InBackgroundColor)
{
    GeneratedButtonColorAndOpacity = InColorAndOpacity;
    GeneratedButtonBackgroundColor = InBackgroundColor;
    RefreshGeneratedButtonLayout();
}

void UWorldSelectionWidget::SetGeneratedButtonFontSize(int32 InFontSize)
{
    GeneratedButtonFontSize = FMath::Max(1, InFontSize);
    RefreshGeneratedButtonLayout();
}

void UWorldSelectionWidget::SetGeneratedButtonTypeface(FName InTypefaceFontName)
{
    GeneratedButtonTypefaceFontName = InTypefaceFontName;
    RefreshGeneratedButtonLayout();
}

void UWorldSelectionWidget::SetGeneratedButtonOutlineSettings(const FFontOutlineSettings& InOutlineSettings)
{
    GeneratedButtonOutlineSettings = InOutlineSettings;
    RefreshGeneratedButtonLayout();
}

void UWorldSelectionWidget::SetGeneratedButtonTextShadow(const FVector2D& InShadowOffset, const FLinearColor& InShadowColor)
{
    GeneratedButtonTextShadowOffset = InShadowOffset;
    GeneratedButtonTextShadowColor = InShadowColor;
    RefreshGeneratedButtonLayout();
}

void UWorldSelectionWidget::RefreshGeneratedButtonLayout()
{
    for (USizeBox* SizeBox : GeneratedWorldButtonSizeBoxes)
    {
        ApplyGeneratedEntrySize(SizeBox);
    }

    for (UWidget* EntryWidget : GeneratedWorldButtonEntries)
    {
        ApplyGeneratedEntrySlotLayout(EntryWidget);
    }

    for (UButton* Button : GeneratedWorldButtons)
    {
        ApplyGeneratedButtonStyle(Button);
        ApplyGeneratedButtonContentLayout(Button);
    }

    for (UTextBlock* Label : GeneratedWorldButtonLabels)
    {
        ApplyGeneratedLabelLayout(Label);
    }
}

UWidget* UWorldSelectionWidget::CreateWorldButtonEntry(const FString& WorldFolderName, const FString& DisplayName, UButton*& OutButton, UTextBlock*& OutLabel)
{
    OutButton = nullptr;
    OutLabel = nullptr;

    USizeBox* EntrySizeBox = NewObject<USizeBox>(this);
    UButton* Button = NewObject<UButton>(this);
    UTextBlock* TextBlock = NewObject<UTextBlock>(this);
    UWorldSelectionButtonClickHandler* ClickHandler = NewObject<UWorldSelectionButtonClickHandler>(this);
    if (!IsValid(EntrySizeBox) || !IsValid(Button) || !IsValid(TextBlock) || !IsValid(ClickHandler))
    {
        return nullptr;
    }

    EntrySizeBox->ClearFlags(RF_Transactional);
    EntrySizeBox->SetFlags(RF_Transient);
    ApplyGeneratedEntrySize(EntrySizeBox);

    Button->ClearFlags(RF_Transactional);
    Button->SetFlags(RF_Transient);
    ApplyGeneratedButtonStyle(Button);

    TextBlock->ClearFlags(RF_Transactional);
    TextBlock->SetFlags(RF_Transient);
    TextBlock->SetText(FText::FromString(DisplayName));
    ApplyGeneratedLabelLayout(TextBlock);

    if (UButtonSlot* ButtonSlot = Cast<UButtonSlot>(Button->AddChild(TextBlock)))
    {
        ButtonSlot->SetPadding(GeneratedButtonTextPadding);
        ButtonSlot->SetHorizontalAlignment(GeneratedButtonContentHorizontalAlignment.GetValue());
        ButtonSlot->SetVerticalAlignment(GeneratedButtonContentVerticalAlignment.GetValue());
    }

    EntrySizeBox->AddChild(Button);

    ClickHandler->Setup(this, WorldFolderName);
    Button->OnClicked.AddDynamic(ClickHandler, &UWorldSelectionButtonClickHandler::HandleClicked);
    GeneratedClickHandlers.Add(ClickHandler);
    GeneratedWorldButtonSizeBoxes.Add(EntrySizeBox);

    OutButton = Button;
    OutLabel = TextBlock;
    return EntrySizeBox;
}

void UWorldSelectionWidget::ApplyGeneratedEntrySize(USizeBox* EntrySizeBox) const
{
    if (!IsValid(EntrySizeBox))
    {
        return;
    }

    if (GeneratedButtonSize.X > 0.0f)
    {
        EntrySizeBox->SetWidthOverride(GeneratedButtonSize.X);
    }
    else
    {
        EntrySizeBox->ClearWidthOverride();
    }

    if (GeneratedButtonSize.Y > 0.0f)
    {
        EntrySizeBox->SetHeightOverride(GeneratedButtonSize.Y);
    }
    else
    {
        EntrySizeBox->ClearHeightOverride();
    }
}

void UWorldSelectionWidget::ApplyGeneratedEntrySlotLayout(UWidget* EntryWidget) const
{
    if (!IsValid(EntryWidget) || !EntryWidget->Slot)
    {
        return;
    }

    UPanelSlot* const ParentPanelSlot = EntryWidget->Slot;
    const EHorizontalAlignment HorizontalAlignment = GeneratedButtonPanelHorizontalAlignment.GetValue();
    const EVerticalAlignment VerticalAlignment = GeneratedButtonPanelVerticalAlignment.GetValue();

    if (UScrollBoxSlot* ScrollBoxPanelSlot = Cast<UScrollBoxSlot>(ParentPanelSlot))
    {
        ScrollBoxPanelSlot->SetPadding(GeneratedButtonEntryPadding);
        ScrollBoxPanelSlot->SetHorizontalAlignment(HorizontalAlignment);
        ScrollBoxPanelSlot->SetVerticalAlignment(VerticalAlignment);
        return;
    }

    if (UVerticalBoxSlot* VerticalBoxPanelSlot = Cast<UVerticalBoxSlot>(ParentPanelSlot))
    {
        VerticalBoxPanelSlot->SetPadding(GeneratedButtonEntryPadding);
        VerticalBoxPanelSlot->SetHorizontalAlignment(HorizontalAlignment);
        VerticalBoxPanelSlot->SetVerticalAlignment(VerticalAlignment);
        return;
    }

    if (UHorizontalBoxSlot* HorizontalBoxPanelSlot = Cast<UHorizontalBoxSlot>(ParentPanelSlot))
    {
        HorizontalBoxPanelSlot->SetPadding(GeneratedButtonEntryPadding);
        HorizontalBoxPanelSlot->SetHorizontalAlignment(HorizontalAlignment);
        HorizontalBoxPanelSlot->SetVerticalAlignment(VerticalAlignment);
        return;
    }

    if (UOverlaySlot* OverlayPanelSlot = Cast<UOverlaySlot>(ParentPanelSlot))
    {
        OverlayPanelSlot->SetPadding(GeneratedButtonEntryPadding);
        OverlayPanelSlot->SetHorizontalAlignment(HorizontalAlignment);
        OverlayPanelSlot->SetVerticalAlignment(VerticalAlignment);
        return;
    }

    if (UGridSlot* GridPanelSlot = Cast<UGridSlot>(ParentPanelSlot))
    {
        GridPanelSlot->SetPadding(GeneratedButtonEntryPadding);
        GridPanelSlot->SetHorizontalAlignment(HorizontalAlignment);
        GridPanelSlot->SetVerticalAlignment(VerticalAlignment);
        return;
    }

    if (UUniformGridSlot* UniformGridPanelSlot = Cast<UUniformGridSlot>(ParentPanelSlot))
    {
        UniformGridPanelSlot->SetHorizontalAlignment(HorizontalAlignment);
        UniformGridPanelSlot->SetVerticalAlignment(VerticalAlignment);
        return;
    }

    if (UCanvasPanelSlot* CanvasPanelSlot = Cast<UCanvasPanelSlot>(ParentPanelSlot))
    {
        if (GeneratedButtonSize.X > 0.0f && GeneratedButtonSize.Y > 0.0f)
        {
            CanvasPanelSlot->SetSize(GeneratedButtonSize);
        }
        CanvasPanelSlot->SetAlignment(MakeCanvasAlignment(HorizontalAlignment, VerticalAlignment));
    }
}

void UWorldSelectionWidget::ApplyGeneratedButtonStyle(UButton* Button) const
{
    if (!IsValid(Button))
    {
        return;
    }

    if (bOverrideGeneratedButtonWidgetStyle)
    {
        Button->SetStyle(GeneratedButtonWidgetStyle);
    }

    Button->SetColorAndOpacity(GeneratedButtonColorAndOpacity);
    Button->SetBackgroundColor(GeneratedButtonBackgroundColor);
}

void UWorldSelectionWidget::ApplyGeneratedButtonContentLayout(UButton* Button) const
{
    if (!IsValid(Button))
    {
        return;
    }

    if (UButtonSlot* ButtonSlot = Cast<UButtonSlot>(Button->GetContentSlot()))
    {
        ButtonSlot->SetPadding(GeneratedButtonTextPadding);
        ButtonSlot->SetHorizontalAlignment(GeneratedButtonContentHorizontalAlignment.GetValue());
        ButtonSlot->SetVerticalAlignment(GeneratedButtonContentVerticalAlignment.GetValue());
    }
}

void UWorldSelectionWidget::ApplyGeneratedLabelLayout(UTextBlock* Label) const
{
    if (!IsValid(Label))
    {
        return;
    }

    FSlateFontInfo FontInfo = Label->GetFont();
    FontInfo.Size = FMath::Max(1, GeneratedButtonFontSize);
    if (!GeneratedButtonTypefaceFontName.IsNone())
    {
        FontInfo.TypefaceFontName = GeneratedButtonTypefaceFontName;
    }
    FontInfo.OutlineSettings = GeneratedButtonOutlineSettings;
    Label->SetFont(FontInfo);

    Label->SetJustification(GeneratedButtonTextJustification.GetValue());
    Label->SetColorAndOpacity(GeneratedButtonTextColor);
    Label->SetShadowOffset(GeneratedButtonTextShadowOffset);
    Label->SetShadowColorAndOpacity(GeneratedButtonTextShadowColor);
}
