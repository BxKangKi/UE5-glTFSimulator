// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Runtime/RuntimeSettingsMenuWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Character/PlayerCharacterController.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Setting/GameSettings.h"
#include "System/GameManagerSubSystem.h"

static const TArray<ERuntimeSettingsField>& GetDefaultSettingsFields()
{
    static const TArray<ERuntimeSettingsField> Fields = {
        ERuntimeSettingsField::BloomIntensity,
        ERuntimeSettingsField::BloomThreshold,
        ERuntimeSettingsField::AmbientOcclusionIntensity,
        ERuntimeSettingsField::RayTracing,
        ERuntimeSettingsField::HeightFog,
        ERuntimeSettingsField::Cloud,
        ERuntimeSettingsField::ShadowQuality,
        ERuntimeSettingsField::TextureQuality,
        ERuntimeSettingsField::ViewDistanceQuality,
        ERuntimeSettingsField::AntiAliasingQuality,
        ERuntimeSettingsField::PostProcessingQuality,
        ERuntimeSettingsField::EffectsQuality,
        ERuntimeSettingsField::FoliageQuality,
        ERuntimeSettingsField::ShadingQuality,
        ERuntimeSettingsField::GlobalIlluminationQuality,
        ERuntimeSettingsField::ReflectionQuality,
        ERuntimeSettingsField::DynamicGlobalIlluminationMethod,
        ERuntimeSettingsField::ReflectionMethod
    };
    return Fields;
}

void URuntimeSettingsAdjustmentButton::SetupAdjustment(URuntimeSettingsMenuWidget* InOwner, ERuntimeSettingsField InField, float InStep)
{
    OwnerWidget = InOwner;
    Field = InField;
    Step = InStep;
    OnClicked.RemoveAll(this);
    OnClicked.AddDynamic(this, &URuntimeSettingsAdjustmentButton::HandleClicked);
}

void URuntimeSettingsAdjustmentButton::HandleClicked()
{
    if (OwnerWidget)
    {
        OwnerWidget->AdjustSettingFromUI(Field, Step);
    }
}

void URuntimeSettingsMenuWidget::NativeConstruct()
{
    Super::NativeConstruct();
    CacheUserWidgetReferences();
    BindButtonEvents();
    RefreshSettingsValues();
}

void URuntimeSettingsMenuWidget::CacheUserWidgetReferences()
{
    if (!WidgetTree)
    {
        return;
    }

    if (!TitleText)
    {
        TitleText = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("TitleText")));
        if (!TitleText)
        {
            TitleText = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("RuntimeSettings_TitleText")));
        }
    }
    if (!ApplyButton)
    {
        ApplyButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("ApplyButton")));
        if (!ApplyButton)
        {
            ApplyButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("RuntimeSettings_ApplyButton")));
        }
    }
    if (!BackButton)
    {
        BackButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("BackButton")));
        if (!BackButton)
        {
            BackButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("RuntimeSettings_BackButton")));
        }
    }

    ValueTextBlocks.Reset();
    ValueFields.Reset();
    for (ERuntimeSettingsField Field : GetDefaultSettingsFields())
    {
        if (UTextBlock* ValueText = Cast<UTextBlock>(WidgetTree->FindWidget(FName(*FString::Printf(TEXT("RuntimeSettings_Value_%d"), static_cast<int32>(Field))))))
        {
            ValueTextBlocks.Add(ValueText);
            ValueFields.Add(Field);
        }
    }
}

void URuntimeSettingsMenuWidget::BindButtonEvents()
{
    if (ApplyButton)
    {
        ApplyButton->OnClicked.RemoveAll(this);
        ApplyButton->OnClicked.AddDynamic(this, &URuntimeSettingsMenuWidget::ApplyAndSaveSettingsFromUI);
    }
    if (BackButton)
    {
        BackButton->OnClicked.RemoveAll(this);
        BackButton->OnClicked.AddDynamic(this, &URuntimeSettingsMenuWidget::CloseSettingsFromUI);
    }
}

UGameSettings* URuntimeSettingsMenuWidget::GetEditableSettings() const
{
    if (UGameManagerSubSystem* SubSystem = UGameManagerSubSystem::GetSubSystem(GetWorld()))
    {
        return SubSystem->GetGameSettings();
    }
    return nullptr;
}

void URuntimeSettingsMenuWidget::RefreshSettingsValues()
{
    UGameSettings* Settings = GetEditableSettings();
    for (int32 Index = 0; Index < ValueTextBlocks.Num() && Index < ValueFields.Num(); ++Index)
    {
        if (ValueTextBlocks[Index])
        {
            ValueTextBlocks[Index]->SetText(GetFieldValueText(ValueFields[Index], Settings));
        }
    }
}

void URuntimeSettingsMenuWidget::AdjustSettingFromUI(ERuntimeSettingsField Field, float Step)
{
    if (UGameSettings* Settings = GetEditableSettings())
    {
        AdjustSettingValue(Field, Step, Settings);
        RefreshSettingsValues();
    }
}

void URuntimeSettingsMenuWidget::ApplyAndSaveSettingsFromUI()
{
    if (UGameManagerSubSystem* SubSystem = UGameManagerSubSystem::GetSubSystem(GetWorld()))
    {
        SubSystem->UpdateSettings();
        SubSystem->SaveSettings();
    }
    RefreshSettingsValues();
}

void URuntimeSettingsMenuWidget::CloseSettingsFromUI()
{
    if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(GetOwningPlayer()))
    {
        PlayerController->ReturnToPauseMenuFromSettings();
    }
}

FText URuntimeSettingsMenuWidget::GetFieldLabel(ERuntimeSettingsField Field) const
{
    switch (Field)
    {
    case ERuntimeSettingsField::BloomIntensity: return FText::FromString(TEXT("Bloom Intensity"));
    case ERuntimeSettingsField::BloomThreshold: return FText::FromString(TEXT("Bloom Threshold"));
    case ERuntimeSettingsField::AmbientOcclusionIntensity: return FText::FromString(TEXT("Ambient Occlusion"));
    case ERuntimeSettingsField::RayTracing: return FText::FromString(TEXT("Ray Tracing"));
    case ERuntimeSettingsField::HeightFog: return FText::FromString(TEXT("Height Fog"));
    case ERuntimeSettingsField::Cloud: return FText::FromString(TEXT("Cloud"));
    case ERuntimeSettingsField::ShadowQuality: return FText::FromString(TEXT("Shadow Quality"));
    case ERuntimeSettingsField::TextureQuality: return FText::FromString(TEXT("Texture Quality"));
    case ERuntimeSettingsField::ViewDistanceQuality: return FText::FromString(TEXT("View Distance"));
    case ERuntimeSettingsField::AntiAliasingQuality: return FText::FromString(TEXT("Anti Aliasing"));
    case ERuntimeSettingsField::PostProcessingQuality: return FText::FromString(TEXT("Post Processing"));
    case ERuntimeSettingsField::EffectsQuality: return FText::FromString(TEXT("Effects"));
    case ERuntimeSettingsField::FoliageQuality: return FText::FromString(TEXT("Foliage"));
    case ERuntimeSettingsField::ShadingQuality: return FText::FromString(TEXT("Shading"));
    case ERuntimeSettingsField::GlobalIlluminationQuality: return FText::FromString(TEXT("GI Quality"));
    case ERuntimeSettingsField::ReflectionQuality: return FText::FromString(TEXT("Reflection Quality"));
    case ERuntimeSettingsField::DynamicGlobalIlluminationMethod: return FText::FromString(TEXT("GI Method"));
    case ERuntimeSettingsField::ReflectionMethod: return FText::FromString(TEXT("Reflection Method"));
    default: break;
    }
    return FText::FromString(TEXT("Unknown"));
}

FText URuntimeSettingsMenuWidget::GetFieldValueText(ERuntimeSettingsField Field, const UGameSettings* Settings) const
{
    if (!Settings)
    {
        return FText::FromString(TEXT("-"));
    }

    auto BoolText = [](bool bValue) { return FText::FromString(bValue ? TEXT("On") : TEXT("Off")); };

    switch (Field)
    {
    case ERuntimeSettingsField::BloomIntensity: return FText::FromString(FString::Printf(TEXT("%.2f"), Settings->BloomIntensity));
    case ERuntimeSettingsField::BloomThreshold: return FText::FromString(FString::Printf(TEXT("%.2f"), Settings->BloomThreshold));
    case ERuntimeSettingsField::AmbientOcclusionIntensity: return FText::FromString(FString::Printf(TEXT("%.2f"), Settings->AmbientOcclusionIntensity));
    case ERuntimeSettingsField::RayTracing: return BoolText(Settings->bRayTracing);
    case ERuntimeSettingsField::HeightFog: return BoolText(Settings->bHeightFog);
    case ERuntimeSettingsField::Cloud: return BoolText(Settings->bCloud);
    case ERuntimeSettingsField::ShadowQuality: return FText::AsNumber(Settings->ShadowQuality);
    case ERuntimeSettingsField::TextureQuality: return FText::AsNumber(Settings->TextureQuality);
    case ERuntimeSettingsField::ViewDistanceQuality: return FText::AsNumber(Settings->ViewDistanceQuality);
    case ERuntimeSettingsField::AntiAliasingQuality: return FText::AsNumber(Settings->AntiAliasingQuality);
    case ERuntimeSettingsField::PostProcessingQuality: return FText::AsNumber(Settings->PostProcessingQuality);
    case ERuntimeSettingsField::EffectsQuality: return FText::AsNumber(Settings->EffectsQuality);
    case ERuntimeSettingsField::FoliageQuality: return FText::AsNumber(Settings->FoliageQuality);
    case ERuntimeSettingsField::ShadingQuality: return FText::AsNumber(Settings->ShadingQuality);
    case ERuntimeSettingsField::GlobalIlluminationQuality: return FText::AsNumber(Settings->GlobalIlluminationQuality);
    case ERuntimeSettingsField::ReflectionQuality: return FText::AsNumber(Settings->ReflectionQuality);
    case ERuntimeSettingsField::DynamicGlobalIlluminationMethod: return FText::AsNumber(Settings->DynamicGlobalIlluminationMethod);
    case ERuntimeSettingsField::ReflectionMethod: return FText::AsNumber(Settings->ReflectionMethod);
    default: break;
    }
    return FText::FromString(TEXT("-"));
}

void URuntimeSettingsMenuWidget::AdjustSettingValue(ERuntimeSettingsField Field, float Step, UGameSettings* Settings) const
{
    if (!Settings)
    {
        return;
    }

    const int32 IntStep = Step >= 0.0f ? 1 : -1;
    switch (Field)
    {
    case ERuntimeSettingsField::BloomIntensity:
        Settings->BloomIntensity = FMath::Clamp(Settings->BloomIntensity + Step * 0.1f, 0.0f, 10.0f);
        break;
    case ERuntimeSettingsField::BloomThreshold:
        Settings->BloomThreshold = FMath::Clamp(Settings->BloomThreshold + Step * 0.1f, -1.0f, 10.0f);
        break;
    case ERuntimeSettingsField::AmbientOcclusionIntensity:
        Settings->AmbientOcclusionIntensity = FMath::Clamp(Settings->AmbientOcclusionIntensity + Step * 0.1f, 0.0f, 5.0f);
        break;
    case ERuntimeSettingsField::RayTracing:
        Settings->bRayTracing = !Settings->bRayTracing;
        break;
    case ERuntimeSettingsField::HeightFog:
        Settings->bHeightFog = !Settings->bHeightFog;
        break;
    case ERuntimeSettingsField::Cloud:
        Settings->bCloud = !Settings->bCloud;
        break;
    case ERuntimeSettingsField::ShadowQuality:
        Settings->ShadowQuality = FMath::Clamp(Settings->ShadowQuality + IntStep, 0, 3);
        break;
    case ERuntimeSettingsField::TextureQuality:
        Settings->TextureQuality = FMath::Clamp(Settings->TextureQuality + IntStep, 0, 3);
        break;
    case ERuntimeSettingsField::ViewDistanceQuality:
        Settings->ViewDistanceQuality = FMath::Clamp(Settings->ViewDistanceQuality + IntStep, 0, 3);
        break;
    case ERuntimeSettingsField::AntiAliasingQuality:
        Settings->AntiAliasingQuality = FMath::Clamp(Settings->AntiAliasingQuality + IntStep, 0, 3);
        break;
    case ERuntimeSettingsField::PostProcessingQuality:
        Settings->PostProcessingQuality = FMath::Clamp(Settings->PostProcessingQuality + IntStep, 0, 3);
        break;
    case ERuntimeSettingsField::EffectsQuality:
        Settings->EffectsQuality = FMath::Clamp(Settings->EffectsQuality + IntStep, 0, 3);
        break;
    case ERuntimeSettingsField::FoliageQuality:
        Settings->FoliageQuality = FMath::Clamp(Settings->FoliageQuality + IntStep, 0, 3);
        break;
    case ERuntimeSettingsField::ShadingQuality:
        Settings->ShadingQuality = FMath::Clamp(Settings->ShadingQuality + IntStep, 0, 3);
        break;
    case ERuntimeSettingsField::GlobalIlluminationQuality:
        Settings->GlobalIlluminationQuality = FMath::Clamp(Settings->GlobalIlluminationQuality + IntStep, 0, 3);
        break;
    case ERuntimeSettingsField::ReflectionQuality:
        Settings->ReflectionQuality = FMath::Clamp(Settings->ReflectionQuality + IntStep, 0, 3);
        break;
    case ERuntimeSettingsField::DynamicGlobalIlluminationMethod:
        Settings->DynamicGlobalIlluminationMethod = FMath::Clamp(Settings->DynamicGlobalIlluminationMethod + IntStep, 0, 3);
        break;
    case ERuntimeSettingsField::ReflectionMethod:
        Settings->ReflectionMethod = FMath::Clamp(Settings->ReflectionMethod + IntStep, 0, 2);
        break;
    default:
        break;
    }
}
