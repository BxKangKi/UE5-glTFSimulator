// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file PhysicsHelper.h
 * 역할: 게임 물리 계산의 공통 기능을 제공합니다.
 * 핵심 기능: 물리 상태·힘 관련 보조 연산.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"

class AActor;
class UObject;
class UWorld;
struct FHitResult;

struct FPhysicsHelper
{
    static bool Raycast(UWorld* World, const FVector& Start, const FVector& End, const FCollisionQueryParams& Params, FHitResult& HitResult);
    static bool Raycast(UWorld* World, const FVector& Start, const FVector& End, ECollisionChannel Channel, const FCollisionQueryParams& Params, FHitResult& HitResult);
    static bool Raycast(const UObject* WorldContextObject, const FVector& Start, const FVector& End, const FCollisionQueryParams& Params, FHitResult& HitResult);
    static bool Raycast(const UObject* WorldContextObject, const FVector& Start, const FVector& End, ECollisionChannel Channel, const FCollisionQueryParams& Params, FHitResult& HitResult);
    static bool Raycast(const AActor* Actor, const FVector& Start, const FVector& End, const FCollisionQueryParams& Params, FHitResult& HitResult);
    static bool Raycast(const AActor* Actor, const FVector& Start, const FVector& End, ECollisionChannel Channel, const FCollisionQueryParams& Params, FHitResult& HitResult);
    static bool Raycast(const AActor* Actor, const FVector& Start, const FVector& End, FHitResult& HitResult, bool bIncludeSelf = false);
    static bool Raycast(const AActor* Actor, const FVector& Start, const FVector& Direction, float Length, FHitResult& HitResult, bool bIncludeSelf = false);
    static bool Raycast(const AActor* Actor, const FVector& Start, const FVector& End, bool bIncludeSelf = false);
};
