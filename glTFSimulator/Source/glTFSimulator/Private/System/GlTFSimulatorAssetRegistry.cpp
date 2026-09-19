// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/GlTFSimulatorAssetRegistry.h"

UGlTFSimulatorAssetRegistry::UGlTFSimulatorAssetRegistry(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // Shared UI component classes are centralized here. These native defaults keep existing
    // projects working after the per-widget class overrides were removed; a Blueprint registry
    // subclass can still override any path in one place. No widget instance is loaded by this CDO.
    ProjectSelectionWidgetClass = TSoftClassPtr<UProjectSelectionWidget>(
        FSoftObjectPath(TEXT("/Game/Blueprints/UI/WBP_BuildSelection.WBP_BuildSelection_C")));
    BuildStatusWidgetClass = TSoftClassPtr<UBuildStatusWidget>(
        FSoftObjectPath(TEXT("/Game/Blueprints/UI/WBP_BuildStatus.WBP_BuildStatus_C")));
    SelectionEntryWidgetClass = TSoftClassPtr<UMenuButtonWidget>(
        FSoftObjectPath(TEXT("/Game/Blueprints/UI/Components/WBP_Button.WBP_Button_C")));
    BuildStatusConfirmWidgetClass = TSoftClassPtr<UMenuButtonWidget>(
        FSoftObjectPath(TEXT("/Game/Blueprints/UI/Components/WBP_Button.WBP_Button_C")));
    BooleanSettingWidgetClass = TSoftClassPtr<UBooleanSettingWidget>(
        FSoftObjectPath(TEXT("/Game/Blueprints/UI/Components/WBP_Settings_Toggle.WBP_Settings_Toggle_C")));
    FloatSettingWidgetClass = TSoftClassPtr<UFloatSettingWidget>(
        FSoftObjectPath(TEXT("/Game/Blueprints/UI/Components/WBP_Settings_Slider.WBP_Settings_Slider_C")));
    EnumSettingWidgetClass = TSoftClassPtr<UEnumSettingWidget>(
        FSoftObjectPath(TEXT("/Game/Blueprints/UI/Components/WBP_Settings_Dropdown.WBP_Settings_Dropdown_C")));
}
