// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file PhysicsHelper.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
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
