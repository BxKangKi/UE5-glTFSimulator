// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/CreatorHUDWidget.h"

#include "Components/Border.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "System/GameUpdateSubSystem.h"

static FString MakeKindLabel(EToolbarItemKind Kind)
{
    switch (Kind)
    {
    case EToolbarItemKind::Prefab:
        return TEXT("Prefab");
    case EToolbarItemKind::Weapon:
        return TEXT("Weapon");
    case EToolbarItemKind::Vehicle:
        return TEXT("Vehicle");
    default:
        return TEXT("None");
    }
}

static void SetTextIfChanged(UTextBlock* TextBlock, const FText& NewText)
{
    if (IsValid(TextBlock) && !TextBlock->GetText().EqualTo(NewText))
    {
        TextBlock->SetText(NewText);
    }
}

void UCreatorHUDWidget::SetToolbarGrid(UUniformGridPanel* InToolbarGrid)
{
    AssignedToolbarGrid = InToolbarGrid;
    RefreshToolbar();
}

void UCreatorHUDWidget::SetItemListScrollBox(UScrollBox* InItemListScrollBox)
{
    AssignedItemListScrollBox = InItemListScrollBox;
    RefreshItemList();
}

void UCreatorHUDWidget::SetItemListPanel(UBorder* InItemListPanel)
{
    AssignedItemListPanel = InItemListPanel;
    RefreshItemList();
}

void UCreatorHUDWidget::SetStatusText(UTextBlock* InStatusText)
{
    AssignedStatusText = InStatusText;
    RefreshStatus();
}

void UCreatorHUDWidget::SetPlacementText(UTextBlock* InPlacementText)
{
    AssignedPlacementText = InPlacementText;
    RefreshStatus();
}

void UCreatorHUDWidget::SetMessageText(UTextBlock* InMessageText)
{
    AssignedMessageText = InMessageText;
    RefreshStatus();
}

void UCreatorHUDWidget::NativeConstruct()
{
    Super::NativeConstruct();

    RefreshManagerReference();
    BindManagerEvents();

    if (GameUpdateTickHandle == INDEX_NONE)
    {
        if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
        {
            GameUpdateTickHandle = GameUpdate->RegisterUpdate(
                this,
                [WeakThis = TWeakObjectPtr<UCreatorHUDWidget>(this)](const float DeltaSeconds)
                {
                    if (UCreatorHUDWidget* StrongThis = WeakThis.Get())
                    {
                        StrongThis->UpdateFromGameUpdate(DeltaSeconds);
                    }
                },
                60);
        }
    }

    RefreshStatus();
    RefreshToolbar();
    RefreshItemList();
}

void UCreatorHUDWidget::NativeDestruct()
{
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
    }
    GameUpdateTickHandle = INDEX_NONE;

    UnbindManagerEvents();
    Super::NativeDestruct();
}

void UCreatorHUDWidget::UpdateFromGameUpdate(float DeltaSeconds)
{
    if (!IsValid(AssignedPlacementText.Get()))
    {
        return;
    }

    // Only crosshair coordinates can change without a manager event. Status, message, and panel
    // state are refreshed by their delegates and no longer rebuild FText every frame.
    SetTextIfChanged(AssignedPlacementText.Get(), GetPlacementInfoText());
}

void UCreatorHUDWidget::RefreshManagerReference()
{
    UnbindManagerEvents();
    CachedGameManager = UGameManagerSubSystem::FindGameManager(this);
    BindManagerEvents();
}

void UCreatorHUDWidget::RefreshToolbar()
{
    // No native fallback buttons are generated here anymore.
    // Keep this as a Blueprint-friendly refresh hook for user-authored WBP graphs.
}

void UCreatorHUDWidget::RefreshItemList()
{
    // No native fallback item buttons are generated here anymore.
    // User-authored WBP graphs should build their own list and call SelectAvailableItemFromUI().

    if (IsValid(AssignedItemListPanel.Get()))
    {
        const UGameManagerSubSystem* Manager = GetGameManager();
        const ESlateVisibility NewVisibility =
            IsValid(Manager) && Manager->IsItemListWindowOpen()
                ? ESlateVisibility::Visible
                : ESlateVisibility::Collapsed;
        if (AssignedItemListPanel.Get()->GetVisibility() != NewVisibility)
        {
            AssignedItemListPanel.Get()->SetVisibility(NewVisibility);
        }
    }
}

void UCreatorHUDWidget::RefreshStatus()
{
    if (IsValid(AssignedStatusText.Get()))
    {
        SetTextIfChanged(AssignedStatusText.Get(), GetStatusText());
    }
    if (IsValid(AssignedPlacementText.Get()))
    {
        SetTextIfChanged(AssignedPlacementText.Get(), GetPlacementInfoText());
    }
    if (IsValid(AssignedMessageText.Get()))
    {
        SetTextIfChanged(AssignedMessageText.Get(), GetMessageText());
    }
    if (IsValid(AssignedItemListPanel.Get()))
    {
        const UGameManagerSubSystem* Manager = GetGameManager();
        const ESlateVisibility NewVisibility =
            IsValid(Manager) && Manager->IsItemListWindowOpen()
                ? ESlateVisibility::Visible
                : ESlateVisibility::Collapsed;
        if (AssignedItemListPanel.Get()->GetVisibility() != NewVisibility)
        {
            AssignedItemListPanel.Get()->SetVisibility(NewVisibility);
        }
    }
}

void UCreatorHUDWidget::SelectToolbarSlotFromUI(int32 SlotIndex)
{
    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->SelectToolbarSlot(SlotIndex);
    }
}

void UCreatorHUDWidget::SelectAvailableItemFromUI(int32 AvailableItemIndex)
{
    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->SelectAvailableItemForCurrentToolbarSlot(AvailableItemIndex, true);
    }
}

void UCreatorHUDWidget::ToggleItemListFromUI()
{
    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->ToggleItemListWindow();
    }
}

void UCreatorHUDWidget::SaveSceneFromUI()
{
    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->SaveScene();
    }
}

void UCreatorHUDWidget::BakeWorldDataFromUI()
{
    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->BakeWorldData();
    }
}

void UCreatorHUDWidget::ToggleSnapFromUI()
{
    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->ToggleSnap();
    }
}

void UCreatorHUDWidget::HandleStateChanged()
{
    RefreshStatus();
}

void UCreatorHUDWidget::HandleMessageChanged(const FString& Message)
{
    RefreshStatus();
}

void UCreatorHUDWidget::HandleToolbarChanged()
{
    RefreshToolbar();
    RefreshStatus();
}

void UCreatorHUDWidget::HandleItemListWindowChanged(bool bOpen)
{
    RefreshItemList();
    RefreshStatus();
}

FText UCreatorHUDWidget::GetStatusText() const
{
    const UGameManagerSubSystem* Manager = GetGameManager();
    if (!IsValid(Manager))
    {
        return FText::FromString(TEXT("GameManager 없음"));
    }

    const FToolbarItem SelectedItem = Manager->GetSelectedToolbarItem();
    const FString ModeText = Manager->GetPlayMode() == EPlayMode::Creator
        ? TEXT("Creator")
        : TEXT("RealLife");

    return FText::FromString(FString::Printf(
        TEXT("%s MODE\nSlot %d / %d\nItem: %s\nKind: %s\nTool: %d"),
        *ModeText,
        Manager->GetSelectedToolbarSlotIndex() + 1,
        Manager->GetToolbarSlotCount(),
        *SelectedItem.DisplayName,
        *MakeKindLabel(SelectedItem.Kind),
        static_cast<int32>(Manager->GetCurrentToolMode())));
}

FText UCreatorHUDWidget::GetPlacementInfoText() const
{
    const UGameManagerSubSystem* Manager = GetGameManager();
    if (!IsValid(Manager))
    {
        return FText::FromString(TEXT("Placement: NONE"));
    }

    const FVector Location = Manager->GetCurrentCrosshairWorldLocation();
    const FString PlacementMode = Manager->HasCrosshairPlacementLocation()
        ? (Manager->IsCrosshairFreeSpacePlacement() ? TEXT("AIR") : TEXT("SURFACE"))
        : TEXT("NONE");
    const FString SnapText = Manager->IsSnapEnabled() ? TEXT("ON") : TEXT("OFF");

    return FText::FromString(FString::Printf(
        TEXT("X %.0f  Y %.0f  Z %.0f\nPlacement: %s\nSnap: %s (%.0f cm)"),
        Location.X,
        Location.Y,
        Location.Z,
        *PlacementMode,
        *SnapText,
        Manager->GetGridSize()));
}

FText UCreatorHUDWidget::GetMessageText() const
{
    const UGameManagerSubSystem* Manager = GetGameManager();
    if (!IsValid(Manager))
    {
        return FText::GetEmpty();
    }

    return FText::FromString(Manager->GetLastMessage());
}

UGameManagerSubSystem* UCreatorHUDWidget::GetGameManager() const
{
    if (IsValid(CachedGameManager))
    {
        return CachedGameManager.Get();
    }

    CachedGameManager = UGameManagerSubSystem::FindGameManager(this);
    return CachedGameManager.Get();
}

void UCreatorHUDWidget::BindManagerEvents()
{
    if (bGameplayEventsBound)
    {
        return;
    }

    UGameManagerSubSystem* Manager = GetGameManager();
    if (!IsValid(Manager))
    {
        return;
    }

    Manager->OnStateChanged.AddDynamic(this, &UCreatorHUDWidget::HandleStateChanged);
    Manager->OnMessageChanged.AddDynamic(this, &UCreatorHUDWidget::HandleMessageChanged);
    Manager->OnToolbarChanged.AddDynamic(this, &UCreatorHUDWidget::HandleToolbarChanged);
    Manager->OnItemListWindowChanged.AddDynamic(this, &UCreatorHUDWidget::HandleItemListWindowChanged);
    bGameplayEventsBound = true;
}

void UCreatorHUDWidget::UnbindManagerEvents()
{
    if (!bGameplayEventsBound)
    {
        return;
    }

    if (IsValid(CachedGameManager))
    {
        CachedGameManager->OnStateChanged.RemoveDynamic(this, &UCreatorHUDWidget::HandleStateChanged);
        CachedGameManager->OnMessageChanged.RemoveDynamic(this, &UCreatorHUDWidget::HandleMessageChanged);
        CachedGameManager->OnToolbarChanged.RemoveDynamic(this, &UCreatorHUDWidget::HandleToolbarChanged);
        CachedGameManager->OnItemListWindowChanged.RemoveDynamic(this, &UCreatorHUDWidget::HandleItemListWindowChanged);
    }

    bGameplayEventsBound = false;
}
