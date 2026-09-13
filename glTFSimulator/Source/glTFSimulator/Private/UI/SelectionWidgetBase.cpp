// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/SelectionWidgetBase.h"

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
    FVector2D MakeSelectionCanvasAlignment(
        const EHorizontalAlignment HorizontalAlignment,
        const EVerticalAlignment VerticalAlignment)
    {
        const float X = HorizontalAlignment == HAlign_Center ? 0.5f
            : HorizontalAlignment == HAlign_Right ? 1.0f : 0.0f;
        const float Y = VerticalAlignment == VAlign_Center ? 0.5f
            : VerticalAlignment == VAlign_Bottom ? 1.0f : 0.0f;
        return FVector2D(X, Y);
    }
}

void USelectionButtonClickHandler::Setup(
    USelectionWidgetBase* InOwnerWidget,
    const FString& InSelectionKey)
{
    OwnerWidget = InOwnerWidget;
    SelectionKey = InSelectionKey.TrimStartAndEnd();
}

void USelectionButtonClickHandler::HandleClicked()
{
    if (USelectionWidgetBase* Owner = OwnerWidget.Get())
    {
        Owner->HandleGeneratedSelectionClicked(SelectionKey);
    }
}

void USelectionWidgetBase::NativeDestruct()
{
    ClearGeneratedSelectionEntries();
    SelectionListPanel.Reset();
    Super::NativeDestruct();
}

void USelectionWidgetBase::HandleGeneratedSelectionClicked(const FString& SelectionKey)
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

    USizeBox* const EntrySizeBox = NewObject<USizeBox>(this, NAME_None, RF_Transient);
    UButton* const Button = NewObject<UButton>(this, NAME_None, RF_Transient);
    UTextBlock* const Label = NewObject<UTextBlock>(this, NAME_None, RF_Transient);
    USelectionButtonClickHandler* const ClickHandler =
        NewObject<USelectionButtonClickHandler>(this, NAME_None, RF_Transient);
    if (!IsValid(EntrySizeBox) || !IsValid(Button) || !IsValid(Label) || !IsValid(ClickHandler))
    {
        return nullptr;
    }

    EntrySizeBox->ClearFlags(RF_Transactional);
    Button->ClearFlags(RF_Transactional);
    Label->ClearFlags(RF_Transactional);
    ClickHandler->ClearFlags(RF_Transactional);

    ApplyGeneratedEntrySize(EntrySizeBox);
    ApplyGeneratedButtonStyle(Button);

    FString NormalizedDisplayName = DisplayName.TrimStartAndEnd();
    if (NormalizedDisplayName.IsEmpty())
    {
        NormalizedDisplayName = NormalizedKey;
    }
    Label->SetText(FText::FromString(NormalizedDisplayName));
    ApplyGeneratedLabelLayout(Label);

    if (UButtonSlot* const ButtonSlot = Cast<UButtonSlot>(Button->AddChild(Label)))
    {
        ButtonSlot->SetPadding(GeneratedButtonTextPadding);
        ButtonSlot->SetHorizontalAlignment(GeneratedButtonContentHorizontalAlignment.GetValue());
        ButtonSlot->SetVerticalAlignment(GeneratedButtonContentVerticalAlignment.GetValue());
    }

    EntrySizeBox->AddChild(Button);
    Panel->AddChild(EntrySizeBox);
    ApplyGeneratedEntrySlotLayout(EntrySizeBox);

    ClickHandler->Setup(this, NormalizedKey);
    Button->OnClicked.AddDynamic(ClickHandler, &USelectionButtonClickHandler::HandleClicked);

    GeneratedSelectionEntries.Add(EntrySizeBox);
    GeneratedSelectionButtons.Add(Button);
    GeneratedSelectionLabels.Add(Label);
    GeneratedSelectionSizeBoxes.Add(EntrySizeBox);
    GeneratedSelectionClickHandlers.Add(ClickHandler);
    return EntrySizeBox;
}

void USelectionWidgetBase::ClearGeneratedSelectionEntries()
{
    const int32 Count = GeneratedSelectionButtons.Num();
    for (int32 Index = 0; Index < Count; ++Index)
    {
        UButton* const Button = GeneratedSelectionButtons[Index];
        USelectionButtonClickHandler* const Handler =
            GeneratedSelectionClickHandlers.IsValidIndex(Index)
                ? GeneratedSelectionClickHandlers[Index].Get() : nullptr;
        if (IsValid(Button) && IsValid(Handler))
        {
            Button->OnClicked.RemoveDynamic(Handler, &USelectionButtonClickHandler::HandleClicked);
        }
    }

    for (UWidget* EntryWidget : GeneratedSelectionEntries)
    {
        if (IsValid(EntryWidget))
        {
            EntryWidget->RemoveFromParent();
        }
    }

    GeneratedSelectionEntries.Reset();
    GeneratedSelectionButtons.Reset();
    GeneratedSelectionLabels.Reset();
    GeneratedSelectionSizeBoxes.Reset();
    GeneratedSelectionClickHandlers.Reset();
}

void USelectionWidgetBase::OnSelectionEntryActivated(const FString& SelectionKey)
{
    UE_LOG(LogTemp, Verbose,
        TEXT("SelectionWidgetBase received unhandled generated selection: %s"), *SelectionKey);
}

void USelectionWidgetBase::SetGeneratedButtonSize(const FVector2D& InButtonSize)
{
    GeneratedButtonSize.X = FMath::Max(0.0f, InButtonSize.X);
    GeneratedButtonSize.Y = FMath::Max(0.0f, InButtonSize.Y);
    RefreshGeneratedButtonLayout();
}

void USelectionWidgetBase::SetGeneratedButtonPanelAlignment(
    EHorizontalAlignment InHorizontalAlignment,
    EVerticalAlignment InVerticalAlignment)
{
    GeneratedButtonPanelHorizontalAlignment = InHorizontalAlignment;
    GeneratedButtonPanelVerticalAlignment = InVerticalAlignment;
    RefreshGeneratedButtonLayout();
}

void USelectionWidgetBase::SetGeneratedButtonContentAlignment(
    EHorizontalAlignment InHorizontalAlignment,
    EVerticalAlignment InVerticalAlignment)
{
    GeneratedButtonContentHorizontalAlignment = InHorizontalAlignment;
    GeneratedButtonContentVerticalAlignment = InVerticalAlignment;
    RefreshGeneratedButtonLayout();
}

void USelectionWidgetBase::SetGeneratedButtonEntryPadding(const FMargin& InPadding)
{
    GeneratedButtonEntryPadding = InPadding;
    RefreshGeneratedButtonLayout();
}

void USelectionWidgetBase::SetGeneratedButtonTextPadding(const FMargin& InPadding)
{
    GeneratedButtonTextPadding = InPadding;
    RefreshGeneratedButtonLayout();
}

void USelectionWidgetBase::SetGeneratedButtonTextJustification(
    const ETextJustify::Type InJustification)
{
    GeneratedButtonTextJustification = InJustification;
    RefreshGeneratedButtonLayout();
}

void USelectionWidgetBase::SetGeneratedButtonStyleOverrideEnabled(bool bInOverrideStyle)
{
    bOverrideGeneratedButtonWidgetStyle = bInOverrideStyle;
    RefreshGeneratedButtonLayout();
}

void USelectionWidgetBase::SetGeneratedButtonWidgetStyle(const FButtonStyle& InButtonStyle)
{
    GeneratedButtonWidgetStyle = InButtonStyle;
    bOverrideGeneratedButtonWidgetStyle = true;
    RefreshGeneratedButtonLayout();
}

void USelectionWidgetBase::SetGeneratedButtonColors(
    const FLinearColor& InColorAndOpacity,
    const FLinearColor& InBackgroundColor)
{
    GeneratedButtonColorAndOpacity = InColorAndOpacity;
    GeneratedButtonBackgroundColor = InBackgroundColor;
    RefreshGeneratedButtonLayout();
}

void USelectionWidgetBase::SetGeneratedButtonTextColor(const FSlateColor& InTextColor)
{
    GeneratedButtonTextColor = InTextColor;
    RefreshGeneratedButtonLayout();
}

void USelectionWidgetBase::SetGeneratedButtonFontSize(int32 InFontSize)
{
    GeneratedButtonFontSize = FMath::Max(1, InFontSize);
    RefreshGeneratedButtonLayout();
}

void USelectionWidgetBase::SetGeneratedButtonTypeface(FName InTypefaceFontName)
{
    GeneratedButtonTypefaceFontName = InTypefaceFontName;
    RefreshGeneratedButtonLayout();
}

void USelectionWidgetBase::SetGeneratedButtonOutlineSettings(
    const FFontOutlineSettings& InOutlineSettings)
{
    GeneratedButtonOutlineSettings = InOutlineSettings;
    RefreshGeneratedButtonLayout();
}

void USelectionWidgetBase::SetGeneratedButtonTextShadow(
    const FVector2D& InShadowOffset,
    const FLinearColor& InShadowColor)
{
    GeneratedButtonTextShadowOffset = InShadowOffset;
    GeneratedButtonTextShadowColor = InShadowColor;
    RefreshGeneratedButtonLayout();
}

void USelectionWidgetBase::RefreshGeneratedButtonLayout()
{
    for (USizeBox* SizeBox : GeneratedSelectionSizeBoxes)
    {
        ApplyGeneratedEntrySize(SizeBox);
    }
    for (UWidget* EntryWidget : GeneratedSelectionEntries)
    {
        ApplyGeneratedEntrySlotLayout(EntryWidget);
    }
    for (UButton* Button : GeneratedSelectionButtons)
    {
        ApplyGeneratedButtonStyle(Button);
        ApplyGeneratedButtonContentLayout(Button);
    }
    for (UTextBlock* Label : GeneratedSelectionLabels)
    {
        ApplyGeneratedLabelLayout(Label);
    }
}

void USelectionWidgetBase::ApplyGeneratedEntrySize(USizeBox* EntrySizeBox) const
{
    if (!IsValid(EntrySizeBox))
    {
        return;
    }

    if (GeneratedButtonSize.X > 0.0f) EntrySizeBox->SetWidthOverride(GeneratedButtonSize.X);
    else EntrySizeBox->ClearWidthOverride();

    if (GeneratedButtonSize.Y > 0.0f) EntrySizeBox->SetHeightOverride(GeneratedButtonSize.Y);
    else EntrySizeBox->ClearHeightOverride();
}

void USelectionWidgetBase::ApplyGeneratedEntrySlotLayout(UWidget* EntryWidget) const
{
    if (!IsValid(EntryWidget) || !EntryWidget->Slot)
    {
        return;
    }

    UPanelSlot* const ParentSlot = EntryWidget->Slot;
    const EHorizontalAlignment Horizontal = GeneratedButtonPanelHorizontalAlignment.GetValue();
    const EVerticalAlignment Vertical = GeneratedButtonPanelVerticalAlignment.GetValue();

    if (UScrollBoxSlot* ScrollSlot = Cast<UScrollBoxSlot>(ParentSlot))
    {
        ScrollSlot->SetPadding(GeneratedButtonEntryPadding);
        ScrollSlot->SetHorizontalAlignment(Horizontal);
        ScrollSlot->SetVerticalAlignment(Vertical);
    }
    else if (UVerticalBoxSlot* VerticalSlot = Cast<UVerticalBoxSlot>(ParentSlot))
    {
        VerticalSlot->SetPadding(GeneratedButtonEntryPadding);
        VerticalSlot->SetHorizontalAlignment(Horizontal);
        VerticalSlot->SetVerticalAlignment(Vertical);
    }
    else if (UHorizontalBoxSlot* HorizontalSlot = Cast<UHorizontalBoxSlot>(ParentSlot))
    {
        HorizontalSlot->SetPadding(GeneratedButtonEntryPadding);
        HorizontalSlot->SetHorizontalAlignment(Horizontal);
        HorizontalSlot->SetVerticalAlignment(Vertical);
    }
    else if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(ParentSlot))
    {
        OverlaySlot->SetPadding(GeneratedButtonEntryPadding);
        OverlaySlot->SetHorizontalAlignment(Horizontal);
        OverlaySlot->SetVerticalAlignment(Vertical);
    }
    else if (UGridSlot* GridPanelSlot = Cast<UGridSlot>(ParentSlot))
    {
        GridPanelSlot->SetPadding(GeneratedButtonEntryPadding);
        GridPanelSlot->SetHorizontalAlignment(Horizontal);
        GridPanelSlot->SetVerticalAlignment(Vertical);
    }
    else if (UUniformGridSlot* UniformSlot = Cast<UUniformGridSlot>(ParentSlot))
    {
        UniformSlot->SetHorizontalAlignment(Horizontal);
        UniformSlot->SetVerticalAlignment(Vertical);
    }
    else if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(ParentSlot))
    {
        if (GeneratedButtonSize.X > 0.0f && GeneratedButtonSize.Y > 0.0f)
        {
            CanvasSlot->SetSize(GeneratedButtonSize);
        }
        CanvasSlot->SetAlignment(MakeSelectionCanvasAlignment(Horizontal, Vertical));
    }
}

void USelectionWidgetBase::ApplyGeneratedButtonStyle(UButton* Button) const
{
    if (!IsValid(Button)) return;
    if (bOverrideGeneratedButtonWidgetStyle)
    {
        Button->SetStyle(GeneratedButtonWidgetStyle);
    }
    Button->SetColorAndOpacity(GeneratedButtonColorAndOpacity);
    Button->SetBackgroundColor(GeneratedButtonBackgroundColor);
}

void USelectionWidgetBase::ApplyGeneratedButtonContentLayout(UButton* Button) const
{
    if (!IsValid(Button)) return;
    if (UButtonSlot* const ContentSlot = Cast<UButtonSlot>(Button->GetContentSlot()))
    {
        ContentSlot->SetPadding(GeneratedButtonTextPadding);
        ContentSlot->SetHorizontalAlignment(GeneratedButtonContentHorizontalAlignment.GetValue());
        ContentSlot->SetVerticalAlignment(GeneratedButtonContentVerticalAlignment.GetValue());
    }
}

void USelectionWidgetBase::ApplyGeneratedLabelLayout(UTextBlock* Label) const
{
    if (!IsValid(Label)) return;

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
