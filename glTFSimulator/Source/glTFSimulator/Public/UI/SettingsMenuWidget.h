// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "SettingsMenuWidget.generated.h"

class UGameSettings;
class USettingsMenuWidget;
class UTextBlock;

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
 * One click only changes this widget's pending value. Nothing is applied to the game, post process,
 * or JSON file until ApplyAndSaveSettingsFromUI() / ConfirmSettingsFromUI() is called.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API USettingsMenuWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    virtual void NativeConstruct() override;

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

protected:
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Widgets")
    TObjectPtr<UTextBlock> TitleText;

    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Widgets")
    TObjectPtr<UButton> ApplyButton;

    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Widgets")
    TObjectPtr<UButton> ConfirmButton;

    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Widgets")
    TObjectPtr<UButton> BackButton;

    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Widgets")
    TObjectPtr<UButton> CancelButton;

    /** Optional title text using the Settings_ prefix. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Widgets")
    TObjectPtr<UTextBlock> Settings_TitleText;

    /** Optional apply button using the Settings_ prefix. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Widgets")
    TObjectPtr<UButton> Settings_ApplyButton;

    /** Optional confirm button using the Settings_ prefix. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Widgets")
    TObjectPtr<UButton> Settings_ConfirmButton;

    /** Optional back button using the Settings_ prefix. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Widgets")
    TObjectPtr<UButton> Settings_BackButton;

    /** Optional cancel button using the Settings_ prefix. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Widgets")
    TObjectPtr<UButton> Settings_CancelButton;

    /** Optional value text for Bloom Intensity. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_BloomIntensityValue;

    /** Optional cycle button for Bloom Intensity. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_BloomIntensityButton;

    /** Optional value text for Bloom Threshold. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_BloomThresholdValue;

    /** Optional cycle button for Bloom Threshold. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_BloomThresholdButton;

    /** Optional value text for Ambient Occlusion. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_AmbientOcclusionIntensityValue;

    /** Optional cycle button for Ambient Occlusion. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_AmbientOcclusionIntensityButton;

    /** Optional value text for Ray Tracing. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_RayTracingValue;

    /** Optional cycle button for Ray Tracing. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_RayTracingButton;

    /** Optional value text for Height Fog. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_HeightFogValue;

    /** Optional cycle button for Height Fog. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_HeightFogButton;

    /** Optional value text for Cloud. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_CloudValue;

    /** Optional cycle button for Cloud. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_CloudButton;

    /** Optional value text for Shadow Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_ShadowQualityValue;

    /** Optional cycle button for Shadow Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_ShadowQualityButton;

    /** Optional value text for Texture Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_TextureQualityValue;

    /** Optional cycle button for Texture Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_TextureQualityButton;

    /** Optional value text for Max Texture Resolution. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_MaxTextureResolutionValue;

    /** Optional cycle button for Max Texture Resolution. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_MaxTextureResolutionButton;

    /** Optional value text for View Distance Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_ViewDistanceQualityValue;

    /** Optional cycle button for View Distance Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_ViewDistanceQualityButton;

    /** Optional value text for Anti Aliasing Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_AntiAliasingQualityValue;

    /** Optional cycle button for Anti Aliasing Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_AntiAliasingQualityButton;

    /** Optional value text for Post Processing Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_PostProcessingQualityValue;

    /** Optional cycle button for Post Processing Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_PostProcessingQualityButton;

    /** Optional value text for Effects Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_EffectsQualityValue;

    /** Optional cycle button for Effects Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_EffectsQualityButton;

    /** Optional value text for Foliage Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_FoliageQualityValue;

    /** Optional cycle button for Foliage Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_FoliageQualityButton;

    /** Optional value text for Shading Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_ShadingQualityValue;

    /** Optional cycle button for Shading Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_ShadingQualityButton;

    /** Optional value text for Global Illumination Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_GlobalIlluminationQualityValue;

    /** Optional cycle button for Global Illumination Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_GlobalIlluminationQualityButton;

    /** Optional value text for Reflection Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_ReflectionQualityValue;

    /** Optional cycle button for Reflection Quality. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_ReflectionQualityButton;

    /** Optional value text for Dynamic Global Illumination Method. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_DynamicGlobalIlluminationMethodValue;

    /** Optional cycle button for Dynamic Global Illumination Method. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_DynamicGlobalIlluminationMethodButton;

    /** Optional value text for Reflection Method. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Value Widgets")
    TObjectPtr<UTextBlock> Settings_ReflectionMethodValue;

    /** Optional cycle button for Reflection Method. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Settings|Button Widgets")
    TObjectPtr<UButton> Settings_ReflectionMethodButton;

private:
    void CollectAssignedWidgetReferences();
    void BindButtonEvents();
    void BindAssignedSettingButtons();
    void RegisterValueTextBlock(ESettingsField Field, UTextBlock* TextBlock);
    void BindAssignedSettingButton(ESettingsField Field, UButton* Button);
    void BindFieldButton(ESettingsField Field, UButton* Button);
    void CopySettingsToPending(const UGameSettings* Settings);
    void ApplyPendingToSettings(UGameSettings* Settings) const;
    void CyclePendingValue(ESettingsField Field, int32 Direction);
    bool TryMatchFieldName(const FString& Input, ESettingsField& OutField) const;

    FText GetFieldValueTextFromPending(ESettingsField Field) const;
    FText GetQualityText(int32 Value) const;
    FText GetBoolText(bool bValue) const;
    FText GetDynamicGlobalIlluminationMethodText(int32 Value) const;
    FText GetReflectionMethodText(int32 Value) const;

    UPROPERTY()
    TArray<TObjectPtr<UTextBlock>> ValueTextBlocks;

    UPROPERTY()
    TArray<ESettingsField> ValueFields;

    TMap<TWeakObjectPtr<UButton>, ESettingsField> BoundFieldButtons;

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
