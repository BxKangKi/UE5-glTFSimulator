// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file MathHelper.cpp
 * 역할: 게임에서 사용하는 공통 수학 연산을 제공합니다.
 * 핵심 기능: 벡터·수치 관련 보조 연산.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "System/MathHelper.h"
#include "Math/MathFwd.h"

int FMathHelper::Sign(const float value)
{
    if (FMath::IsNearlyZero(value))
    {
        return 0;
    }

    return value > 0.0f ? 1 : -1;
}

FVector FMathHelper::Pow(const FVector &vec, const float exp)
{
    return FVector(FMath::Pow(vec.X, exp), FMath::Pow(vec.Y, exp), FMath::Pow(vec.Z, exp));
}

float FMathHelper::ClampLerp(const float a, const float b, const float t, const float min, const float max, const float threshold)
{
    const float Result = FMath::Lerp(a, b, t);
    const float SafeMin = FMath::Min(min, max);
    const float SafeMax = FMath::Max(min, max);

    if (Result - threshold < SafeMin)
    {
        return SafeMin;
    }
    if (Result + threshold > SafeMax)
    {
        return SafeMax;
    }
    return Result;
}

FRotator FMathHelper::Slerp(const FRotator &a, const FRotator &b, const float t)
{
    FQuat A = FQuat(a);
    FQuat B = FQuat(b);
    FQuat Q = FQuat::Slerp(A, B, t);
    return Q.Rotator();
}


float FMathHelper::ClampAngle(const float angle)
{
    return FMath::Fmod((angle + 360.0f), 360.0f);
}

float FMathHelper::Saturate(const float x)
{
    return FMath::Clamp(x, 0.0f, 1.0f);
}

float FMathHelper::LengthYZ(const FVector &Vec)
{
    return FMath::Sqrt(FMath::Square(Vec.Y) + FMath::Square(Vec.Z));
}

float FMathHelper::LengthXZ(const FVector &Vec)
{
    return FMath::Sqrt(FMath::Square(Vec.X) + FMath::Square(Vec.Z));
}

float FMathHelper::Unlerp(const float a, const float b, const float x)
{
    const float Denominator = b - a;
    if (FMath::IsNearlyZero(Denominator))
    {
        return x >= b ? 1.0f : 0.0f;
    }

    return FMath::Clamp((x - a) / Denominator, 0.0f, 1.0f);
}