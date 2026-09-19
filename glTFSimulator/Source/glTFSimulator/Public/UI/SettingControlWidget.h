// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file SettingControlWidget.h
 * Reusable Blueprint-authored settings row widgets selected by setting value type.
 */

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/SettingsFieldTypes.h"
#include "SettingControlWidget.generated.h"

class UButton;
class UComboBoxString;
class USlider;
class UTextBlock;
class USettingsMenuWidget;

/**
 * Base contract for a generated setting row WBP.
 *
 * Visual layout is owned entirely by Blueprint. Native code only supplies the field metadata,
 * current pending value, and forwards user interaction back to USettingsMenuWidget.
 */
UCLASS(Abstract, Blueprintable, BlueprintType)
class GLTFSIMULATOR_API USettingControlWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category="Settings Control")
    virtual void ConfigureSetting(USettingsMenuWidget* InOwner, ESettingsField InField);

    UFUNCTION(BlueprintCallable, Category="Settings Control")
    virtual void RefreshSettingWidget();

    UFUNCTION(BlueprintPure, Category="Settings Control")
    ESettingsField GetSettingField() const { return SettingField; }

    UFUNCTION(BlueprintPure, Category="Settings Control")
    FText GetSettingLabel() const;

    UFUNCTION(BlueprintPure, Category="Settings Control")
    FText GetSettingValueText() const;

protected:
    UPROPERTY(BlueprintReadOnly, Transient, Category="Settings Control")
    TObjectPtr<USettingsMenuWidget> OwnerMenu;

    UPROPERTY(BlueprintReadOnly, Transient, Category="Settings Control")
    ESettingsField SettingField = ESettingsField::BloomIntensity;
};

/** Boolean setting row. The WBP usually contains a styled UButton or custom toggle surface. */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UBooleanSettingWidget : public USettingControlWidget
{
    GENERATED_BODY()

public:
    virtual void NativeDestruct() override;
    virtual void ConfigureSetting(USettingsMenuWidget* InOwner, ESettingsField InField) override;
    virtual void RefreshSettingWidget() override;

    UFUNCTION(BlueprintCallable, Category="Settings Control|Boolean")
    void SetToggleButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Settings Control|Boolean")
    void SetLabelTextBlock(UTextBlock* InTextBlock);

    UFUNCTION(BlueprintCallable, Category="Settings Control|Boolean")
    void SetValueTextBlock(UTextBlock* InTextBlock);

    /** Allows a fully custom WBP interaction to toggle without assigning a native UButton. */
    UFUNCTION(BlueprintCallable, Category="Settings Control|Boolean")
    void ToggleValue();

    UFUNCTION(BlueprintPure, Category="Settings Control|Boolean")
    bool GetBooleanValue() const;

    UFUNCTION(BlueprintImplementableEvent, Category="Settings Control|Boolean", meta=(DisplayName="On Boolean Setting Updated"))
    void BP_OnBooleanSettingUpdated(const FText& Label, bool bValue, const FText& ValueText);

private:
    UFUNCTION()
    void HandleToggleClicked();

    TWeakObjectPtr<UButton> AssignedToggleButton;
    TWeakObjectPtr<UTextBlock> AssignedLabelText;
    TWeakObjectPtr<UTextBlock> AssignedValueText;
};

/** Numeric ranged setting row. The WBP normally contains a styled Slider/progress-bar-like control. */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UFloatSettingWidget : public USettingControlWidget
{
    GENERATED_BODY()

public:
    virtual void NativeDestruct() override;
    virtual void ConfigureSetting(USettingsMenuWidget* InOwner, ESettingsField InField) override;
    virtual void RefreshSettingWidget() override;

    UFUNCTION(BlueprintCallable, Category="Settings Control|Float")
    void SetSlider(USlider* InSlider);

    UFUNCTION(BlueprintCallable, Category="Settings Control|Float")
    void SetLabelTextBlock(UTextBlock* InTextBlock);

    UFUNCTION(BlueprintCallable, Category="Settings Control|Float")
    void SetValueTextBlock(UTextBlock* InTextBlock);

    /** Allows a custom Blueprint bar/control to push a numeric value directly. */
    UFUNCTION(BlueprintCallable, Category="Settings Control|Float")
    void SetNumericValue(float Value);

    UFUNCTION(BlueprintPure, Category="Settings Control|Float")
    float GetNumericValue() const;

    UFUNCTION(BlueprintPure, Category="Settings Control|Float")
    bool GetNumericRange(float& OutMin, float& OutMax, float& OutStep) const;

    UFUNCTION(BlueprintImplementableEvent, Category="Settings Control|Float", meta=(DisplayName="On Float Setting Updated"))
    void BP_OnFloatSettingUpdated(const FText& Label, float Value, float MinValue, float MaxValue, float Step, const FText& ValueText);

private:
    UFUNCTION()
    void HandleSliderValueChanged(float Value);

    TWeakObjectPtr<USlider> AssignedSlider;
    TWeakObjectPtr<UTextBlock> AssignedLabelText;
    TWeakObjectPtr<UTextBlock> AssignedValueText;
    bool bInternalRefresh = false;
};

/** Discrete option setting row. Int-backed qualities and enum-like fields use this WBP. */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UEnumSettingWidget : public USettingControlWidget
{
    GENERATED_BODY()

public:
    virtual void NativeDestruct() override;
    virtual void ConfigureSetting(USettingsMenuWidget* InOwner, ESettingsField InField) override;
    virtual void RefreshSettingWidget() override;

    UFUNCTION(BlueprintCallable, Category="Settings Control|Enum")
    void SetDropdown(UComboBoxString* InDropdown);

    UFUNCTION(BlueprintCallable, Category="Settings Control|Enum")
    void SetLabelTextBlock(UTextBlock* InTextBlock);

    UFUNCTION(BlueprintCallable, Category="Settings Control|Enum")
    void SetValueTextBlock(UTextBlock* InTextBlock);

    /** Allows a custom dropdown WBP to submit an option string without assigning ComboBoxString. */
    UFUNCTION(BlueprintCallable, Category="Settings Control|Enum")
    void SelectOption(const FString& SelectedOption);

    UFUNCTION(BlueprintPure, Category="Settings Control|Enum")
    TArray<FText> GetOptions() const;

    UFUNCTION(BlueprintImplementableEvent, Category="Settings Control|Enum", meta=(DisplayName="On Enum Setting Updated"))
    void BP_OnEnumSettingUpdated(const FText& Label, const TArray<FText>& Options, const FText& SelectedValue);

private:
    UFUNCTION()
    void HandleSelectionChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

    void RebuildDropdownOptions();

    TWeakObjectPtr<UComboBoxString> AssignedDropdown;
    TWeakObjectPtr<UTextBlock> AssignedLabelText;
    TWeakObjectPtr<UTextBlock> AssignedValueText;
    bool bInternalRefresh = false;
};
