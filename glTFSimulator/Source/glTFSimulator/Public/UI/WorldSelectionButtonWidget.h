// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "WorldSelectionButtonWidget.generated.h"

class UButton;
class UTextBlock;
class UWorldSelectionWidget;

/**
 * One generated entry in the world-selection list.
 *
 * A Blueprint button widget can be reparented to this class. Assign its Level button and Name
 * text block through same-named widget variables or by calling the setter functions below.
 * This class does not search the widget tree by name and does not create fallback widgets.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UWorldSelectionButtonWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    /** Assigns the button used to open the world. Useful when the WBP cannot use BindWidget variables. */
    UFUNCTION(BlueprintCallable, Category="Start World|World Button")
    void SetLevelButton(UButton* InLevelButton);

    /** Assigns the text block that displays the world name. Useful when the WBP cannot use BindWidget variables. */
    UFUNCTION(BlueprintCallable, Category="Start World|World Button")
    void SetNameTextBlock(UTextBlock* InNameTextBlock);

    /** Assigns the world folder key and display text used by this generated button. */
    UFUNCTION(BlueprintCallable, Category="Start World|World Button")
    void SetupWorldButton(UWorldSelectionWidget* InOwner, const FString& InWorldFolderName, const FString& InDisplayName);

    /** Opens the assigned world through the owning world-selection widget. */
    UFUNCTION(BlueprintCallable, Category="Start World|World Button")
    void OpenAssignedWorld();

    UFUNCTION(BlueprintPure, Category="Start World|World Button")
    FString GetWorldFolderName() const { return WorldFolderName; }

    UFUNCTION(BlueprintPure, Category="Start World|World Button")
    FString GetWorldDisplayName() const { return WorldDisplayName; }

protected:
    /** Button from WBP_WorldButton. Name the widget variable Level, or set it with SetLevelButton(). */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Start World|Widgets")
    TObjectPtr<UButton> Level;

    /** TextBlock from WBP_WorldButton. Name the widget variable Name, or set it with SetNameTextBlock(). */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Start World|Widgets")
    TObjectPtr<UTextBlock> Name;

private:
    void BindClickDelegate();
    void RefreshDisplayName();

    UPROPERTY(Transient)
    TWeakObjectPtr<UWorldSelectionWidget> OwningWorldSelectionWidget;

    UPROPERTY(Transient)
    FString WorldFolderName;

    UPROPERTY(Transient)
    FString WorldDisplayName;
};
