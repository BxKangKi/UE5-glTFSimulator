// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Editor-facing object categories used only for stable generated names.

/**
 * @file PlacementTypes.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "PlacementTypes.generated.h"

UENUM(BlueprintType)
enum class EPlacedObjectKind : uint8
{
    Static = 0 UMETA(DisplayName = "Static"),
    Vehicle = 1 UMETA(DisplayName = "Vehicle")
};
