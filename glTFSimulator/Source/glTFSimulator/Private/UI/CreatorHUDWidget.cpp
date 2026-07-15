// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/CreatorHUDWidget.h"

#include "Components/Border.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Model/EditableMeshActor.h"
#include "System/GameUpdateSubSystem.h"

static FString MakeKindLabel(EToolbarItemKind Kind)
{
        switch (Kind)
        {
        case EToolbarItemKind::CreateObject:
            return TEXT("Object");
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
                [this](const float DeltaSeconds)
                {
                    UpdateFromGameUpdate(DeltaSeconds);
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
    if (!IsValid(CreatorHUD_StatusText) && !IsValid(CreatorHUD_PlacementText) &&
        !IsValid(CreatorHUD_MessageText) && !IsValid(CreatorHUD_ItemListPanel))
    {
        return;
    }

    RefreshStatus();
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

    if (IsValid(CreatorHUD_ItemListPanel))
    {
        const UGameManagerSubSystem* Manager = GetGameManager();
        CreatorHUD_ItemListPanel->SetVisibility(IsValid(Manager) && Manager->IsItemListWindowOpen() ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    }
}

void UCreatorHUDWidget::RefreshStatus()
{
    if (IsValid(CreatorHUD_StatusText))
    {
        CreatorHUD_StatusText->SetText(GetStatusText());
    }
    if (IsValid(CreatorHUD_PlacementText))
    {
        CreatorHUD_PlacementText->SetText(GetPlacementInfoText());
    }
    if (IsValid(CreatorHUD_MessageText))
    {
        CreatorHUD_MessageText->SetText(GetMessageText());
    }
    if (IsValid(CreatorHUD_ItemListPanel))
    {
        const UGameManagerSubSystem* Manager = GetGameManager();
        CreatorHUD_ItemListPanel->SetVisibility(IsValid(Manager) && Manager->IsItemListWindowOpen() ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
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

void UCreatorHUDWidget::FinishEditFromUI()
{
    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->FinishCurrentEditableMesh();
    }
}

void UCreatorHUDWidget::CancelEditFromUI()
{
    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->CancelCurrentEditableMesh(true);
    }
}

void UCreatorHUDWidget::SaveSceneFromUI()
{
    if (UGameManagerSubSystem* Manager = GetGameManager())
    {
        Manager->SaveScene();
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
    const FString ModeText = Manager->GetPlayMode() == EPlayMode::Creator ? TEXT("Creator") : TEXT("RealLife");
    const FString EditText = Manager->IsEditingGeneratedMesh()
        ? FString::Printf(TEXT("Editing: V%d / T%d / %s"), Manager->GetCurrentEditableMeshVertexCount(), Manager->GetCurrentEditableMeshTriangleCount(), Manager->IsCurrentEditableMeshTopologyValid() ? TEXT("VALID") : TEXT("INVALID"))
        : TEXT("Editing: None");

    const FString Text = FString::Printf(
        TEXT("%s MODE\nSlot %d / %d\nItem: %s\nKind: %s\nTool: %d\n%s"),
        *ModeText,
        Manager->GetSelectedToolbarSlotIndex() + 1,
        Manager->GetToolbarSlotCount(),
        *SelectedItem.DisplayName,
        *MakeKindLabel(SelectedItem.Kind),
        static_cast<int32>(Manager->GetCurrentToolMode()),
        *EditText);

    return FText::FromString(Text);
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
    const FString ValidText = Manager->IsEditingGeneratedMesh()
        ? (Manager->IsCurrentEditableMeshTopologyValid() ? TEXT("VALID") : TEXT("INVALID"))
        : TEXT("READY");

    const FString Text = FString::Printf(
        TEXT("X %.0f  Y %.0f  Z %.0f\nPlacement: %s\nSnap: %s (%.0f cm)\nTopology: %s\nHighlighted Vertex: %d"),
        Location.X,
        Location.Y,
        Location.Z,
        *PlacementMode,
        *SnapText,
        Manager->GetGridSize(),
        *ValidText,
        Manager->GetHighlightedEditableVertexIndex());

    return FText::FromString(Text);
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
