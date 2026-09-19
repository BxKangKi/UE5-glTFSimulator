// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file JsonData.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "Dom/JsonObject.h"
#include "JsonData.generated.h"

// Engine-internal class; do not edit manually.
UINTERFACE(MinimalAPI, Blueprintable)
class UJsonData : public UInterface
{
    GENERATED_BODY()
};

// Actual interface class exposed to gameplay code.
class GLTFSIMULATOR_API IJsonData
{
    GENERATED_BODY()

public:
    virtual TSharedRef<FJsonObject> Serialization() = 0;
    virtual bool Deserialization(TSharedPtr<FJsonObject> Json) = 0;
};