// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Runtime/RuntimePlacementHUDWidget.h"
#include "Runtime/RuntimeGameplayManager.h"

void URuntimePlacementHUDWidget::SetManager(ARuntimeGameplayManager* InManager)
{
    Manager = InManager;
}

void URuntimePlacementHUDWidget::Refresh()
{
    // Native HUD refresh was removed. Blueprint UserWidgets should bind to
    // ARuntimeGameplayManager::OnRuntimeStateChanged or poll BuildRuntimeStatusText().
}

void URuntimePlacementHUDWidget::RebindRuntimeButtons()
{
    // Native button binding was removed. Bind your Blueprint widget buttons directly
    // to ARuntimeGameplayManager BlueprintCallable functions.
}
