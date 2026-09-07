// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Editor-facing object categories used only for stable generated names.

#pragma once

#include "CoreMinimal.h"
#include "PlacementTypes.generated.h"

UENUM(BlueprintType)
enum class EPlacedObjectKind : uint8
{
    Prefab = 0 UMETA(DisplayName = "Prefab"),
    Vehicle = 1 UMETA(DisplayName = "Vehicle")
};
