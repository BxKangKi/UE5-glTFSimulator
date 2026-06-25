// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Runtime/RuntimeCreatorHUDWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Runtime/RuntimeEditableMeshActor.h"

static FString MakeKindLabel(ERuntimeToolbarItemKind Kind)
{
        switch (Kind)
        {
        case ERuntimeToolbarItemKind::CreateObject:
            return TEXT("Object");
        case ERuntimeToolbarItemKind::Prefab:
            return TEXT("Prefab");
        case ERuntimeToolbarItemKind::Weapon:
            return TEXT("Weapon");
        case ERuntimeToolbarItemKind::Vehicle:
            return TEXT("Vehicle");
        default:
            return TEXT("None");
        }
}

void URuntimeCreatorHUDWidget::NativeConstruct()
{
    Super::NativeConstruct();

    CacheUserWidgetReferences();
    RefreshRuntimeManagerReference();
    BindRuntimeManagerEvents();

    RefreshStatus();
    RefreshToolbar();
    RefreshItemList();
}

void URuntimeCreatorHUDWidget::NativeDestruct()
{
    UnbindRuntimeManagerEvents();
    Super::NativeDestruct();
}

void URuntimeCreatorHUDWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
    Super::NativeTick(MyGeometry, InDeltaTime);

    if (!IsValid(UserStatusTextBlock) && !IsValid(UserPlacementInfoTextBlock) &&
        !IsValid(UserMessageTextBlock) && !IsValid(UserItemListPanel))
    {
        return;
    }

    RefreshStatus();
}

void URuntimeCreatorHUDWidget::CacheUserWidgetReferences()
{
    if (!WidgetTree)
    {
        return;
    }

    UserToolbarGrid = Cast<UUniformGridPanel>(WidgetTree->FindWidget(TEXT("RuntimeCreatorHUD_ToolbarGrid")));
    UserItemListScrollBox = Cast<UScrollBox>(WidgetTree->FindWidget(TEXT("RuntimeCreatorHUD_ItemListScrollBox")));
    UserItemListPanel = Cast<UBorder>(WidgetTree->FindWidget(TEXT("RuntimeCreatorHUD_ItemListPanel")));
    UserStatusTextBlock = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("RuntimeCreatorHUD_StatusText")));
    UserPlacementInfoTextBlock = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("RuntimeCreatorHUD_PlacementText")));
    UserMessageTextBlock = Cast<UTextBlock>(WidgetTree->FindWidget(TEXT("RuntimeCreatorHUD_MessageText")));
}

void URuntimeCreatorHUDWidget::RefreshRuntimeManagerReference()
{
    UnbindRuntimeManagerEvents();
    CachedRuntimeManager = ARuntimeGameplayManager::FindRuntimeGameplayManager(this);
    BindRuntimeManagerEvents();
}

void URuntimeCreatorHUDWidget::RefreshToolbar()
{
    // No native fallback buttons are generated here anymore.
    // Keep this as a Blueprint-friendly refresh hook for user-authored WBP graphs.
    CacheUserWidgetReferences();
}

void URuntimeCreatorHUDWidget::RefreshItemList()
{
    // No native fallback item buttons are generated here anymore.
    // User-authored WBP graphs should build their own list and call SelectAvailableItemFromUI().
    CacheUserWidgetReferences();

    if (IsValid(UserItemListPanel))
    {
        const ARuntimeGameplayManager* Manager = GetRuntimeManager();
        UserItemListPanel->SetVisibility(IsValid(Manager) && Manager->IsItemListWindowOpen() ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    }
}

void URuntimeCreatorHUDWidget::RefreshStatus()
{
    if (!IsValid(UserStatusTextBlock) && !IsValid(UserPlacementInfoTextBlock) &&
        !IsValid(UserMessageTextBlock) && !IsValid(UserItemListPanel))
    {
        CacheUserWidgetReferences();
    }

    if (IsValid(UserStatusTextBlock))
    {
        UserStatusTextBlock->SetText(GetStatusText());
    }
    if (IsValid(UserPlacementInfoTextBlock))
    {
        UserPlacementInfoTextBlock->SetText(GetPlacementInfoText());
    }
    if (IsValid(UserMessageTextBlock))
    {
        UserMessageTextBlock->SetText(GetMessageText());
    }
    if (IsValid(UserItemListPanel))
    {
        const ARuntimeGameplayManager* Manager = GetRuntimeManager();
        UserItemListPanel->SetVisibility(IsValid(Manager) && Manager->IsItemListWindowOpen() ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    }
}

void URuntimeCreatorHUDWidget::SelectToolbarSlotFromUI(int32 SlotIndex)
{
    if (ARuntimeGameplayManager* Manager = GetRuntimeManager())
    {
        Manager->SelectToolbarSlot(SlotIndex);
    }
}

void URuntimeCreatorHUDWidget::SelectAvailableItemFromUI(int32 AvailableItemIndex)
{
    if (ARuntimeGameplayManager* Manager = GetRuntimeManager())
    {
        Manager->SelectAvailableItemForCurrentToolbarSlot(AvailableItemIndex, true);
    }
}

void URuntimeCreatorHUDWidget::ToggleItemListFromUI()
{
    if (ARuntimeGameplayManager* Manager = GetRuntimeManager())
    {
        Manager->ToggleItemListWindow();
    }
}

void URuntimeCreatorHUDWidget::FinishEditFromUI()
{
    if (ARuntimeGameplayManager* Manager = GetRuntimeManager())
    {
        Manager->FinishCurrentEditableMesh();
    }
}

void URuntimeCreatorHUDWidget::CancelEditFromUI()
{
    if (ARuntimeGameplayManager* Manager = GetRuntimeManager())
    {
        Manager->CancelCurrentEditableMesh(true);
    }
}

void URuntimeCreatorHUDWidget::SaveRuntimeSceneFromUI()
{
    if (ARuntimeGameplayManager* Manager = GetRuntimeManager())
    {
        Manager->SaveRuntimeScene();
    }
}

void URuntimeCreatorHUDWidget::ToggleSnapFromUI()
{
    if (ARuntimeGameplayManager* Manager = GetRuntimeManager())
    {
        Manager->ToggleSnap();
    }
}

void URuntimeCreatorHUDWidget::HandleRuntimeStateChanged()
{
    RefreshStatus();
}

void URuntimeCreatorHUDWidget::HandleRuntimeMessageChanged(const FString& Message)
{
    RefreshStatus();
}

void URuntimeCreatorHUDWidget::HandleRuntimeToolbarChanged()
{
    RefreshToolbar();
    RefreshStatus();
}

void URuntimeCreatorHUDWidget::HandleRuntimeItemListWindowChanged(bool bOpen)
{
    RefreshItemList();
    RefreshStatus();
}

FText URuntimeCreatorHUDWidget::GetStatusText() const
{
    const ARuntimeGameplayManager* Manager = GetRuntimeManager();
    if (!IsValid(Manager))
    {
        return FText::FromString(TEXT("RuntimeGameplayManager 없음"));
    }

    const FRuntimeToolbarItem SelectedItem = Manager->GetSelectedToolbarItem();
    const FString ModeText = Manager->GetRuntimePlayMode() == ERuntimePlayMode::Creator ? TEXT("Creator") : TEXT("RealLife");
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

FText URuntimeCreatorHUDWidget::GetPlacementInfoText() const
{
    const ARuntimeGameplayManager* Manager = GetRuntimeManager();
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

FText URuntimeCreatorHUDWidget::GetMessageText() const
{
    const ARuntimeGameplayManager* Manager = GetRuntimeManager();
    if (!IsValid(Manager))
    {
        return FText::GetEmpty();
    }

    return FText::FromString(Manager->GetLastRuntimeMessage());
}

ARuntimeGameplayManager* URuntimeCreatorHUDWidget::GetRuntimeManager() const
{
    if (IsValid(CachedRuntimeManager))
    {
        return CachedRuntimeManager.Get();
    }

    CachedRuntimeManager = ARuntimeGameplayManager::FindRuntimeGameplayManager(this);
    return CachedRuntimeManager.Get();
}

void URuntimeCreatorHUDWidget::BindRuntimeManagerEvents()
{
    if (bRuntimeEventsBound)
    {
        return;
    }

    ARuntimeGameplayManager* Manager = GetRuntimeManager();
    if (!IsValid(Manager))
    {
        return;
    }

    Manager->OnRuntimeStateChanged.AddDynamic(this, &URuntimeCreatorHUDWidget::HandleRuntimeStateChanged);
    Manager->OnRuntimeMessageChanged.AddDynamic(this, &URuntimeCreatorHUDWidget::HandleRuntimeMessageChanged);
    Manager->OnRuntimeToolbarChanged.AddDynamic(this, &URuntimeCreatorHUDWidget::HandleRuntimeToolbarChanged);
    Manager->OnRuntimeItemListWindowChanged.AddDynamic(this, &URuntimeCreatorHUDWidget::HandleRuntimeItemListWindowChanged);
    bRuntimeEventsBound = true;
}

void URuntimeCreatorHUDWidget::UnbindRuntimeManagerEvents()
{
    if (!bRuntimeEventsBound)
    {
        return;
    }

    if (IsValid(CachedRuntimeManager))
    {
        CachedRuntimeManager->OnRuntimeStateChanged.RemoveDynamic(this, &URuntimeCreatorHUDWidget::HandleRuntimeStateChanged);
        CachedRuntimeManager->OnRuntimeMessageChanged.RemoveDynamic(this, &URuntimeCreatorHUDWidget::HandleRuntimeMessageChanged);
        CachedRuntimeManager->OnRuntimeToolbarChanged.RemoveDynamic(this, &URuntimeCreatorHUDWidget::HandleRuntimeToolbarChanged);
        CachedRuntimeManager->OnRuntimeItemListWindowChanged.RemoveDynamic(this, &URuntimeCreatorHUDWidget::HandleRuntimeItemListWindowChanged);
    }

    bRuntimeEventsBound = false;
}
