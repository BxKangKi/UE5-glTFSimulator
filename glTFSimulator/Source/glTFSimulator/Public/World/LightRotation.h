// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "LightRotation.generated.h"

USTRUCT(BlueprintType)
struct FLightRotation
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "glTFStreamActor")
    FRotator Sun = FRotator::ZeroRotator;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "glTFStreamActor")
    FRotator Moon = FRotator::ZeroRotator;

    static FLightRotation Default();
};