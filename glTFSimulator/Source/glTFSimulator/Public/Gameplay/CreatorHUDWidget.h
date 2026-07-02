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

/**
 * Creator HUD bridge class.
 *
 * This class no longer creates a native/UMG fallback layout. Create your own WBP
 * and bind its buttons to the BlueprintCallable functions below.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UCreatorHUDWidget : public UUserWidget
{
    GENERATED_BODY()

public:
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

    /** Lightly refreshes per-frame values such as center-cursor and status text. */
    virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
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

    /** Caches same-named text and panel widgets from the WBP only; it does not create widgets. */
    void CacheUserWidgetReferences();

private:
    /** Used for automatic text updates only when same-named widgets exist in the custom WBP. */
    UPROPERTY(Transient)
    TObjectPtr<UUniformGridPanel> UserToolbarGrid;

    UPROPERTY(Transient)
    TObjectPtr<UScrollBox> UserItemListScrollBox;

    UPROPERTY(Transient)
    TObjectPtr<UBorder> UserItemListPanel;

    UPROPERTY(Transient)
    TObjectPtr<UTextBlock> UserStatusTextBlock;

    UPROPERTY(Transient)
    TObjectPtr<UTextBlock> UserPlacementInfoTextBlock;

    UPROPERTY(Transient)
    TObjectPtr<UTextBlock> UserMessageTextBlock;

    /** GameManager currently connected to this HUD. */
    UPROPERTY(Transient)
    mutable TObjectPtr<UGameManagerSubSystem> CachedGameManager;

    /** Tracks whether events are already bound so duplicate bindings are avoided. */
    bool bGameplayEventsBound = false;
};
