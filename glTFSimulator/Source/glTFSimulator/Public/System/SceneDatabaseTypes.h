// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Lightweight Scene-index value types shared without pulling binary/player headers into UHT classes.

#pragma once

#include "CoreMinimal.h"

/** One Scene definition's world-space bounds persisted in data/scenes.dat. */
struct GLTFSIMULATOR_API FSceneDatabaseEntry
{
    FGuid UUID;
    /** Center of the complete Scene bounds. */
    FVector Location = FVector::ZeroVector;
    /** Full width/depth/height of the complete Scene bounds. */
    FVector Size = FVector::ZeroVector;
};
