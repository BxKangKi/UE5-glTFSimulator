// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file MenuButtonWidget.h
 * Reusable Blueprint-facing button widget base used by generated menu entries.
 */

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "MenuButtonWidget.generated.h"

class UButton;
class UTextBlock;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMenuButtonClicked, const FString&, ButtonKey);

/**
 * Minimal native contract for a fully Blueprint-authored button WBP.
 *
 * The Blueprint owns all appearance, layout, animation, iconography, and hover/pressed styling.
 * Native code only supplies a stable key/label and receives a click notification.
 *
 * Typical Blueprint setup:
 * 1. Create WBP_MenuButton with this class as its parent.
 * 2. Place any UMG layout you want.
 * 3. In Construct, call SetButton() for the clickable UButton and optionally SetLabelTextBlock().
 * 4. Selection/BuildStatus widgets can then receive the WBP class instead of style parameters.
 *
 * If the WBP does not use a native UButton, call TriggerClick() from any Blueprint event instead.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UMenuButtonWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    virtual void NativeDestruct() override;

    UPROPERTY(BlueprintAssignable, Category="Menu Button")
    FMenuButtonClicked OnButtonClicked;

    /** Assigns the actual clickable UButton contained by this WBP. */
    UFUNCTION(BlueprintCallable, Category="Menu Button|Widgets")
    void SetButton(UButton* InButton);

    /** Optional label. When assigned, ConfigureButton() writes DisplayLabel into it automatically. */
    UFUNCTION(BlueprintCallable, Category="Menu Button|Widgets")
    void SetLabelTextBlock(UTextBlock* InTextBlock);

    /** Supplies the stable action key and the text intended for this WBP instance. */
    UFUNCTION(BlueprintCallable, Category="Menu Button")
    void ConfigureButton(const FString& InButtonKey, const FString& InDisplayLabel);

    /** Allows a custom Blueprint interaction to emit the same click event without a UButton. */
    UFUNCTION(BlueprintCallable, Category="Menu Button")
    void TriggerClick();

    UFUNCTION(BlueprintPure, Category="Menu Button")
    FString GetButtonKey() const { return ButtonKey; }

    UFUNCTION(BlueprintPure, Category="Menu Button")
    FString GetDisplayLabel() const { return DisplayLabel; }

    /** Called after ConfigureButton so the WBP can update custom text/icons/layout. */
    UFUNCTION(BlueprintImplementableEvent, Category="Menu Button", meta=(DisplayName="On Button Configured"))
    void BP_OnButtonConfigured(const FString& InButtonKey, const FString& InDisplayLabel);

private:
    UFUNCTION()
    void HandleNativeButtonClicked();

    TWeakObjectPtr<UButton> AssignedButton;
    TWeakObjectPtr<UTextBlock> AssignedLabel;

    FString ButtonKey;
    FString DisplayLabel;
};
