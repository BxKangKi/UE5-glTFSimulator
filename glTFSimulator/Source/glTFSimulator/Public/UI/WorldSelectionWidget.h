// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UI/StartWorldWidget.h"
#include "WorldSelectionWidget.generated.h"

/**
 * Compatibility wrapper for older WBP_LevelMenu assets.
 *
 * New widgets can inherit UStartWorldWidget directly. This class intentionally adds no
 * separate logic so the StartWorld menu flow is still owned by a single native base class.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UWorldSelectionWidget : public UStartWorldWidget
{
    GENERATED_BODY()
};
