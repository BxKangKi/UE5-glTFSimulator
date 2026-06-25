// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "World/LightRotation.h"

FLightRotation FLightRotation::Default()
{
    FLightRotation Result;
    Result.Sun = FRotator::ZeroRotator;
    Result.Moon = FRotator::ZeroRotator;
    return Result;
}