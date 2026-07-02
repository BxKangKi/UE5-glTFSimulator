// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Gameplay/PlacementHUDWidget.h"
#include "System/GameManagerSubSystem.h"

void UPlacementHUDWidget::SetManager(UGameManagerSubSystem* InManager)
{
    Manager = InManager;
}

void UPlacementHUDWidget::Refresh()
{
    // Native HUD refresh was removed. Blueprint UserWidgets should bind to
    // UGameManagerSubSystem::OnStateChanged or poll BuildStatusText().
}

void UPlacementHUDWidget::RebindButtons()
{
    // Native button binding was removed. Bind your Blueprint widget buttons directly
    // to UGameManagerSubSystem BlueprintCallable functions.
}
