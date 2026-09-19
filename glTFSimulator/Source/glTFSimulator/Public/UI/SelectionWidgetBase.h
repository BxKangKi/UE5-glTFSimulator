// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file SelectionWidgetBase.h
 * Shared generated-selection behavior used by Project and World selection menus.
 */

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "SelectionWidgetBase.generated.h"

class UMenuButtonWidget;
class UPanelWidget;
class UWidget;

/**
 * Base class for menus that generate selectable entries at runtime.
 *
 * Button appearance is intentionally not defined here. Each entry is created from a Blueprint
 * widget class derived from UMenuButtonWidget, allowing the complete button design to live in WBP.
 */
UCLASS(Abstract, Blueprintable, BlueprintType)
class GLTFSIMULATOR_API USelectionWidgetBase : public UUserWidget
{
    GENERATED_BODY()

public:
    virtual void NativeDestruct() override;

    /** Internal click receiver used by generated UMenuButtonWidget instances. */
    UFUNCTION()
    void HandleSelectionButtonClicked(const FString& SelectionKey);

protected:
    /** Assigns the panel that owns generated entries. Existing generated entries are removed. */
    void SetSelectionListPanel(UPanelWidget* InPanel);

    UPanelWidget* GetSelectionListPanel() const { return SelectionListPanel.Get(); }

    /** Creates, configures, binds, and adds one WBP entry to the assigned panel. */
    UWidget* AddGeneratedSelectionEntry(const FString& SelectionKey, const FString& DisplayName);

    /** Removes generated entries without touching Blueprint-authored children. */
    void ClearGeneratedSelectionEntries();

    /** Domain action implemented by derived selection widgets. */
    virtual void OnSelectionEntryActivated(const FString& SelectionKey);

private:
    UClass* ResolveSelectionEntryWidgetClass();

    TWeakObjectPtr<UPanelWidget> SelectionListPanel;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UMenuButtonWidget>> GeneratedSelectionEntries;

    UPROPERTY(Transient)
    TObjectPtr<UClass> ResolvedSelectionEntryWidgetClass;
};
