// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "System/PhysicsHelper.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

bool FPhysicsHelper::Raycast(AActor *Actor, const FVector &Start, const FVector &End, const FCollisionQueryParams &Params, FHitResult &HitResult)
{
    if (!IsValid(Actor))
        return false;
    UWorld *World = Actor->GetWorld();
    if (!IsValid(World))
        return false;
    return World->LineTraceSingleByChannel(HitResult, Start, End, ECC_Visibility, Params);
}

bool FPhysicsHelper::Raycast(AActor *Actor, const FVector &Start, const FVector &End, FHitResult &HitResult, const bool bIncludeSelf)
{
    FCollisionQueryParams Params;
    Params.AddIgnoredActor(Actor);
    return Raycast(Actor, Start, End, Params, HitResult);
}


bool FPhysicsHelper::Raycast(AActor *Actor, const FVector &Start, const FVector &Direction, const float Length, FHitResult &HitResult, const bool bIncludeSelf)
{
    const FVector End = Start + (Length * Direction);
    return Raycast(Actor, Start, End, HitResult, bIncludeSelf);
}

bool FPhysicsHelper::Raycast(AActor *Actor, const FVector &Start, const FVector &End, const bool bIncludeSelf)
{
    FHitResult HitResult;
    return Raycast(Actor, Start, End, HitResult, bIncludeSelf);
}