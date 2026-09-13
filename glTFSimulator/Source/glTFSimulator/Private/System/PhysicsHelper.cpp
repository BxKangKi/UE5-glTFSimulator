// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file PhysicsHelper.cpp
 * 역할: 게임 물리 계산의 공통 기능을 제공합니다.
 * 핵심 기능: 물리 상태·힘 관련 보조 연산.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "System/PhysicsHelper.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

bool FPhysicsHelper::Raycast(UWorld* World, const FVector& Start, const FVector& End, const FCollisionQueryParams& Params, FHitResult& HitResult)
{
    return Raycast(World, Start, End, ECC_Visibility, Params, HitResult);
}

bool FPhysicsHelper::Raycast(UWorld* World, const FVector& Start, const FVector& End, ECollisionChannel Channel, const FCollisionQueryParams& Params, FHitResult& HitResult)
{
    HitResult = FHitResult();

    if (!IsValid(World))
    {
        return false;
    }

    return World->LineTraceSingleByChannel(HitResult, Start, End, Channel, Params);
}

bool FPhysicsHelper::Raycast(const UObject* WorldContextObject, const FVector& Start, const FVector& End, const FCollisionQueryParams& Params, FHitResult& HitResult)
{
    return Raycast(WorldContextObject, Start, End, ECC_Visibility, Params, HitResult);
}

bool FPhysicsHelper::Raycast(const UObject* WorldContextObject, const FVector& Start, const FVector& End, ECollisionChannel Channel, const FCollisionQueryParams& Params, FHitResult& HitResult)
{
    HitResult = FHitResult();

    if (!IsValid(WorldContextObject))
    {
        return false;
    }

    return Raycast(WorldContextObject->GetWorld(), Start, End, Channel, Params, HitResult);
}

bool FPhysicsHelper::Raycast(const AActor* Actor, const FVector& Start, const FVector& End, const FCollisionQueryParams& Params, FHitResult& HitResult)
{
    return Raycast(Actor, Start, End, ECC_Visibility, Params, HitResult);
}

bool FPhysicsHelper::Raycast(const AActor* Actor, const FVector& Start, const FVector& End, ECollisionChannel Channel, const FCollisionQueryParams& Params, FHitResult& HitResult)
{
    HitResult = FHitResult();

    if (!IsValid(Actor))
    {
        return false;
    }

    return Raycast(Actor->GetWorld(), Start, End, Channel, Params, HitResult);
}

bool FPhysicsHelper::Raycast(const AActor* Actor, const FVector& Start, const FVector& End, FHitResult& HitResult, bool bIncludeSelf)
{
    FCollisionQueryParams Params(SCENE_QUERY_STAT(PhysicsHelperRaycast), false);
    if (!bIncludeSelf && IsValid(Actor))
    {
        Params.AddIgnoredActor(Actor);
    }
    return Raycast(Actor, Start, End, Params, HitResult);
}

bool FPhysicsHelper::Raycast(const AActor* Actor, const FVector& Start, const FVector& Direction, float Length, FHitResult& HitResult, bool bIncludeSelf)
{
    const FVector SafeDirection = Direction.GetSafeNormal();
    if (SafeDirection.IsNearlyZero() || Length <= 0.0f)
    {
        HitResult = FHitResult();
        return false;
    }

    const FVector End = Start + SafeDirection * Length;
    return Raycast(Actor, Start, End, HitResult, bIncludeSelf);
}

bool FPhysicsHelper::Raycast(const AActor* Actor, const FVector& Start, const FVector& End, bool bIncludeSelf)
{
    FHitResult HitResult;
    return Raycast(Actor, Start, End, HitResult, bIncludeSelf);
}
