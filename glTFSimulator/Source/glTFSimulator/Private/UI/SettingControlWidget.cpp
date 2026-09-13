// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/SettingControlWidget.h"
#include "UI/SettingsMenuWidget.h"

#include "Components/Button.h"
#include "Components/ComboBoxString.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"

void USettingControlWidget::ConfigureSetting(USettingsMenuWidget* InOwner, ESettingsField InField)
{
    OwnerMenu = InOwner;
    SettingField = InField;
    RefreshSettingWidget();
}

void USettingControlWidget::RefreshSettingWidget()
{
}

FText USettingControlWidget::GetSettingLabel() const
{
    return IsValid(OwnerMenu) ? OwnerMenu->GetSettingLabelText(SettingField) : FText::GetEmpty();
}

FText USettingControlWidget::GetSettingValueText() const
{
    return IsValid(OwnerMenu) ? OwnerMenu->GetPendingSettingValueText(SettingField) : FText::GetEmpty();
}

void UBooleanSettingWidget::NativeDestruct()
{
    if (UButton* Button = AssignedToggleButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UBooleanSettingWidget::HandleToggleClicked);
    }
    AssignedToggleButton.Reset();
    AssignedLabelText.Reset();
    AssignedValueText.Reset();
    Super::NativeDestruct();
}

void UBooleanSettingWidget::ConfigureSetting(USettingsMenuWidget* InOwner, ESettingsField InField)
{
    Super::ConfigureSetting(InOwner, InField);
    if (UButton* Button = AssignedToggleButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UBooleanSettingWidget::HandleToggleClicked);
        Button->OnClicked.AddDynamic(this, &UBooleanSettingWidget::HandleToggleClicked);
    }
    RefreshSettingWidget();
}

void UBooleanSettingWidget::RefreshSettingWidget()
{
    if (!IsValid(OwnerMenu)) return;

    const FText Label = OwnerMenu->GetSettingLabelText(SettingField);
    const FText ValueText = OwnerMenu->GetPendingSettingValueText(SettingField);
    const bool bValue = OwnerMenu->GetPendingBooleanSettingValue(SettingField);

    if (UTextBlock* Text = AssignedLabelText.Get()) Text->SetText(Label);
    if (UTextBlock* Text = AssignedValueText.Get()) Text->SetText(ValueText);
    BP_OnBooleanSettingUpdated(Label, bValue, ValueText);
}

void UBooleanSettingWidget::SetToggleButton(UButton* InButton)
{
    if (UButton* Existing = AssignedToggleButton.Get())
    {
        Existing->OnClicked.RemoveDynamic(this, &UBooleanSettingWidget::HandleToggleClicked);
    }
    AssignedToggleButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UBooleanSettingWidget::HandleToggleClicked);
        InButton->OnClicked.AddDynamic(this, &UBooleanSettingWidget::HandleToggleClicked);
    }
}

void UBooleanSettingWidget::SetLabelTextBlock(UTextBlock* InTextBlock)
{
    AssignedLabelText = InTextBlock;
    RefreshSettingWidget();
}

void UBooleanSettingWidget::SetValueTextBlock(UTextBlock* InTextBlock)
{
    AssignedValueText = InTextBlock;
    RefreshSettingWidget();
}

void UBooleanSettingWidget::ToggleValue()
{
    if (IsValid(OwnerMenu))
    {
        OwnerMenu->ToggleSettingFromUI(SettingField);
    }
}

bool UBooleanSettingWidget::GetBooleanValue() const
{
    return IsValid(OwnerMenu) && OwnerMenu->GetPendingBooleanSettingValue(SettingField);
}

void UBooleanSettingWidget::HandleToggleClicked()
{
    ToggleValue();
}

void UFloatSettingWidget::NativeDestruct()
{
    if (USlider* Slider = AssignedSlider.Get())
    {
        Slider->OnValueChanged.RemoveDynamic(this, &UFloatSettingWidget::HandleSliderValueChanged);
    }
    AssignedSlider.Reset();
    AssignedLabelText.Reset();
    AssignedValueText.Reset();
    Super::NativeDestruct();
}

void UFloatSettingWidget::ConfigureSetting(USettingsMenuWidget* InOwner, ESettingsField InField)
{
    Super::ConfigureSetting(InOwner, InField);
    if (USlider* Slider = AssignedSlider.Get())
    {
        Slider->OnValueChanged.RemoveDynamic(this, &UFloatSettingWidget::HandleSliderValueChanged);
        Slider->OnValueChanged.AddDynamic(this, &UFloatSettingWidget::HandleSliderValueChanged);
    }
    RefreshSettingWidget();
}

void UFloatSettingWidget::RefreshSettingWidget()
{
    if (!IsValid(OwnerMenu)) return;

    float MinValue = 0.0f;
    float MaxValue = 1.0f;
    float Step = 0.01f;
    OwnerMenu->GetSettingSliderRange(SettingField, MinValue, MaxValue, Step);
    const float Value = OwnerMenu->GetPendingNumericSettingValue(SettingField);
    const FText Label = OwnerMenu->GetSettingLabelText(SettingField);
    const FText ValueText = OwnerMenu->GetPendingSettingValueText(SettingField);

    bInternalRefresh = true;
    if (USlider* Slider = AssignedSlider.Get())
    {
        Slider->SetMinValue(MinValue);
        Slider->SetMaxValue(MaxValue);
        Slider->SetStepSize(Step);
        Slider->SetValue(Value);
    }
    bInternalRefresh = false;

    if (UTextBlock* Text = AssignedLabelText.Get()) Text->SetText(Label);
    if (UTextBlock* Text = AssignedValueText.Get()) Text->SetText(ValueText);
    BP_OnFloatSettingUpdated(Label, Value, MinValue, MaxValue, Step, ValueText);
}

void UFloatSettingWidget::SetSlider(USlider* InSlider)
{
    if (USlider* Existing = AssignedSlider.Get())
    {
        Existing->OnValueChanged.RemoveDynamic(this, &UFloatSettingWidget::HandleSliderValueChanged);
    }
    AssignedSlider = InSlider;
    if (IsValid(InSlider))
    {
        InSlider->OnValueChanged.RemoveDynamic(this, &UFloatSettingWidget::HandleSliderValueChanged);
        InSlider->OnValueChanged.AddDynamic(this, &UFloatSettingWidget::HandleSliderValueChanged);
    }
    RefreshSettingWidget();
}

void UFloatSettingWidget::SetLabelTextBlock(UTextBlock* InTextBlock)
{
    AssignedLabelText = InTextBlock;
    RefreshSettingWidget();
}

void UFloatSettingWidget::SetValueTextBlock(UTextBlock* InTextBlock)
{
    AssignedValueText = InTextBlock;
    RefreshSettingWidget();
}

void UFloatSettingWidget::SetNumericValue(float Value)
{
    if (IsValid(OwnerMenu))
    {
        OwnerMenu->SetSettingFromSliderValue(SettingField, Value);
    }
}

float UFloatSettingWidget::GetNumericValue() const
{
    return IsValid(OwnerMenu) ? OwnerMenu->GetPendingNumericSettingValue(SettingField) : 0.0f;
}

bool UFloatSettingWidget::GetNumericRange(float& OutMin, float& OutMax, float& OutStep) const
{
    return IsValid(OwnerMenu) && OwnerMenu->GetSettingSliderRange(SettingField, OutMin, OutMax, OutStep);
}

void UFloatSettingWidget::HandleSliderValueChanged(float Value)
{
    if (!bInternalRefresh)
    {
        SetNumericValue(Value);
    }
}

void UEnumSettingWidget::NativeDestruct()
{
    if (UComboBoxString* Dropdown = AssignedDropdown.Get())
    {
        Dropdown->OnSelectionChanged.RemoveDynamic(this, &UEnumSettingWidget::HandleSelectionChanged);
    }
    AssignedDropdown.Reset();
    AssignedLabelText.Reset();
    AssignedValueText.Reset();
    Super::NativeDestruct();
}

void UEnumSettingWidget::ConfigureSetting(USettingsMenuWidget* InOwner, ESettingsField InField)
{
    Super::ConfigureSetting(InOwner, InField);
    if (UComboBoxString* Dropdown = AssignedDropdown.Get())
    {
        Dropdown->OnSelectionChanged.RemoveDynamic(this, &UEnumSettingWidget::HandleSelectionChanged);
        Dropdown->OnSelectionChanged.AddDynamic(this, &UEnumSettingWidget::HandleSelectionChanged);
    }
    RebuildDropdownOptions();
    RefreshSettingWidget();
}

void UEnumSettingWidget::RefreshSettingWidget()
{
    if (!IsValid(OwnerMenu)) return;

    const FText Label = OwnerMenu->GetSettingLabelText(SettingField);
    const FText ValueText = OwnerMenu->GetPendingSettingValueText(SettingField);
    const TArray<FText> Options = OwnerMenu->GetSettingOptionTexts(SettingField);

    bInternalRefresh = true;
    if (UComboBoxString* Dropdown = AssignedDropdown.Get())
    {
        const FString Selected = ValueText.ToString();
        if (Dropdown->FindOptionIndex(Selected) == INDEX_NONE)
        {
            RebuildDropdownOptions();
            bInternalRefresh = true;
        }
        Dropdown->SetSelectedOption(Selected);
    }
    bInternalRefresh = false;

    if (UTextBlock* Text = AssignedLabelText.Get()) Text->SetText(Label);
    if (UTextBlock* Text = AssignedValueText.Get()) Text->SetText(ValueText);
    BP_OnEnumSettingUpdated(Label, Options, ValueText);
}

void UEnumSettingWidget::SetDropdown(UComboBoxString* InDropdown)
{
    if (UComboBoxString* Existing = AssignedDropdown.Get())
    {
        Existing->OnSelectionChanged.RemoveDynamic(this, &UEnumSettingWidget::HandleSelectionChanged);
    }
    AssignedDropdown = InDropdown;
    if (IsValid(InDropdown))
    {
        InDropdown->OnSelectionChanged.RemoveDynamic(this, &UEnumSettingWidget::HandleSelectionChanged);
        InDropdown->OnSelectionChanged.AddDynamic(this, &UEnumSettingWidget::HandleSelectionChanged);
    }
    RebuildDropdownOptions();
    RefreshSettingWidget();
}

void UEnumSettingWidget::SetLabelTextBlock(UTextBlock* InTextBlock)
{
    AssignedLabelText = InTextBlock;
    RefreshSettingWidget();
}

void UEnumSettingWidget::SetValueTextBlock(UTextBlock* InTextBlock)
{
    AssignedValueText = InTextBlock;
    RefreshSettingWidget();
}

void UEnumSettingWidget::SelectOption(const FString& SelectedOption)
{
    if (IsValid(OwnerMenu))
    {
        OwnerMenu->SetSettingFromDropdownSelection(SettingField, SelectedOption);
    }
}

TArray<FText> UEnumSettingWidget::GetOptions() const
{
    return IsValid(OwnerMenu) ? OwnerMenu->GetSettingOptionTexts(SettingField) : TArray<FText>();
}

void UEnumSettingWidget::HandleSelectionChanged(FString SelectedItem, ESelectInfo::Type /*SelectionType*/)
{
    if (!bInternalRefresh)
    {
        SelectOption(SelectedItem);
    }
}

void UEnumSettingWidget::RebuildDropdownOptions()
{
    UComboBoxString* Dropdown = AssignedDropdown.Get();
    if (!IsValid(Dropdown) || !IsValid(OwnerMenu)) return;

    bInternalRefresh = true;
    Dropdown->ClearOptions();
    for (const FText& Option : OwnerMenu->GetSettingOptionTexts(SettingField))
    {
        Dropdown->AddOption(Option.ToString());
    }
    bInternalRefresh = false;
}
