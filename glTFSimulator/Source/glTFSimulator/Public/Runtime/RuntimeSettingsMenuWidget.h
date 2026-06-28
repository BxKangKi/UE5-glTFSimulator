// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "RuntimeSettingsMenuWidget.generated.h"

class UTextBlock;
class UGameSettings;
class URuntimeSettingsMenuWidget;

UENUM(BlueprintType)
enum class ERuntimeSettingsField : uint8
{
    BloomIntensity UMETA(DisplayName="Bloom Intensity"),
    BloomThreshold UMETA(DisplayName="Bloom Threshold"),
    AmbientOcclusionIntensity UMETA(DisplayName="Ambient Occlusion"),
    RayTracing UMETA(DisplayName="Ray Tracing"),
    HeightFog UMETA(DisplayName="Height Fog"),
    Cloud UMETA(DisplayName="Cloud"),
    ShadowQuality UMETA(DisplayName="Shadow Quality"),
    TextureQuality UMETA(DisplayName="Texture Quality"),
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

/** Button helper that Blueprint widgets can use when they want +/- adjustment buttons. */
UCLASS(Blueprintable)
class GLTFSIMULATOR_API URuntimeSettingsAdjustmentButton : public UButton
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category="Runtime Settings")
    void SetupAdjustment(URuntimeSettingsMenuWidget* InOwner, ERuntimeSettingsField InField, float InStep);

    UFUNCTION()
    void HandleClicked();

private:
    UPROPERTY()
    TObjectPtr<URuntimeSettingsMenuWidget> OwnerWidget;

    UPROPERTY()
    ERuntimeSettingsField Field = ERuntimeSettingsField::BloomIntensity;

    UPROPERTY()
    float Step = 0.0f;
};

/**
 * Blueprint-editable settings menu that edits the existing UGameSettings/SettingData object.
 *
 * This class no longer creates a native fallback editor. Create the visual tree in a WBP child.
 * ApplyButton and BackButton are bound automatically when present; your own buttons can call
 * AdjustSettingFromUI, ApplyAndSaveSettingsFromUI, and CloseSettingsFromUI directly.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API URuntimeSettingsMenuWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    virtual void NativeConstruct() override;

    UFUNCTION(BlueprintCallable, Category="Runtime Settings")
    UGameSettings* GetEditableSettings() const;

    UFUNCTION(BlueprintCallable, Category="Runtime Settings")
    void RefreshSettingsValues();

    UFUNCTION(BlueprintCallable, Category="Runtime Settings")
    void AdjustSettingFromUI(ERuntimeSettingsField Field, float Step);

    UFUNCTION(BlueprintCallable, Category="Runtime Settings")
    void ApplyAndSaveSettingsFromUI();

    UFUNCTION(BlueprintCallable, Category="Runtime Settings")
    void CloseSettingsFromUI();

protected:
    UPROPERTY(BlueprintReadOnly, meta=(BindWidgetOptional), Category="Runtime Settings|Widgets")
    TObjectPtr<UTextBlock> TitleText;

    UPROPERTY(BlueprintReadOnly, meta=(BindWidgetOptional), Category="Runtime Settings|Widgets")
    TObjectPtr<UButton> ApplyButton;

    UPROPERTY(BlueprintReadOnly, meta=(BindWidgetOptional), Category="Runtime Settings|Widgets")
    TObjectPtr<UButton> BackButton;

private:
    void CacheUserWidgetReferences();
    void BindButtonEvents();
    FText GetFieldLabel(ERuntimeSettingsField Field) const;
    FText GetFieldValueText(ERuntimeSettingsField Field, const UGameSettings* Settings) const;
    void AdjustSettingValue(ERuntimeSettingsField Field, float Step, UGameSettings* Settings) const;

    UPROPERTY()
    TArray<TObjectPtr<UTextBlock>> ValueTextBlocks;

    UPROPERTY()
    TArray<ERuntimeSettingsField> ValueFields;
};
