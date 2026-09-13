// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file SettingsMenuWidget.h
 * 역할: 설정 메뉴의 보류값과 적용 동작을 관리합니다.
 * 핵심 기능: 설정 컨트롤 바인딩, 값 변경·확인·적용.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/ComboBoxString.h"
#include "Components/Slider.h"
#include "SettingsMenuWidget.generated.h"

class UGameSettings;
class USettingsMenuWidget;
class UTextBlock;
class USettingsControlBinding;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSettingsCloseRequested);

UENUM(BlueprintType)
enum class ESettingsField : uint8
{
    BloomIntensity UMETA(DisplayName="Bloom Intensity"),
    BloomThreshold UMETA(DisplayName="Bloom Threshold"),
    AmbientOcclusionIntensity UMETA(DisplayName="Ambient Occlusion"),
    RayTracing UMETA(DisplayName="Ray Tracing"),
    HeightFog UMETA(DisplayName="Height Fog"),
    Cloud UMETA(DisplayName="Cloud"),
    ShadowQuality UMETA(DisplayName="Shadow Quality"),
    TextureQuality UMETA(DisplayName="Texture Quality"),
    MaxTextureResolution UMETA(DisplayName="Max Texture Resolution"),
    ViewDistanceQuality UMETA(DisplayName="View Distance Quality"),
    StreamingDistanceMultiplier UMETA(DisplayName="Streaming Distance Multiplier"),
    StreamingUnloadDistanceMultiplier UMETA(DisplayName="Streaming Unload Multiplier"),
    ObjectStreamingRadiusMeters UMETA(DisplayName="Object Streaming Radius"),
    StreamingSceneSpawnBudget UMETA(DisplayName="Scene Spawn Budget"),
    StreamingNodeBudgetPerFrame UMETA(DisplayName="Node Budget Per Frame"),
    AntiAliasingQuality UMETA(DisplayName="Anti Aliasing Quality"),
    PostProcessingQuality UMETA(DisplayName="Post Processing Quality"),
    EffectsQuality UMETA(DisplayName="Effects Quality"),
    FoliageQuality UMETA(DisplayName="Foliage Quality"),
    ShadingQuality UMETA(DisplayName="Shading Quality"),
    GlobalIlluminationQuality UMETA(DisplayName="GI Quality"),
    ReflectionQuality UMETA(DisplayName="Reflection Quality"),
    DynamicGlobalIlluminationMethod UMETA(DisplayName="GI Method"),
    ReflectionMethod UMETA(DisplayName="Reflection Method")
};

UENUM(BlueprintType)
enum class ESettingsControlType : uint8
{
    Slider UMETA(DisplayName="Slider"),
    Dropdown UMETA(DisplayName="Dropdown"),
    Toggle UMETA(DisplayName="Toggle Button")
};

/**
 * Per-control delegate adapter. A Blueprint can keep using ordinary Slider/ComboBoxString/Button
 * widgets; registering the widget creates one adapter that remembers which settings field it owns.
 */
UCLASS(Transient)
class GLTFSIMULATOR_API USettingsControlBinding : public UObject
{
    GENERATED_BODY()

public:
    void BindSlider(USettingsMenuWidget* InOwner, ESettingsField InField, USlider* InSlider);
    void BindDropdown(USettingsMenuWidget* InOwner, ESettingsField InField, UComboBoxString* InDropdown);
    void BindToggle(USettingsMenuWidget* InOwner, ESettingsField InField, UButton* InButton);
    void Unbind();

    UFUNCTION()
    void HandleSliderValueChanged(float Value);

    UFUNCTION()
    void HandleDropdownSelectionChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

    UFUNCTION()
    void HandleToggleClicked();

    ESettingsField GetField() const { return Field; }
    UObject* GetControlObject() const;

private:
    UPROPERTY(Transient)
    TObjectPtr<USettingsMenuWidget> OwnerWidget;

    UPROPERTY(Transient)
    TObjectPtr<USlider> Slider;

    UPROPERTY(Transient)
    TObjectPtr<UComboBoxString> Dropdown;

    UPROPERTY(Transient)
    TObjectPtr<UButton> ToggleButton;

    ESettingsField Field = ESettingsField::BloomIntensity;
};

/** Backward-compatible helper button. Its click changes only the pending value; Apply/Confirm commits it. */
UCLASS(Blueprintable)
class GLTFSIMULATOR_API USettingsAdjustmentButton : public UButton
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category="Settings")
    void SetupAdjustment(USettingsMenuWidget* InOwner, ESettingsField InField, float InStep);

    UFUNCTION()
    void HandleClicked();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Settings")
    ESettingsField Field = ESettingsField::BloomIntensity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Settings")
    float Step = 1.0f;

private:
    UPROPERTY()
    TObjectPtr<USettingsMenuWidget> OwnerWidget;
};

/** Button class that cycles one settings field when clicked. Use it in WBP setting rows when desired. */
UCLASS(Blueprintable)
class GLTFSIMULATOR_API USettingsCycleButton : public UButton
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category="Settings")
    void SetupCycleButton(USettingsMenuWidget* InOwner, ESettingsField InField, int32 InDirection = 1);

    UFUNCTION(BlueprintCallable, Category="Settings")
    void RefreshDisplayedText();

    UFUNCTION()
    void HandleClicked();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Settings")
    ESettingsField Field = ESettingsField::BloomIntensity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Settings")
    int32 Direction = 1;

private:
    UPROPERTY()
    TObjectPtr<USettingsMenuWidget> OwnerWidget;
};

/**
 * Blueprint-editable settings menu backed by UGameSettings.
 *
 * WBP children should pass buttons and value text blocks explicitly from Construct by calling
 * the Set*Button(), SetTitleText(), RegisterSettingValueText(), and RegisterSettingButton()
 * functions. This class does not use automatic widget-name binding or widget-tree lookup.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API USettingsMenuWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    /** Shared close contract used by both MainGameMode and the gameplay pause menu. */
    UPROPERTY(BlueprintAssignable, Category="Settings")
    FSettingsCloseRequested OnCloseRequested;

    UFUNCTION(BlueprintCallable, Category="Settings|Widgets")
    void SetTitleText(UTextBlock* InTitleText);

    UFUNCTION(BlueprintCallable, Category="Settings|Widgets")
    void SetApplyButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Settings|Widgets")
    void SetConfirmButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Settings|Widgets")
    void SetBackButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Settings|Widgets")
    void SetCancelButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Settings|Widgets")
    void RegisterSettingValueText(ESettingsField Field, UTextBlock* InTextBlock);

    UFUNCTION(BlueprintCallable, Category="Settings|Widgets")
    void RegisterSettingButton(ESettingsField Field, UButton* InButton);

    /** Preferred range binding: ordinary Blueprint Slider, configured and wired automatically. */
    UFUNCTION(BlueprintCallable, Category="Settings|Widgets")
    void RegisterSettingSlider(ESettingsField Field, USlider* InSlider);

    /** Preferred discrete-choice binding: ordinary Blueprint ComboBoxString, populated automatically. */
    UFUNCTION(BlueprintCallable, Category="Settings|Widgets")
    void RegisterSettingDropdown(ESettingsField Field, UComboBoxString* InDropdown);

    /** Preferred boolean binding: ordinary Blueprint Button; each click toggles the pending bool. */
    UFUNCTION(BlueprintCallable, Category="Settings|Widgets")
    void RegisterSettingToggleButton(ESettingsField Field, UButton* InButton);

    /** Removes any Slider/Dropdown/Toggle adapter currently registered for Field. */
    UFUNCTION(BlueprintCallable, Category="Settings|Widgets")
    void UnregisterSettingControl(ESettingsField Field);

    /** Returns which Blueprint control should normally represent this field. */
    UFUNCTION(BlueprintPure, Category="Settings|Controls")
    ESettingsControlType GetSettingControlType(ESettingsField Field) const;

    /** Returns the actual Slider min/max/step for range settings. */
    UFUNCTION(BlueprintPure, Category="Settings|Controls")
    bool GetSettingSliderRange(ESettingsField Field, float& OutMin, float& OutMax, float& OutStep) const;

    /** Direct graph target for a Slider OnValueChanged event. */
    UFUNCTION(BlueprintCallable, Category="Settings|Controls")
    void SetSettingFromSliderValue(ESettingsField Field, float Value);

    /** Direct graph target for a ComboBoxString OnSelectionChanged event. */
    UFUNCTION(BlueprintCallable, Category="Settings|Controls")
    void SetSettingFromDropdownSelection(ESettingsField Field, const FString& SelectedOption);

    /** Direct graph target for a boolean Button OnClicked event. */
    UFUNCTION(BlueprintCallable, Category="Settings|Controls")
    void ToggleSettingFromUI(ESettingsField Field);

    /** Returns the active settings object owned by the GameManager subsystem. */
    UFUNCTION(BlueprintCallable, Category="Settings")
    UGameSettings* GetEditableSettings() const;

    /** Loads the current saved/runtime values into the pending UI copy and refreshes button text. */
    UFUNCTION(BlueprintCallable, Category="Settings")
    void InitializeSettingsFromSavedData();

    /** Refreshes all known value labels and setting buttons from the pending UI copy. */
    UFUNCTION(BlueprintCallable, Category="Settings")
    void RefreshSettingsValues();

    /** Cycles a setting to its next/previous option in the pending UI copy only. */
    UFUNCTION(BlueprintCallable, Category="Settings")
    void CycleSettingValueFromUI(ESettingsField Field, int32 Direction = 1);

    /** Cycles a setting by enum/display name. Useful when a Blueprint stores setting names as text/name data. */
    UFUNCTION(BlueprintCallable, Category="Settings")
    void CycleSettingByNameFromUI(FName FieldName, int32 Direction = 1);

    /** Reads the button text, finds the matching setting label, and cycles that pending setting. */
    UFUNCTION(BlueprintCallable, Category="Settings|Buttons")
    void CycleSettingByButtonTextFromUI(UButton* SourceButton, int32 Direction = 1);

    /** Backward-compatible adjustment entry point. Positive values cycle forward; negative values cycle backward. */
    UFUNCTION(BlueprintCallable, Category="Settings")
    void AdjustSettingFromUI(ESettingsField Field, float Step);

    /** Applies the pending UI copy to UGameSettings, updates runtime systems, and saves settings.json. */
    UFUNCTION(BlueprintCallable, Category="Settings")
    void ApplyAndSaveSettingsFromUI();

    /** Alias for ApplyAndSaveSettingsFromUI(), intended for a Confirm/OK button. */
    UFUNCTION(BlueprintCallable, Category="Settings")
    void ConfirmSettingsFromUI();

    /** Reloads current runtime/saved values and discards pending UI edits. */
    UFUNCTION(BlueprintCallable, Category="Settings")
    void DiscardPendingSettingsFromUI();

    /** Closes the settings screen without applying pending edits. */
    UFUNCTION(BlueprintCallable, Category="Settings")
    void CloseSettingsFromUI();

    /** Returns all setting fields represented by UGameSettings. */
    UFUNCTION(BlueprintPure, Category="Settings")
    TArray<ESettingsField> GetSettingFieldList() const;

    /** Returns the user-facing label for a setting field. */
    UFUNCTION(BlueprintPure, Category="Settings|Text")
    FText GetSettingLabelText(ESettingsField Field) const;

    /** Returns the current pending value text for a setting field. */
    UFUNCTION(BlueprintPure, Category="Settings|Text")
    FText GetPendingSettingValueText(ESettingsField Field) const;

    /** Returns "Label: Value" for a setting button. */
    UFUNCTION(BlueprintPure, Category="Settings|Text")
    FText GetSettingButtonText(ESettingsField Field) const;

    /** Returns every option label that the field cycles through. */
    UFUNCTION(BlueprintPure, Category="Settings|Text")
    TArray<FText> GetSettingOptionTexts(ESettingsField Field) const;

    /** Reads the first child text block inside a button. */
    UFUNCTION(BlueprintPure, Category="Settings|Buttons")
    FText GetButtonTextFromUI(UButton* SourceButton) const;

    /** Updates the first child text block inside a button. */
    UFUNCTION(BlueprintCallable, Category="Settings|Buttons")
    bool SetButtonTextFromUI(UButton* SourceButton, const FText& NewText) const;

    /** Tries to map a button label such as "Shadow Quality: High" back to a settings field. */
    UFUNCTION(BlueprintPure, Category="Settings|Buttons")
    bool TryGetSettingFieldFromButtonText(const FText& ButtonText, ESettingsField& OutField) const;

    // Direct functions for WBP button bindings. Each function cycles one pending setting forward.
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleBloomIntensityFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleBloomThresholdFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleAmbientOcclusionIntensityFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleRayTracingFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleHeightFogFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleCloudFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleShadowQualityFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleTextureQualityFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleMaxTextureResolutionFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleViewDistanceQualityFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleStreamingDistanceMultiplierFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleStreamingUnloadDistanceMultiplierFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleObjectStreamingRadiusMetersFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleStreamingSceneSpawnBudgetFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleStreamingNodeBudgetPerFrameFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleAntiAliasingQualityFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CyclePostProcessingQualityFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleEffectsQualityFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleFoliageQualityFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleShadingQualityFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleGlobalIlluminationQualityFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleReflectionQualityFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleDynamicGlobalIlluminationMethodFromUI();
    UFUNCTION(BlueprintCallable, Category="Settings|Cycle")
    void CycleReflectionMethodFromUI();

private:
    void CollectAssignedWidgetReferences();
    void BindButtonEvents();
    void UnbindButtonEvents();
    void BindAssignedSettingButtons();
    void RegisterValueTextBlock(ESettingsField Field, UTextBlock* TextBlock);
    void BindAssignedSettingButton(ESettingsField Field, UButton* Button);
    void BindFieldButton(ESettingsField Field, UButton* Button);
    void UnbindFieldButton(ESettingsField Field, UButton* Button);
    void RefreshRegisteredControls();
    void RemoveControlBinding(ESettingsField Field);
    USettingsControlBinding* FindControlBinding(ESettingsField Field) const;
    float GetPendingNumericValue(ESettingsField Field) const;
    void CopySettingsToPending(const UGameSettings* Settings);
    void ApplyPendingToSettings(UGameSettings* Settings) const;
    void CyclePendingValue(ESettingsField Field, int32 Direction);
    bool TryMatchFieldName(const FString& Input, ESettingsField& OutField) const;

    UButton* GetApplyButton() const { return AssignedApplyButton.Get(); }
    UButton* GetConfirmButton() const { return AssignedConfirmButton.Get(); }
    UButton* GetBackButton() const { return AssignedBackButton.Get(); }
    UButton* GetCancelButton() const { return AssignedCancelButton.Get(); }

    FText GetFieldValueTextFromPending(ESettingsField Field) const;
    FText GetQualityText(int32 Value) const;
    FText GetBoolText(bool bValue) const;
    FText GetDynamicGlobalIlluminationMethodText(int32 Value) const;
    FText GetReflectionMethodText(int32 Value) const;

    TWeakObjectPtr<UTextBlock> AssignedTitleText;
    TWeakObjectPtr<UButton> AssignedApplyButton;
    TWeakObjectPtr<UButton> AssignedConfirmButton;
    TWeakObjectPtr<UButton> AssignedBackButton;
    TWeakObjectPtr<UButton> AssignedCancelButton;

    TMap<ESettingsField, TWeakObjectPtr<UTextBlock>> RegisteredValueTextWidgets;
    TMap<ESettingsField, TWeakObjectPtr<UButton>> RegisteredSettingButtons;

    TArray<TWeakObjectPtr<UTextBlock>> ValueTextBlocks;
    TArray<ESettingsField> ValueFields;

    TMap<TWeakObjectPtr<UButton>, ESettingsField> BoundFieldButtons;

    UPROPERTY(Transient)
    TArray<TObjectPtr<USettingsControlBinding>> ControlBindings;

    float PendingBloomIntensity = 0.675f;
    float PendingBloomThreshold = -1.0f;
    float PendingAmbientOcclusionIntensity = 0.5f;
    bool bPendingRayTracing = true;
    bool bPendingHeightFog = true;
    bool bPendingCloud = true;
    int32 PendingShadowQuality = 2;
    int32 PendingTextureQuality = 2;
    int32 PendingMaxTextureResolution = 768;
    int32 PendingViewDistanceQuality = 2;
    float PendingStreamingDistanceMultiplier = 64.0f;
    float PendingStreamingUnloadDistanceMultiplier = 1.10f;
    float PendingObjectStreamingRadiusMeters = 2048.0f;
    int32 PendingStreamingSceneSpawnBudget = 2;
    int32 PendingStreamingNodeBudgetPerFrame = 32;
    int32 PendingAntiAliasingQuality = 2;
    int32 PendingPostProcessingQuality = 2;
    int32 PendingEffectsQuality = 2;
    int32 PendingFoliageQuality = 2;
    int32 PendingShadingQuality = 2;
    int32 PendingGlobalIlluminationQuality = 2;
    int32 PendingReflectionQuality = 2;
    int32 PendingDynamicGlobalIlluminationMethod = 1;
    int32 PendingReflectionMethod = 1;
};
