// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "WaterInteract.generated.h"

// Engine-internal class; do not edit manually.
UINTERFACE(MinimalAPI, Blueprintable)
class UWaterInteract : public UInterface
{
    GENERATED_BODY()
};

// Actual interface class exposed to gameplay code.
class GLTFSIMULATOR_API IWaterInteract
{
    GENERATED_BODY()

public:
    // Exposes the interface so both Blueprint and C++ can implement or call it.
    virtual void EnterWater(const float Level = 0.0f) = 0;

    // Exposes the interface so both Blueprint and C++ can implement or call it.
    virtual void ExitWater(const float Level = 0.0f) = 0;
};