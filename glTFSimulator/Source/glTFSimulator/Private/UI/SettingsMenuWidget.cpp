// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/SettingsMenuWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Character/PlayerCharacterController.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Setting/GameSettings.h"
#include "System/GameManagerSubSystem.h"

static const TArray<ESettingsField>& GetDefaultSettingsFields()
{
    static const TArray<ESettingsField> Fields = {
        ESettingsField::BloomIntensity,
        ESettingsField::BloomThreshold,
        ESettingsField::AmbientOcclusionIntensity,
        ESettingsField::RayTracing,
        ESettingsField::HeightFog,
        ESettingsField::Cloud,
        ESettingsField::ShadowQuality,
        ESettingsField::TextureQuality,
        ESettingsField::ViewDistanceQuality,
        ESettingsField::AntiAliasingQuality,
        ESettingsField::PostProcessingQuality,
        ESettingsField::EffectsQuality,
        ESettingsField::FoliageQuality,
        ESettingsField::ShadingQuality,
        ESettingsField::GlobalIlluminationQuality,
        ESettingsField::ReflectionQuality,
        ESettingsField::DynamicGlobalIlluminationMethod,
        ESettingsField::ReflectionMethod
    };
    return Fields;
}

void USettingsAdjustmentButton::SetupAdjustment(USettingsMenuWidget* InOwner, ESettingsField InField, float InStep)
{
    OwnerWidget = InOwner;
    Field = InField;
    Step = InStep;
    OnClicked.RemoveAll(this);
    OnClicked.AddDynamic(this, &USettingsAdjustmentButton::HandleClicked);
}

void USettingsAdjustmentButton::HandleClicked()
{
    if (OwnerWidget)
    {
        OwnerWidget->AdjustSettingFromUI(Field, Step);
    }
}

void USettingsMenuWidget::NativeConstruct()
{
    Super::NativeConstruct();
    CacheUserWidgetReferences();
    BindButtonEvents();
    RefreshSettingsValues();
}

void USettingsMenuWidget::CacheUserWidgetReferences()
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
            TitleText = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("Settings_TitleText")));
        }
    }
    if (!ApplyButton)
    {
        ApplyButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("ApplyButton")));
        if (!ApplyButton)
        {
            ApplyButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("Settings_ApplyButton")));
        }
    }
    if (!BackButton)
    {
        BackButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("BackButton")));
        if (!BackButton)
        {
            BackButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("Settings_BackButton")));
        }
    }

    ValueTextBlocks.Reset();
    ValueFields.Reset();
    for (ESettingsField Field : GetDefaultSettingsFields())
    {
        if (UTextBlock* ValueText = Cast<UTextBlock>(WidgetTree->FindWidget(FName(*FString::Printf(TEXT("Settings_Value_%d"), static_cast<int32>(Field))))))
        {
            ValueTextBlocks.Add(ValueText);
            ValueFields.Add(Field);
        }
    }
}

void USettingsMenuWidget::BindButtonEvents()
{
    if (ApplyButton)
    {
        ApplyButton->OnClicked.RemoveAll(this);
        ApplyButton->OnClicked.AddDynamic(this, &USettingsMenuWidget::ApplyAndSaveSettingsFromUI);
    }
    if (BackButton)
    {
        BackButton->OnClicked.RemoveAll(this);
        BackButton->OnClicked.AddDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
    }
}

UGameSettings* USettingsMenuWidget::GetEditableSettings() const
{
    if (UGameManagerSubSystem* SubSystem = UGameManagerSubSystem::GetSubSystem(GetWorld()))
    {
        return SubSystem->GetGameSettings();
    }
    return nullptr;
}

void USettingsMenuWidget::RefreshSettingsValues()
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

void USettingsMenuWidget::AdjustSettingFromUI(ESettingsField Field, float Step)
{
    if (UGameSettings* Settings = GetEditableSettings())
    {
        AdjustSettingValue(Field, Step, Settings);
        RefreshSettingsValues();
    }
}

void USettingsMenuWidget::ApplyAndSaveSettingsFromUI()
{
    if (UGameManagerSubSystem* SubSystem = UGameManagerSubSystem::GetSubSystem(GetWorld()))
    {
        SubSystem->UpdateSettings();
        SubSystem->SaveSettings();
    }
    RefreshSettingsValues();
}

void USettingsMenuWidget::CloseSettingsFromUI()
{
    if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(GetOwningPlayer()))
    {
        PlayerController->ReturnToPauseMenuFromSettings();
    }
}

FText USettingsMenuWidget::GetFieldLabel(ESettingsField Field) const
{
    switch (Field)
    {
    case ESettingsField::BloomIntensity: return FText::FromString(TEXT("Bloom Intensity"));
    case ESettingsField::BloomThreshold: return FText::FromString(TEXT("Bloom Threshold"));
    case ESettingsField::AmbientOcclusionIntensity: return FText::FromString(TEXT("Ambient Occlusion"));
    case ESettingsField::RayTracing: return FText::FromString(TEXT("Ray Tracing"));
    case ESettingsField::HeightFog: return FText::FromString(TEXT("Height Fog"));
    case ESettingsField::Cloud: return FText::FromString(TEXT("Cloud"));
    case ESettingsField::ShadowQuality: return FText::FromString(TEXT("Shadow Quality"));
    case ESettingsField::TextureQuality: return FText::FromString(TEXT("Texture Quality"));
    case ESettingsField::ViewDistanceQuality: return FText::FromString(TEXT("View Distance"));
    case ESettingsField::AntiAliasingQuality: return FText::FromString(TEXT("Anti Aliasing"));
    case ESettingsField::PostProcessingQuality: return FText::FromString(TEXT("Post Processing"));
    case ESettingsField::EffectsQuality: return FText::FromString(TEXT("Effects"));
    case ESettingsField::FoliageQuality: return FText::FromString(TEXT("Foliage"));
    case ESettingsField::ShadingQuality: return FText::FromString(TEXT("Shading"));
    case ESettingsField::GlobalIlluminationQuality: return FText::FromString(TEXT("GI Quality"));
    case ESettingsField::ReflectionQuality: return FText::FromString(TEXT("Reflection Quality"));
    case ESettingsField::DynamicGlobalIlluminationMethod: return FText::FromString(TEXT("GI Method"));
    case ESettingsField::ReflectionMethod: return FText::FromString(TEXT("Reflection Method"));
    default: break;
    }
    return FText::FromString(TEXT("Unknown"));
}

FText USettingsMenuWidget::GetFieldValueText(ESettingsField Field, const UGameSettings* Settings) const
{
    if (!Settings)
    {
        return FText::FromString(TEXT("-"));
    }

    auto BoolText = [](bool bValue) { return FText::FromString(bValue ? TEXT("On") : TEXT("Off")); };

    switch (Field)
    {
    case ESettingsField::BloomIntensity: return FText::FromString(FString::Printf(TEXT("%.2f"), Settings->BloomIntensity));
    case ESettingsField::BloomThreshold: return FText::FromString(FString::Printf(TEXT("%.2f"), Settings->BloomThreshold));
    case ESettingsField::AmbientOcclusionIntensity: return FText::FromString(FString::Printf(TEXT("%.2f"), Settings->AmbientOcclusionIntensity));
    case ESettingsField::RayTracing: return BoolText(Settings->bRayTracing);
    case ESettingsField::HeightFog: return BoolText(Settings->bHeightFog);
    case ESettingsField::Cloud: return BoolText(Settings->bCloud);
    case ESettingsField::ShadowQuality: return FText::AsNumber(Settings->ShadowQuality);
    case ESettingsField::TextureQuality: return FText::AsNumber(Settings->TextureQuality);
    case ESettingsField::ViewDistanceQuality: return FText::AsNumber(Settings->ViewDistanceQuality);
    case ESettingsField::AntiAliasingQuality: return FText::AsNumber(Settings->AntiAliasingQuality);
    case ESettingsField::PostProcessingQuality: return FText::AsNumber(Settings->PostProcessingQuality);
    case ESettingsField::EffectsQuality: return FText::AsNumber(Settings->EffectsQuality);
    case ESettingsField::FoliageQuality: return FText::AsNumber(Settings->FoliageQuality);
    case ESettingsField::ShadingQuality: return FText::AsNumber(Settings->ShadingQuality);
    case ESettingsField::GlobalIlluminationQuality: return FText::AsNumber(Settings->GlobalIlluminationQuality);
    case ESettingsField::ReflectionQuality: return FText::AsNumber(Settings->ReflectionQuality);
    case ESettingsField::DynamicGlobalIlluminationMethod: return FText::AsNumber(Settings->DynamicGlobalIlluminationMethod);
    case ESettingsField::ReflectionMethod: return FText::AsNumber(Settings->ReflectionMethod);
    default: break;
    }
    return FText::FromString(TEXT("-"));
}

void USettingsMenuWidget::AdjustSettingValue(ESettingsField Field, float Step, UGameSettings* Settings) const
{
    if (!Settings)
    {
        return;
    }

    const int32 IntStep = Step >= 0.0f ? 1 : -1;
    switch (Field)
    {
    case ESettingsField::BloomIntensity:
        Settings->BloomIntensity = FMath::Clamp(Settings->BloomIntensity + Step * 0.1f, 0.0f, 10.0f);
        break;
    case ESettingsField::BloomThreshold:
        Settings->BloomThreshold = FMath::Clamp(Settings->BloomThreshold + Step * 0.1f, -1.0f, 10.0f);
        break;
    case ESettingsField::AmbientOcclusionIntensity:
        Settings->AmbientOcclusionIntensity = FMath::Clamp(Settings->AmbientOcclusionIntensity + Step * 0.1f, 0.0f, 5.0f);
        break;
    case ESettingsField::RayTracing:
        Settings->bRayTracing = !Settings->bRayTracing;
        break;
    case ESettingsField::HeightFog:
        Settings->bHeightFog = !Settings->bHeightFog;
        break;
    case ESettingsField::Cloud:
        Settings->bCloud = !Settings->bCloud;
        break;
    case ESettingsField::ShadowQuality:
        Settings->ShadowQuality = FMath::Clamp(Settings->ShadowQuality + IntStep, 0, 3);
        break;
    case ESettingsField::TextureQuality:
        Settings->TextureQuality = FMath::Clamp(Settings->TextureQuality + IntStep, 0, 3);
        break;
    case ESettingsField::ViewDistanceQuality:
        Settings->ViewDistanceQuality = FMath::Clamp(Settings->ViewDistanceQuality + IntStep, 0, 3);
        break;
    case ESettingsField::AntiAliasingQuality:
        Settings->AntiAliasingQuality = FMath::Clamp(Settings->AntiAliasingQuality + IntStep, 0, 3);
        break;
    case ESettingsField::PostProcessingQuality:
        Settings->PostProcessingQuality = FMath::Clamp(Settings->PostProcessingQuality + IntStep, 0, 3);
        break;
    case ESettingsField::EffectsQuality:
        Settings->EffectsQuality = FMath::Clamp(Settings->EffectsQuality + IntStep, 0, 3);
        break;
    case ESettingsField::FoliageQuality:
        Settings->FoliageQuality = FMath::Clamp(Settings->FoliageQuality + IntStep, 0, 3);
        break;
    case ESettingsField::ShadingQuality:
        Settings->ShadingQuality = FMath::Clamp(Settings->ShadingQuality + IntStep, 0, 3);
        break;
    case ESettingsField::GlobalIlluminationQuality:
        Settings->GlobalIlluminationQuality = FMath::Clamp(Settings->GlobalIlluminationQuality + IntStep, 0, 3);
        break;
    case ESettingsField::ReflectionQuality:
        Settings->ReflectionQuality = FMath::Clamp(Settings->ReflectionQuality + IntStep, 0, 3);
        break;
    case ESettingsField::DynamicGlobalIlluminationMethod:
        Settings->DynamicGlobalIlluminationMethod = FMath::Clamp(Settings->DynamicGlobalIlluminationMethod + IntStep, 0, 3);
        break;
    case ESettingsField::ReflectionMethod:
        Settings->ReflectionMethod = FMath::Clamp(Settings->ReflectionMethod + IntStep, 0, 2);
        break;
    default:
        break;
    }
}
