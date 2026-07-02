// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "PauseMenuWidget.generated.h"

class UButton;
class UTextBlock;

/**
 * Blueprint-editable pause menu base widget.
 *
 * This class no longer creates a native fallback layout. Create the visual tree
 * in a WBP child and bind widgets named ContinueButton, SettingsButton, and ExitButton,
 * or call the BlueprintCallable functions from your own UI events.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UPauseMenuWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    virtual void NativeConstruct() override;

    UFUNCTION(BlueprintCallable, Category="Pause")
    void ContinueFromUI();

    UFUNCTION(BlueprintCallable, Category="Pause")
    void OpenSettingsFromUI();

    UFUNCTION(BlueprintCallable, Category="Pause")
    void ExitFromUI();

protected:
    UPROPERTY(BlueprintReadOnly, meta=(BindWidgetOptional), Category="Pause|Widgets")
    TObjectPtr<UTextBlock> TitleText;

    UPROPERTY(BlueprintReadOnly, meta=(BindWidgetOptional), Category="Pause|Widgets")
    TObjectPtr<UButton> ContinueButton;

    UPROPERTY(BlueprintReadOnly, meta=(BindWidgetOptional), Category="Pause|Widgets")
    TObjectPtr<UButton> SettingsButton;

    UPROPERTY(BlueprintReadOnly, meta=(BindWidgetOptional), Category="Pause|Widgets")
    TObjectPtr<UButton> ExitButton;

private:
    void CacheUserWidgetReferences();
    void BindButtonEvents();
};
