// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

/**
 * @file CharacterFunctionLibrary.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CharacterFunctionLibrary.generated.h"

class USkeletalMeshComponent;
class UPhysicsAsset;
class USkeletalMesh;
class USkeleton;

UCLASS()
class GLTFSIMULATOR_API UCharacterFunctionLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    static void BlendRagdoll(USkeletalMeshComponent &Mesh, const float Weight, const float Threshold = 0.0f);
    static void KeepSecondaryPhysicsBodies(USkeletalMeshComponent &Mesh);
    static void DisableRagdollPhysicsButKeepSecondary(USkeletalMeshComponent &Mesh);
    static bool HasNonSecondarySimulatingPhysicsBodies(USkeletalMeshComponent &Mesh);
    /** Removes invalid/duplicate entries from a private runtime PhysicsAsset copy without replacing surviving subobjects. */
    static bool SanitizeRuntimePhysicsAsset(UPhysicsAsset *PhysicsAsset, const USkeletalMesh *MeshAsset);
    /** Rebuilds lookup/bounds/cooked state once after all runtime physics mutations are complete. */
    static void FinalizeRuntimePhysicsAsset(UPhysicsAsset *PhysicsAsset);
    static void SetupAllBodiesBelowCollidersAndConstraints(UPhysicsAsset *PhysicsAsset,
                                                           const USkeletalMesh *MeshAsset,
                                                           const FName &RootBoneName);
    static FVector GetBoneLocation(const USkeletalMeshComponent &SkeletalMesh, const FName &BoneName);
    static FRotator GetBoneRotation(const USkeletalMeshComponent &SkeletalMesh, const FName &BoneName);

    UFUNCTION(BlueprintCallable, Category = "Character|Skeleton")
    static USkeleton *DuplicateSkeleton(const USkeleton *SourceSkeleton);

    UFUNCTION(BlueprintCallable, Category = "Character|Skeleton")
    static USkeleton *MergeSkeleton(const USkeleton *Source, const USkeleton *Target);

    UFUNCTION(BlueprintCallable, Category = "Character|BitMask")
    static bool IsStateActive(int32 State, int32 BitFlag);

private:
    static void SetBodiesBelowPhysics(USkeletalMeshComponent &Mesh, bool bWakeSecondaryBodies);
    static FTransform GetBoneDeltaTransform(const USkeletalMesh &MeshAsset, const int32 BoneIndex, const int32 InParentIndex);
    static void ConfigureBodyPhysics(USkeletalMeshComponent &Mesh,
                                     const FName &RootBone, const bool bSimulate, const float BlendWeight, const bool bIncludeSelf);
};
