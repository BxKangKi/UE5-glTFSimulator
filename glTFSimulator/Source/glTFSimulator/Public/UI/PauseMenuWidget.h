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
 * Create the visual tree in a WBP child and pass widget references explicitly from the
 * WBP Construct event. This class does not use automatic widget-name binding or widget-tree lookup.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UPauseMenuWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    UFUNCTION(BlueprintCallable, Category="Pause|Widgets")
    void SetTitleText(UTextBlock* InTitleText);

    UFUNCTION(BlueprintCallable, Category="Pause|Widgets")
    void SetContinueButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Pause|Widgets")
    void SetSettingsButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Pause|Widgets")
    void SetExitButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Pause")
    void ContinueFromUI();

    UFUNCTION(BlueprintCallable, Category="Pause")
    void OpenSettingsFromUI();

    UFUNCTION(BlueprintCallable, Category="Pause")
    void ExitFromUI();

private:
    void BindButtonEvents();
    void UnbindButtonEvents();

    TWeakObjectPtr<UTextBlock> AssignedTitleText;
    TWeakObjectPtr<UButton> AssignedContinueButton;
    TWeakObjectPtr<UButton> AssignedSettingsButton;
    TWeakObjectPtr<UButton> AssignedExitButton;
};
