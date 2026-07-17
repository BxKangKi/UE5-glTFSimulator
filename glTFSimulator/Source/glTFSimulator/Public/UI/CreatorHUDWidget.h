// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "System/GameManagerSubSystem.h"
#include "CreatorHUDWidget.generated.h"

class UBorder;
class UScrollBox;
class UTextBlock;
class UUniformGridPanel;
class UGameUpdateSubSystem;

/**
 * Creator HUD bridge class.
 *
 * Create the visual tree in a WBP child and pass widget references explicitly from the
 * WBP Construct event. This class does not use automatic widget-name binding or widget-tree lookup.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UCreatorHUDWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    /** Explicitly assigns the toolbar grid owned by the WBP. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Widgets")
    void SetToolbarGrid(UUniformGridPanel* InToolbarGrid);

    /** Explicitly assigns the item-list scroll box owned by the WBP. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Widgets")
    void SetItemListScrollBox(UScrollBox* InItemListScrollBox);

    /** Explicitly assigns the item-list panel owned by the WBP. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Widgets")
    void SetItemListPanel(UBorder* InItemListPanel);

    /** Explicitly assigns the status text block owned by the WBP. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Widgets")
    void SetStatusText(UTextBlock* InStatusText);

    /** Explicitly assigns the placement-info text block owned by the WBP. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Widgets")
    void SetPlacementText(UTextBlock* InPlacementText);

    /** Explicitly assigns the message text block owned by the WBP. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Widgets")
    void SetMessageText(UTextBlock* InMessageText);

    /** Re-finds the GameManager used by this HUD and refreshes all displayed values. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD")
    void RefreshManagerReference();

    /** Refresh entry point for custom WBP toolbar widgets. No fallback buttons are generated here. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD")
    void RefreshToolbar();

    /** Refresh entry point for custom WBP item-list widgets. No fallback buttons are generated here. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD")
    void RefreshItemList();

    /** Immediately refreshes status, coordinate, and snap text. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD")
    void RefreshStatus();

    /** Allows external Blueprints to select a specific toolbar slot manually. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Toolbar")
    void SelectToolbarSlotFromUI(int32 SlotIndex);

    /** Allows external Blueprints to select an item from the full item list manually. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Inventory")
    void SelectAvailableItemFromUI(int32 AvailableItemIndex);

    /** Called by UI buttons to open or close the full item-list window. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Inventory")
    void ToggleItemListFromUI();

    /** Called by UI buttons to finish the currently edited mesh. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Editing")
    void FinishEditFromUI();

    /** Called by UI buttons to cancel the currently edited mesh. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Editing")
    void CancelEditFromUI();

    /** Called by UI buttons to save the current scene. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Save")
    void SaveSceneFromUI();

    /** Called by UI buttons to toggle grid snapping. */
    UFUNCTION(BlueprintCallable, Category="Creator HUD|Placement")
    void ToggleSnapFromUI();

protected:
    /** Binds manager events after the widget is constructed. */
    virtual void NativeConstruct() override;

    /** Unbinds manager events when the widget is removed. */
    virtual void NativeDestruct() override;

private:
    int32 GameUpdateTickHandle = INDEX_NONE;
    void UpdateFromGameUpdate(float DeltaSeconds);

    /** Bridges GameManager state changes to HUD refreshes. */
    UFUNCTION()
    void HandleStateChanged();

    /** Bridges GameManager message changes to HUD refreshes. */
    UFUNCTION()
    void HandleMessageChanged(const FString& Message);

    /** Bridges GameManager toolbar changes to HUD refreshes. */
    UFUNCTION()
    void HandleToolbarChanged();

    /** Bridges GameManager item-list window changes to HUD refreshes. */
    UFUNCTION()
    void HandleItemListWindowChanged(bool bOpen);

    /** Returns the status panel text. */
    FText GetStatusText() const;

    /** Returns the coordinate, snap, and validity panel text. */
    FText GetPlacementInfoText() const;

    /** Returns the last gameplay message text. */
    FText GetMessageText() const;

    /** Finds and caches the manager pointer when it has not been resolved yet. */
    UGameManagerSubSystem* GetGameManager() const;

    /** Binds manager events without duplicate bindings. */
    void BindManagerEvents();

    /** Safely removes manager event bindings. */
    void UnbindManagerEvents();

    TWeakObjectPtr<UUniformGridPanel> AssignedToolbarGrid;
    TWeakObjectPtr<UScrollBox> AssignedItemListScrollBox;
    TWeakObjectPtr<UBorder> AssignedItemListPanel;
    TWeakObjectPtr<UTextBlock> AssignedStatusText;
    TWeakObjectPtr<UTextBlock> AssignedPlacementText;
    TWeakObjectPtr<UTextBlock> AssignedMessageText;

    /** GameManager currently connected to this HUD. */
    UPROPERTY(Transient)
    mutable TObjectPtr<UGameManagerSubSystem> CachedGameManager;

    /** Tracks whether events are already bound so duplicate bindings are avoided. */
    bool bGameplayEventsBound = false;
};
