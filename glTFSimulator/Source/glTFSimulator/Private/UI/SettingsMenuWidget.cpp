// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/SettingsMenuWidget.h"

#include "Character/PlayerCharacterController.h"
#include "Components/ContentWidget.h"
#include "Components/TextBlock.h"
#include "Setting/GameSettings.h"
#include "System/GameManagerSubSystem.h"

namespace
{
    constexpr float BloomStep = 0.25f;
    constexpr float BloomIntensityMin = 0.0f;
    constexpr float BloomIntensityMax = 10.0f;
    constexpr float BloomThresholdMin = -1.0f;
    constexpr float BloomThresholdMax = 10.0f;
    constexpr float AmbientOcclusionStep = 0.25f;
    constexpr float AmbientOcclusionMin = 0.0f;
    constexpr float AmbientOcclusionMax = 5.0f;
    constexpr int32 QualityMin = 0;
    constexpr int32 QualityMax = 3;
    constexpr int32 ReflectionMethodMin = 0;
    constexpr int32 ReflectionMethodMax = 2;
    constexpr int32 TextureResolutionMin = 256;
    constexpr int32 TextureResolutionMax = 4096;

    const TArray<ESettingsField>& GetDefaultSettingsFields()
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
            ESettingsField::MaxTextureResolution,
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

    int32 WrapIndex(int32 Value, int32 MinValue, int32 MaxValue)
    {
        const int32 Count = MaxValue - MinValue + 1;
        if (Count <= 0)
        {
            return MinValue;
        }

        int32 Offset = (Value - MinValue) % Count;
        if (Offset < 0)
        {
            Offset += Count;
        }
        return MinValue + Offset;
    }

    int32 NormalizedDirection(int32 Direction)
    {
        return Direction < 0 ? -1 : 1;
    }

    void CycleInt(int32& Value, int32 MinValue, int32 MaxValue, int32 Direction)
    {
        Value = WrapIndex(Value + NormalizedDirection(Direction), MinValue, MaxValue);
    }

    void CycleTextureResolution(int32& Value, int32 Direction)
    {
        static const int32 Options[] = {256, 512, 768, 1024, 1536, 2048, 4096};
        constexpr int32 OptionCount = sizeof(Options) / sizeof(Options[0]);

        int32 ClosestIndex = 0;
        int32 ClosestDistance = TNumericLimits<int32>::Max();
        for (int32 Index = 0; Index < OptionCount; ++Index)
        {
            const int32 Distance = FMath::Abs(Value - Options[Index]);
            if (Distance < ClosestDistance)
            {
                ClosestDistance = Distance;
                ClosestIndex = Index;
            }
        }

        Value = Options[WrapIndex(ClosestIndex + NormalizedDirection(Direction), 0, OptionCount - 1)];
    }

    void CycleFloat(float& Value, float MinValue, float MaxValue, float Step, int32 Direction)
    {
        const int32 Count = FMath::Max(1, FMath::RoundToInt((MaxValue - MinValue) / Step) + 1);
        int32 CurrentIndex = FMath::RoundToInt((FMath::Clamp(Value, MinValue, MaxValue) - MinValue) / Step);
        CurrentIndex = FMath::Clamp(CurrentIndex, 0, Count - 1);

        int32 NewIndex = (CurrentIndex + NormalizedDirection(Direction)) % Count;
        if (NewIndex < 0)
        {
            NewIndex += Count;
        }

        Value = MinValue + static_cast<float>(NewIndex) * Step;
        Value = FMath::Clamp(Value, MinValue, MaxValue);
    }

    FString NormalizeFieldText(FString Text)
    {
        Text.TrimStartAndEndInline();
        Text.ToLowerInline();
        Text.ReplaceInline(TEXT(" "), TEXT(""));
        Text.ReplaceInline(TEXT("_"), TEXT(""));
        Text.ReplaceInline(TEXT("-"), TEXT(""));
        return Text;
    }

    FString StripValuePart(const FString& Text)
    {
        FString Left;
        FString Right;
        if (Text.Split(TEXT(":"), &Left, &Right))
        {
            return Left;
        }
        return Text;
    }

    FName FieldName(ESettingsField Field)
    {
        switch (Field)
        {
        case ESettingsField::BloomIntensity: return TEXT("BloomIntensity");
        case ESettingsField::BloomThreshold: return TEXT("BloomThreshold");
        case ESettingsField::AmbientOcclusionIntensity: return TEXT("AmbientOcclusionIntensity");
        case ESettingsField::RayTracing: return TEXT("RayTracing");
        case ESettingsField::HeightFog: return TEXT("HeightFog");
        case ESettingsField::Cloud: return TEXT("Cloud");
        case ESettingsField::ShadowQuality: return TEXT("ShadowQuality");
        case ESettingsField::TextureQuality: return TEXT("TextureQuality");
        case ESettingsField::MaxTextureResolution: return TEXT("MaxTextureResolution");
        case ESettingsField::ViewDistanceQuality: return TEXT("ViewDistanceQuality");
        case ESettingsField::AntiAliasingQuality: return TEXT("AntiAliasingQuality");
        case ESettingsField::PostProcessingQuality: return TEXT("PostProcessingQuality");
        case ESettingsField::EffectsQuality: return TEXT("EffectsQuality");
        case ESettingsField::FoliageQuality: return TEXT("FoliageQuality");
        case ESettingsField::ShadingQuality: return TEXT("ShadingQuality");
        case ESettingsField::GlobalIlluminationQuality: return TEXT("GlobalIlluminationQuality");
        case ESettingsField::ReflectionQuality: return TEXT("ReflectionQuality");
        case ESettingsField::DynamicGlobalIlluminationMethod: return TEXT("DynamicGlobalIlluminationMethod");
        case ESettingsField::ReflectionMethod: return TEXT("ReflectionMethod");
        default: return NAME_None;
        }
    }
}

void USettingsAdjustmentButton::SetupAdjustment(USettingsMenuWidget* InOwner, ESettingsField InField, float InStep)
{
    OwnerWidget = InOwner;
    Field = InField;
    Step = InStep;
    OnClicked.RemoveDynamic(this, &USettingsAdjustmentButton::HandleClicked);
    OnClicked.AddDynamic(this, &USettingsAdjustmentButton::HandleClicked);
}

void USettingsAdjustmentButton::HandleClicked()
{
    if (OwnerWidget)
    {
        OwnerWidget->AdjustSettingFromUI(Field, Step);
    }
}

void USettingsCycleButton::SetupCycleButton(USettingsMenuWidget* InOwner, ESettingsField InField, int32 InDirection)
{
    OwnerWidget = InOwner;
    Field = InField;
    Direction = InDirection;
    OnClicked.RemoveDynamic(this, &USettingsCycleButton::HandleClicked);
    OnClicked.AddDynamic(this, &USettingsCycleButton::HandleClicked);
    RefreshDisplayedText();
}

void USettingsCycleButton::RefreshDisplayedText()
{
    if (OwnerWidget)
    {
        OwnerWidget->SetButtonTextFromUI(this, OwnerWidget->GetSettingButtonText(Field));
    }
}

void USettingsCycleButton::HandleClicked()
{
    if (OwnerWidget)
    {
        OwnerWidget->CycleSettingValueFromUI(Field, Direction);
        RefreshDisplayedText();
    }
}

void USettingsMenuWidget::NativeConstruct()
{
    Super::NativeConstruct();

    CollectAssignedWidgetReferences();
    BindButtonEvents();
    InitializeSettingsFromSavedData();
}

void USettingsMenuWidget::NativeDestruct()
{
    UnbindButtonEvents();
    BoundFieldButtons.Empty();
    AssignedTitleText.Reset();
    AssignedApplyButton.Reset();
    AssignedConfirmButton.Reset();
    AssignedBackButton.Reset();
    AssignedCancelButton.Reset();
    RegisteredValueTextWidgets.Empty();
    RegisteredSettingButtons.Empty();
    ValueTextBlocks.Empty();
    ValueFields.Empty();

    Super::NativeDestruct();
}

void USettingsMenuWidget::SetTitleText(UTextBlock* InTitleText)
{
    AssignedTitleText = InTitleText;
}

void USettingsMenuWidget::SetApplyButton(UButton* InButton)
{
    if (UButton* Button = AssignedApplyButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::ApplyAndSaveSettingsFromUI);
    }

    AssignedApplyButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::ApplyAndSaveSettingsFromUI);
        InButton->OnClicked.AddDynamic(this, &USettingsMenuWidget::ApplyAndSaveSettingsFromUI);
    }
}

void USettingsMenuWidget::SetConfirmButton(UButton* InButton)
{
    if (UButton* Button = AssignedConfirmButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::ConfirmSettingsFromUI);
    }

    AssignedConfirmButton = InButton;
    if (IsValid(InButton) && InButton != GetApplyButton())
    {
        InButton->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::ConfirmSettingsFromUI);
        InButton->OnClicked.AddDynamic(this, &USettingsMenuWidget::ConfirmSettingsFromUI);
    }
}

void USettingsMenuWidget::SetBackButton(UButton* InButton)
{
    if (UButton* Button = AssignedBackButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
    }

    AssignedBackButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
        InButton->OnClicked.AddDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
    }
}

void USettingsMenuWidget::SetCancelButton(UButton* InButton)
{
    if (UButton* Button = AssignedCancelButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
    }

    AssignedCancelButton = InButton;
    if (IsValid(InButton) && InButton != GetBackButton())
    {
        InButton->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
        InButton->OnClicked.AddDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
    }
}

void USettingsMenuWidget::RegisterSettingValueText(ESettingsField Field, UTextBlock* InTextBlock)
{
    if (IsValid(InTextBlock))
    {
        RegisteredValueTextWidgets.FindOrAdd(Field) = InTextBlock;
    }
    else
    {
        RegisteredValueTextWidgets.Remove(Field);
    }

    CollectAssignedWidgetReferences();
    RefreshSettingsValues();
}

void USettingsMenuWidget::RegisterSettingButton(ESettingsField Field, UButton* InButton)
{
    if (TWeakObjectPtr<UButton>* ExistingButtonPtr = RegisteredSettingButtons.Find(Field))
    {
        if (UButton* ExistingButton = ExistingButtonPtr->Get())
        {
            UnbindFieldButton(Field, ExistingButton);
            BoundFieldButtons.Remove(TWeakObjectPtr<UButton>(ExistingButton));
        }
    }

    if (IsValid(InButton))
    {
        RegisteredSettingButtons.FindOrAdd(Field) = InButton;
        BindFieldButton(Field, InButton);
    }
    else
    {
        RegisteredSettingButtons.Remove(Field);
    }

    RefreshSettingsValues();
}

UGameSettings* USettingsMenuWidget::GetEditableSettings() const
{
    if (UGameManagerSubSystem* SubSystem = UGameManagerSubSystem::GetSubSystem(GetWorld()))
    {
        return SubSystem->GetGameSettings();
    }
    return nullptr;
}

void USettingsMenuWidget::InitializeSettingsFromSavedData()
{
    CopySettingsToPending(GetEditableSettings());
    RefreshSettingsValues();
}

void USettingsMenuWidget::CollectAssignedWidgetReferences()
{
    ValueTextBlocks.Reset();
    ValueFields.Reset();

    for (ESettingsField Field : GetDefaultSettingsFields())
    {
        if (TWeakObjectPtr<UTextBlock>* TextBlockPtr = RegisteredValueTextWidgets.Find(Field))
        {
            RegisterValueTextBlock(Field, TextBlockPtr->Get());
        }
    }
}

void USettingsMenuWidget::RegisterValueTextBlock(ESettingsField Field, UTextBlock* TextBlock)
{
    if (!IsValid(TextBlock))
    {
        return;
    }

    ValueTextBlocks.Add(TextBlock);
    ValueFields.Add(Field);
}

void USettingsMenuWidget::BindButtonEvents()
{
    if (UButton* Button = GetApplyButton())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::ApplyAndSaveSettingsFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::ApplyAndSaveSettingsFromUI);
    }
    if (UButton* Button = GetConfirmButton())
    {
        if (Button != GetApplyButton())
        {
            Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::ConfirmSettingsFromUI);
            Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::ConfirmSettingsFromUI);
        }
    }
    if (UButton* Button = GetBackButton())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
    }
    if (UButton* Button = GetCancelButton())
    {
        if (Button != GetBackButton())
        {
            Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
            Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
        }
    }

    BindAssignedSettingButtons();
}

void USettingsMenuWidget::UnbindButtonEvents()
{
    if (UButton* Button = GetApplyButton())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::ApplyAndSaveSettingsFromUI);
    }
    if (UButton* Button = GetConfirmButton())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::ConfirmSettingsFromUI);
    }
    if (UButton* Button = GetBackButton())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
    }
    if (UButton* Button = GetCancelButton())
    {
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CloseSettingsFromUI);
    }

    for (const TPair<TWeakObjectPtr<UButton>, ESettingsField>& Pair : BoundFieldButtons)
    {
        if (UButton* Button = Pair.Key.Get())
        {
            UnbindFieldButton(Pair.Value, Button);
        }
    }
}

void USettingsMenuWidget::BindAssignedSettingButtons()
{
    BoundFieldButtons.Empty();

    for (ESettingsField Field : GetDefaultSettingsFields())
    {
        if (TWeakObjectPtr<UButton>* ButtonPtr = RegisteredSettingButtons.Find(Field))
        {
            BindAssignedSettingButton(Field, ButtonPtr->Get());
        }
    }
}

void USettingsMenuWidget::BindAssignedSettingButton(ESettingsField Field, UButton* Button)
{
    BindFieldButton(Field, Button);
}

void USettingsMenuWidget::BindFieldButton(ESettingsField Field, UButton* Button)
{
    if (!IsValid(Button) || Button == GetApplyButton() || Button == GetConfirmButton() || Button == GetBackButton() || Button == GetCancelButton())
    {
        return;
    }

    if (USettingsCycleButton* CycleButton = Cast<USettingsCycleButton>(Button))
    {
        CycleButton->SetupCycleButton(this, Field, CycleButton->Direction);
        BoundFieldButtons.Add(TWeakObjectPtr<UButton>(Button), Field);
        return;
    }

    if (USettingsAdjustmentButton* AdjustmentButton = Cast<USettingsAdjustmentButton>(Button))
    {
        AdjustmentButton->SetupAdjustment(this, Field, AdjustmentButton->Step);
        BoundFieldButtons.Add(TWeakObjectPtr<UButton>(Button), Field);
        return;
    }

    switch (Field)
    {
    case ESettingsField::BloomIntensity:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleBloomIntensityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleBloomIntensityFromUI);
        break;
    case ESettingsField::BloomThreshold:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleBloomThresholdFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleBloomThresholdFromUI);
        break;
    case ESettingsField::AmbientOcclusionIntensity:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleAmbientOcclusionIntensityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleAmbientOcclusionIntensityFromUI);
        break;
    case ESettingsField::RayTracing:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleRayTracingFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleRayTracingFromUI);
        break;
    case ESettingsField::HeightFog:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleHeightFogFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleHeightFogFromUI);
        break;
    case ESettingsField::Cloud:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleCloudFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleCloudFromUI);
        break;
    case ESettingsField::ShadowQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleShadowQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleShadowQualityFromUI);
        break;
    case ESettingsField::TextureQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleTextureQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleTextureQualityFromUI);
        break;
    case ESettingsField::MaxTextureResolution:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleMaxTextureResolutionFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleMaxTextureResolutionFromUI);
        break;
    case ESettingsField::ViewDistanceQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleViewDistanceQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleViewDistanceQualityFromUI);
        break;
    case ESettingsField::AntiAliasingQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleAntiAliasingQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleAntiAliasingQualityFromUI);
        break;
    case ESettingsField::PostProcessingQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CyclePostProcessingQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CyclePostProcessingQualityFromUI);
        break;
    case ESettingsField::EffectsQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleEffectsQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleEffectsQualityFromUI);
        break;
    case ESettingsField::FoliageQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleFoliageQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleFoliageQualityFromUI);
        break;
    case ESettingsField::ShadingQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleShadingQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleShadingQualityFromUI);
        break;
    case ESettingsField::GlobalIlluminationQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleGlobalIlluminationQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleGlobalIlluminationQualityFromUI);
        break;
    case ESettingsField::ReflectionQuality:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleReflectionQualityFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleReflectionQualityFromUI);
        break;
    case ESettingsField::DynamicGlobalIlluminationMethod:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleDynamicGlobalIlluminationMethodFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleDynamicGlobalIlluminationMethodFromUI);
        break;
    case ESettingsField::ReflectionMethod:
        Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleReflectionMethodFromUI);
        Button->OnClicked.AddDynamic(this, &USettingsMenuWidget::CycleReflectionMethodFromUI);
        break;
    default: break;
    }

    BoundFieldButtons.Add(TWeakObjectPtr<UButton>(Button), Field);
}

void USettingsMenuWidget::UnbindFieldButton(ESettingsField Field, UButton* Button)
{
    if (!IsValid(Button))
    {
        return;
    }

    if (USettingsAdjustmentButton* AdjustmentButton = Cast<USettingsAdjustmentButton>(Button))
    {
        AdjustmentButton->OnClicked.RemoveDynamic(AdjustmentButton, &USettingsAdjustmentButton::HandleClicked);
        return;
    }

    if (USettingsCycleButton* CycleButton = Cast<USettingsCycleButton>(Button))
    {
        CycleButton->OnClicked.RemoveDynamic(CycleButton, &USettingsCycleButton::HandleClicked);
        return;
    }

    switch (Field)
    {
    case ESettingsField::BloomIntensity: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleBloomIntensityFromUI); break;
    case ESettingsField::BloomThreshold: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleBloomThresholdFromUI); break;
    case ESettingsField::AmbientOcclusionIntensity: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleAmbientOcclusionIntensityFromUI); break;
    case ESettingsField::RayTracing: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleRayTracingFromUI); break;
    case ESettingsField::HeightFog: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleHeightFogFromUI); break;
    case ESettingsField::Cloud: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleCloudFromUI); break;
    case ESettingsField::ShadowQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleShadowQualityFromUI); break;
    case ESettingsField::TextureQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleTextureQualityFromUI); break;
    case ESettingsField::MaxTextureResolution: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleMaxTextureResolutionFromUI); break;
    case ESettingsField::ViewDistanceQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleViewDistanceQualityFromUI); break;
    case ESettingsField::AntiAliasingQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleAntiAliasingQualityFromUI); break;
    case ESettingsField::PostProcessingQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CyclePostProcessingQualityFromUI); break;
    case ESettingsField::EffectsQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleEffectsQualityFromUI); break;
    case ESettingsField::FoliageQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleFoliageQualityFromUI); break;
    case ESettingsField::ShadingQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleShadingQualityFromUI); break;
    case ESettingsField::GlobalIlluminationQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleGlobalIlluminationQualityFromUI); break;
    case ESettingsField::ReflectionQuality: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleReflectionQualityFromUI); break;
    case ESettingsField::DynamicGlobalIlluminationMethod: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleDynamicGlobalIlluminationMethodFromUI); break;
    case ESettingsField::ReflectionMethod: Button->OnClicked.RemoveDynamic(this, &USettingsMenuWidget::CycleReflectionMethodFromUI); break;
    default: break;
    }
}

void USettingsMenuWidget::CopySettingsToPending(const UGameSettings* Settings)
{
    if (!Settings)
    {
        return;
    }

    PendingBloomIntensity = FMath::Clamp(Settings->BloomIntensity, BloomIntensityMin, BloomIntensityMax);
    PendingBloomThreshold = FMath::Clamp(Settings->BloomThreshold, BloomThresholdMin, BloomThresholdMax);
    PendingAmbientOcclusionIntensity = FMath::Clamp(Settings->AmbientOcclusionIntensity, AmbientOcclusionMin, AmbientOcclusionMax);
    bPendingRayTracing = Settings->bRayTracing;
    bPendingHeightFog = Settings->bHeightFog;
    bPendingCloud = Settings->bCloud;
    PendingShadowQuality = FMath::Clamp(Settings->ShadowQuality, QualityMin, QualityMax);
    PendingTextureQuality = FMath::Clamp(Settings->TextureQuality, QualityMin, QualityMax);
    PendingMaxTextureResolution = FMath::Clamp(Settings->MaxTextureResolution, TextureResolutionMin, TextureResolutionMax);
    PendingViewDistanceQuality = FMath::Clamp(Settings->ViewDistanceQuality, QualityMin, QualityMax);
    PendingAntiAliasingQuality = FMath::Clamp(Settings->AntiAliasingQuality, QualityMin, QualityMax);
    PendingPostProcessingQuality = FMath::Clamp(Settings->PostProcessingQuality, QualityMin, QualityMax);
    PendingEffectsQuality = FMath::Clamp(Settings->EffectsQuality, QualityMin, QualityMax);
    PendingFoliageQuality = FMath::Clamp(Settings->FoliageQuality, QualityMin, QualityMax);
    PendingShadingQuality = FMath::Clamp(Settings->ShadingQuality, QualityMin, QualityMax);
    PendingGlobalIlluminationQuality = FMath::Clamp(Settings->GlobalIlluminationQuality, QualityMin, QualityMax);
    PendingReflectionQuality = FMath::Clamp(Settings->ReflectionQuality, QualityMin, QualityMax);
    PendingDynamicGlobalIlluminationMethod = FMath::Clamp(Settings->DynamicGlobalIlluminationMethod, QualityMin, QualityMax);
    PendingReflectionMethod = FMath::Clamp(Settings->ReflectionMethod, ReflectionMethodMin, ReflectionMethodMax);
}

void USettingsMenuWidget::ApplyPendingToSettings(UGameSettings* Settings) const
{
    if (!Settings)
    {
        return;
    }

    Settings->BloomIntensity = PendingBloomIntensity;
    Settings->BloomThreshold = PendingBloomThreshold;
    Settings->AmbientOcclusionIntensity = PendingAmbientOcclusionIntensity;
    Settings->bRayTracing = bPendingRayTracing;
    Settings->bHeightFog = bPendingHeightFog;
    Settings->bCloud = bPendingCloud;
    Settings->ShadowQuality = PendingShadowQuality;
    Settings->TextureQuality = PendingTextureQuality;
    Settings->MaxTextureResolution = PendingMaxTextureResolution;
    Settings->ViewDistanceQuality = PendingViewDistanceQuality;
    Settings->AntiAliasingQuality = PendingAntiAliasingQuality;
    Settings->PostProcessingQuality = PendingPostProcessingQuality;
    Settings->EffectsQuality = PendingEffectsQuality;
    Settings->FoliageQuality = PendingFoliageQuality;
    Settings->ShadingQuality = PendingShadingQuality;
    Settings->GlobalIlluminationQuality = PendingGlobalIlluminationQuality;
    Settings->ReflectionQuality = PendingReflectionQuality;
    Settings->DynamicGlobalIlluminationMethod = PendingDynamicGlobalIlluminationMethod;
    Settings->ReflectionMethod = PendingReflectionMethod;
}

void USettingsMenuWidget::RefreshSettingsValues()
{
    for (int32 Index = 0; Index < ValueTextBlocks.Num() && Index < ValueFields.Num(); ++Index)
    {
        if (UTextBlock* TextBlock = ValueTextBlocks[Index].Get())
        {
            TextBlock->SetText(GetPendingSettingValueText(ValueFields[Index]));
        }
    }

    for (const TPair<TWeakObjectPtr<UButton>, ESettingsField>& Pair : BoundFieldButtons)
    {
        if (UButton* Button = Pair.Key.Get())
        {
            SetButtonTextFromUI(Button, GetSettingButtonText(Pair.Value));
        }
    }
}

void USettingsMenuWidget::CycleSettingValueFromUI(ESettingsField Field, int32 Direction)
{
    CyclePendingValue(Field, Direction);
    RefreshSettingsValues();
}

void USettingsMenuWidget::CycleSettingByNameFromUI(FName FieldNameValue, int32 Direction)
{
    ESettingsField Field = ESettingsField::BloomIntensity;
    if (TryMatchFieldName(FieldNameValue.ToString(), Field))
    {
        CycleSettingValueFromUI(Field, Direction);
    }
}

void USettingsMenuWidget::CycleSettingByButtonTextFromUI(UButton* SourceButton, int32 Direction)
{
    if (!SourceButton)
    {
        return;
    }

    ESettingsField Field = ESettingsField::BloomIntensity;
    if (TryGetSettingFieldFromButtonText(GetButtonTextFromUI(SourceButton), Field))
    {
        CycleSettingValueFromUI(Field, Direction);
    }
}

void USettingsMenuWidget::AdjustSettingFromUI(ESettingsField Field, float Step)
{
    CycleSettingValueFromUI(Field, Step < 0.0f ? -1 : 1);
}

void USettingsMenuWidget::ApplyAndSaveSettingsFromUI()
{
    if (UGameManagerSubSystem* SubSystem = UGameManagerSubSystem::GetSubSystem(GetWorld()))
    {
        ApplyPendingToSettings(SubSystem->GetGameSettings());
        SubSystem->UpdateSettings();
        SubSystem->SaveSettings();
    }

    RefreshSettingsValues();
}

void USettingsMenuWidget::ConfirmSettingsFromUI()
{
    ApplyAndSaveSettingsFromUI();
}

void USettingsMenuWidget::DiscardPendingSettingsFromUI()
{
    InitializeSettingsFromSavedData();
}

void USettingsMenuWidget::CloseSettingsFromUI()
{
    // Back/Cancel closes without committing pending changes.
    DiscardPendingSettingsFromUI();

    if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(GetOwningPlayer()))
    {
        PlayerController->ReturnToPauseMenuFromSettings();
    }
}

TArray<ESettingsField> USettingsMenuWidget::GetSettingFieldList() const
{
    return GetDefaultSettingsFields();
}

FText USettingsMenuWidget::GetSettingLabelText(ESettingsField Field) const
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
    case ESettingsField::MaxTextureResolution: return FText::FromString(TEXT("Max Texture Resolution"));
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

FText USettingsMenuWidget::GetPendingSettingValueText(ESettingsField Field) const
{
    return GetFieldValueTextFromPending(Field);
}

FText USettingsMenuWidget::GetSettingButtonText(ESettingsField Field) const
{
    return FText::FromString(FString::Printf(TEXT("%s: %s"), *GetSettingLabelText(Field).ToString(), *GetPendingSettingValueText(Field).ToString()));
}

TArray<FText> USettingsMenuWidget::GetSettingOptionTexts(ESettingsField Field) const
{
    TArray<FText> Options;

    switch (Field)
    {
    case ESettingsField::BloomIntensity:
        for (float Value = BloomIntensityMin; Value <= BloomIntensityMax + KINDA_SMALL_NUMBER; Value += BloomStep)
        {
            Options.Add(FText::FromString(FString::Printf(TEXT("%.2f"), Value)));
        }
        break;
    case ESettingsField::BloomThreshold:
        for (float Value = BloomThresholdMin; Value <= BloomThresholdMax + KINDA_SMALL_NUMBER; Value += BloomStep)
        {
            Options.Add(FText::FromString(FString::Printf(TEXT("%.2f"), Value)));
        }
        break;
    case ESettingsField::AmbientOcclusionIntensity:
        for (float Value = AmbientOcclusionMin; Value <= AmbientOcclusionMax + KINDA_SMALL_NUMBER; Value += AmbientOcclusionStep)
        {
            Options.Add(FText::FromString(FString::Printf(TEXT("%.2f"), Value)));
        }
        break;
    case ESettingsField::RayTracing:
    case ESettingsField::HeightFog:
    case ESettingsField::Cloud:
        Options.Add(GetBoolText(false));
        Options.Add(GetBoolText(true));
        break;
    case ESettingsField::DynamicGlobalIlluminationMethod:
        for (int32 Value = QualityMin; Value <= QualityMax; ++Value)
        {
            Options.Add(GetDynamicGlobalIlluminationMethodText(Value));
        }
        break;
    case ESettingsField::ReflectionMethod:
        for (int32 Value = ReflectionMethodMin; Value <= ReflectionMethodMax; ++Value)
        {
            Options.Add(GetReflectionMethodText(Value));
        }
        break;
    case ESettingsField::MaxTextureResolution:
        {
            static const int32 OptionsPx[] = {256, 512, 768, 1024, 1536, 2048, 4096};
            for (const int32 Value : OptionsPx)
            {
                Options.Add(FText::FromString(FString::Printf(TEXT("%d px"), Value)));
            }
        }
        break;
    default:
        for (int32 Value = QualityMin; Value <= QualityMax; ++Value)
        {
            Options.Add(GetQualityText(Value));
        }
        break;
    }

    return Options;
}

FText USettingsMenuWidget::GetButtonTextFromUI(UButton* SourceButton) const
{
    if (!SourceButton)
    {
        return FText::GetEmpty();
    }

    if (UTextBlock* TextBlock = Cast<UTextBlock>(SourceButton->GetContent()))
    {
        return TextBlock->GetText();
    }

    return FText::GetEmpty();
}

bool USettingsMenuWidget::SetButtonTextFromUI(UButton* SourceButton, const FText& NewText) const
{
    if (!SourceButton)
    {
        return false;
    }

    if (UTextBlock* TextBlock = Cast<UTextBlock>(SourceButton->GetContent()))
    {
        TextBlock->SetText(NewText);
        return true;
    }

    return false;
}

bool USettingsMenuWidget::TryGetSettingFieldFromButtonText(const FText& ButtonText, ESettingsField& OutField) const
{
    return TryMatchFieldName(StripValuePart(ButtonText.ToString()), OutField);
}

void USettingsMenuWidget::CyclePendingValue(ESettingsField Field, int32 Direction)
{
    switch (Field)
    {
    case ESettingsField::BloomIntensity: CycleFloat(PendingBloomIntensity, BloomIntensityMin, BloomIntensityMax, BloomStep, Direction); break;
    case ESettingsField::BloomThreshold: CycleFloat(PendingBloomThreshold, BloomThresholdMin, BloomThresholdMax, BloomStep, Direction); break;
    case ESettingsField::AmbientOcclusionIntensity: CycleFloat(PendingAmbientOcclusionIntensity, AmbientOcclusionMin, AmbientOcclusionMax, AmbientOcclusionStep, Direction); break;
    case ESettingsField::RayTracing: bPendingRayTracing = !bPendingRayTracing; break;
    case ESettingsField::HeightFog: bPendingHeightFog = !bPendingHeightFog; break;
    case ESettingsField::Cloud: bPendingCloud = !bPendingCloud; break;
    case ESettingsField::ShadowQuality: CycleInt(PendingShadowQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::TextureQuality: CycleInt(PendingTextureQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::MaxTextureResolution: CycleTextureResolution(PendingMaxTextureResolution, Direction); break;
    case ESettingsField::ViewDistanceQuality: CycleInt(PendingViewDistanceQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::AntiAliasingQuality: CycleInt(PendingAntiAliasingQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::PostProcessingQuality: CycleInt(PendingPostProcessingQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::EffectsQuality: CycleInt(PendingEffectsQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::FoliageQuality: CycleInt(PendingFoliageQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::ShadingQuality: CycleInt(PendingShadingQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::GlobalIlluminationQuality: CycleInt(PendingGlobalIlluminationQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::ReflectionQuality: CycleInt(PendingReflectionQuality, QualityMin, QualityMax, Direction); break;
    case ESettingsField::DynamicGlobalIlluminationMethod: CycleInt(PendingDynamicGlobalIlluminationMethod, QualityMin, QualityMax, Direction); break;
    case ESettingsField::ReflectionMethod: CycleInt(PendingReflectionMethod, ReflectionMethodMin, ReflectionMethodMax, Direction); break;
    default: break;
    }
}

bool USettingsMenuWidget::TryMatchFieldName(const FString& Input, ESettingsField& OutField) const
{
    const FString NormalizedInput = NormalizeFieldText(StripValuePart(Input));
    for (ESettingsField Field : GetDefaultSettingsFields())
    {
        const FString Label = NormalizeFieldText(GetSettingLabelText(Field).ToString());
        const FString Name = NormalizeFieldText(FieldName(Field).ToString());
        if (NormalizedInput == Label || NormalizedInput == Name)
        {
            OutField = Field;
            return true;
        }
    }
    return false;
}

FText USettingsMenuWidget::GetFieldValueTextFromPending(ESettingsField Field) const
{
    switch (Field)
    {
    case ESettingsField::BloomIntensity: return FText::FromString(FString::Printf(TEXT("%.2f"), PendingBloomIntensity));
    case ESettingsField::BloomThreshold: return FText::FromString(FString::Printf(TEXT("%.2f"), PendingBloomThreshold));
    case ESettingsField::AmbientOcclusionIntensity: return FText::FromString(FString::Printf(TEXT("%.2f"), PendingAmbientOcclusionIntensity));
    case ESettingsField::RayTracing: return GetBoolText(bPendingRayTracing);
    case ESettingsField::HeightFog: return GetBoolText(bPendingHeightFog);
    case ESettingsField::Cloud: return GetBoolText(bPendingCloud);
    case ESettingsField::ShadowQuality: return GetQualityText(PendingShadowQuality);
    case ESettingsField::TextureQuality: return GetQualityText(PendingTextureQuality);
    case ESettingsField::MaxTextureResolution: return FText::FromString(FString::Printf(TEXT("%d px"), PendingMaxTextureResolution));
    case ESettingsField::ViewDistanceQuality: return GetQualityText(PendingViewDistanceQuality);
    case ESettingsField::AntiAliasingQuality: return GetQualityText(PendingAntiAliasingQuality);
    case ESettingsField::PostProcessingQuality: return GetQualityText(PendingPostProcessingQuality);
    case ESettingsField::EffectsQuality: return GetQualityText(PendingEffectsQuality);
    case ESettingsField::FoliageQuality: return GetQualityText(PendingFoliageQuality);
    case ESettingsField::ShadingQuality: return GetQualityText(PendingShadingQuality);
    case ESettingsField::GlobalIlluminationQuality: return GetQualityText(PendingGlobalIlluminationQuality);
    case ESettingsField::ReflectionQuality: return GetQualityText(PendingReflectionQuality);
    case ESettingsField::DynamicGlobalIlluminationMethod: return GetDynamicGlobalIlluminationMethodText(PendingDynamicGlobalIlluminationMethod);
    case ESettingsField::ReflectionMethod: return GetReflectionMethodText(PendingReflectionMethod);
    default: break;
    }
    return FText::FromString(TEXT("-"));
}

FText USettingsMenuWidget::GetQualityText(int32 Value) const
{
    switch (FMath::Clamp(Value, QualityMin, QualityMax))
    {
    case 0: return FText::FromString(TEXT("Low"));
    case 1: return FText::FromString(TEXT("Medium"));
    case 2: return FText::FromString(TEXT("High"));
    case 3: return FText::FromString(TEXT("Epic"));
    default: break;
    }
    return FText::AsNumber(Value);
}

FText USettingsMenuWidget::GetBoolText(bool bValue) const
{
    return FText::FromString(bValue ? TEXT("On") : TEXT("Off"));
}

FText USettingsMenuWidget::GetDynamicGlobalIlluminationMethodText(int32 Value) const
{
    switch (FMath::Clamp(Value, QualityMin, QualityMax))
    {
    case 0: return FText::FromString(TEXT("None"));
    case 1: return FText::FromString(TEXT("Lumen"));
    case 2: return FText::FromString(TEXT("Screen Space"));
    case 3: return FText::FromString(TEXT("Plugin"));
    default: break;
    }
    return FText::AsNumber(Value);
}

FText USettingsMenuWidget::GetReflectionMethodText(int32 Value) const
{
    switch (FMath::Clamp(Value, ReflectionMethodMin, ReflectionMethodMax))
    {
    case 0: return FText::FromString(TEXT("None"));
    case 1: return FText::FromString(TEXT("Lumen"));
    case 2: return FText::FromString(TEXT("Screen Space"));
    default: break;
    }
    return FText::AsNumber(Value);
}

void USettingsMenuWidget::CycleBloomIntensityFromUI() { CycleSettingValueFromUI(ESettingsField::BloomIntensity); }
void USettingsMenuWidget::CycleBloomThresholdFromUI() { CycleSettingValueFromUI(ESettingsField::BloomThreshold); }
void USettingsMenuWidget::CycleAmbientOcclusionIntensityFromUI() { CycleSettingValueFromUI(ESettingsField::AmbientOcclusionIntensity); }
void USettingsMenuWidget::CycleRayTracingFromUI() { CycleSettingValueFromUI(ESettingsField::RayTracing); }
void USettingsMenuWidget::CycleHeightFogFromUI() { CycleSettingValueFromUI(ESettingsField::HeightFog); }
void USettingsMenuWidget::CycleCloudFromUI() { CycleSettingValueFromUI(ESettingsField::Cloud); }
void USettingsMenuWidget::CycleShadowQualityFromUI() { CycleSettingValueFromUI(ESettingsField::ShadowQuality); }
void USettingsMenuWidget::CycleTextureQualityFromUI() { CycleSettingValueFromUI(ESettingsField::TextureQuality); }
void USettingsMenuWidget::CycleMaxTextureResolutionFromUI() { CycleSettingValueFromUI(ESettingsField::MaxTextureResolution); }
void USettingsMenuWidget::CycleViewDistanceQualityFromUI() { CycleSettingValueFromUI(ESettingsField::ViewDistanceQuality); }
void USettingsMenuWidget::CycleAntiAliasingQualityFromUI() { CycleSettingValueFromUI(ESettingsField::AntiAliasingQuality); }
void USettingsMenuWidget::CyclePostProcessingQualityFromUI() { CycleSettingValueFromUI(ESettingsField::PostProcessingQuality); }
void USettingsMenuWidget::CycleEffectsQualityFromUI() { CycleSettingValueFromUI(ESettingsField::EffectsQuality); }
void USettingsMenuWidget::CycleFoliageQualityFromUI() { CycleSettingValueFromUI(ESettingsField::FoliageQuality); }
void USettingsMenuWidget::CycleShadingQualityFromUI() { CycleSettingValueFromUI(ESettingsField::ShadingQuality); }
void USettingsMenuWidget::CycleGlobalIlluminationQualityFromUI() { CycleSettingValueFromUI(ESettingsField::GlobalIlluminationQuality); }
void USettingsMenuWidget::CycleReflectionQualityFromUI() { CycleSettingValueFromUI(ESettingsField::ReflectionQuality); }
void USettingsMenuWidget::CycleDynamicGlobalIlluminationMethodFromUI() { CycleSettingValueFromUI(ESettingsField::DynamicGlobalIlluminationMethod); }
void USettingsMenuWidget::CycleReflectionMethodFromUI() { CycleSettingValueFromUI(ESettingsField::ReflectionMethod); }
