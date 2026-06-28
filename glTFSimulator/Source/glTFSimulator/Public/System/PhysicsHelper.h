// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"

class AActor;

struct FPhysicsHelper
{
    static bool Raycast(AActor *Actor, const FVector &Start, const FVector &End, const FCollisionQueryParams &Params, FHitResult &HitResult);
    static bool Raycast(AActor *Actor, const FVector &Start, const FVector &End, FHitResult &HitResult, const bool bIncludeSelf = false);
    static bool Raycast(AActor *Actor, const FVector &Start, const FVector &Direction, const float Length, FHitResult &HitResult, const bool bIncludeSelf = false);
    static bool Raycast(AActor *Actor, const FVector &Start, const FVector &End, const bool bIncludeSelf = false);
};