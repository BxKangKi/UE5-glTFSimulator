// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "CharacterFunctionLibrary.generated.h"

class USkeletalMeshComponent;
class ACharacter;
class UPhysicsAsset;
class USkeletalMesh;
class USkeleton;

UCLASS()
class GLTFSIMULATOR_API UCharacterFunctionLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    static void BlendRagdoll(USkeletalMeshComponent &Mesh, const float Weight, const float Thershold = 0.0f);
    static void KeepSecondaryPhysicsBodies(USkeletalMeshComponent &Mesh);
    static void DisableRagdollPhysicsButKeepSecondary(USkeletalMeshComponent &Mesh);
    static bool HasNonSecondarySimulatingPhysicsBodies(USkeletalMeshComponent &Mesh);
    static UPhysicsAsset *MergePhysicsAsset(UPhysicsAsset *Target, UPhysicsAsset *Source);
    static void SetupAllBodiesBelowCollidersAndConstraints(UPhysicsAsset *PhysicsAsset,
                                                           USkeletalMeshComponent *SkeletalMesh,
                                                           const FName &RootBoneName);
    static FVector GetBoneLocation(const USkeletalMeshComponent &SkeletalMesh, const FName &BoneName);
    static FRotator GetBoneRotation(const USkeletalMeshComponent &SkeletalMesh, const FName &BoneName);

    UFUNCTION(BlueprintCallable, Category = "Character|Skeleton")
    static void CopyBoneTransforms(USkeletalMesh *SourceMeshAsset, USkeletalMeshComponent *TargetMesh);

    UFUNCTION(BlueprintCallable, Category = "Character|Skeleton")
    static TMap<FString, FTransform> GenerateBoneTransformMap(USkeletalMeshComponent *SkeletonComp);

    UFUNCTION(BlueprintCallable, Category = "Character|Skeleton")
    static USkeleton *DuplicateSkeleton(const USkeleton *SourceSkeleton);

    UFUNCTION(BlueprintCallable, Category = "Character|Skeleton")
    static USkeleton *MergeSkeleton(const USkeleton *Source, const USkeleton *Target);

    UFUNCTION(BlueprintCallable, Category = "Character|BitMask")
    static bool IsStateActive(int32 State, int32 BitFlag);

private:
    static void SetBodiesBelowPhysics(USkeletalMeshComponent &Mesh);
    static FTransform GetBoneDeltaTransform(const USkeletalMesh &MeshAsset, const int32 BoneIndex, const int32 InParentIndex);
    static void ConfigureBodyPhysics(USkeletalMeshComponent &Mesh,
                                     const FName &RootBone, const bool bSimulate, const float BlendWeight, const bool bIncludeSelf);

};