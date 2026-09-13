// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file MathHelper.h
 * 역할: 게임에서 사용하는 공통 수학 연산을 제공합니다.
 * 핵심 기능: 벡터·수치 관련 보조 연산.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
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