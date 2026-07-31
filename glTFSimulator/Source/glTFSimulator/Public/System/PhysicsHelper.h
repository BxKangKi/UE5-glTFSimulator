// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

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
