// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file MathHelper.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"

#define __PI__ 3.14159265358979323846
#define Deg2Rad (__PI__ / 180.0f)
#define Rad2Deg (180.0f / __PI__)

struct FMathHelper
{
    static int Sign(const float value);
    static FVector Pow(const FVector &vec, const float exp);
    static float ClampLerp(const float a, const float b, const float t, const float min, const float max, const float threshold);
    static FRotator Slerp(const FRotator &a, const FRotator &b, const float t);
    static float ClampAngle(const float angle);
    static float Saturate(const float x);
    static float LengthYZ(const FVector &Vec);
    static float LengthXZ(const FVector &Vec);
    static float Unlerp(const float a, const float b, const float x);
};