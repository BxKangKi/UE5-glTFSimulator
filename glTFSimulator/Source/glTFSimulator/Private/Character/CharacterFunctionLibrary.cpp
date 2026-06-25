// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Character/CharacterFunctionLibrary.h"
#include "System/MacroLibrary.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "PhysicsEngine/ShapeElem.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimCurveTypes.h"
#include "Misc/PackageName.h" // GetTransientPackage
#include "ReferenceSkeleton.h"

#define DEFAULT_RADIUS 1.0f
#define BONE_HAIR_ROOT_WEIGHT 0.5f
#define DYN_ROOT_WEIGHT 0.4f
#define DEFAULT_DAMPING 0.1f

FTransform UCharacterFunctionLibrary::GetBoneDeltaTransform(const USkeletalMesh &MeshAsset, const int32 BoneIndex, const int32 InParentIndex)
{
    FTransform Transform = MeshAsset.GetRefSkeleton().GetRefBonePose()[BoneIndex];
    int32 ParentIndex = MeshAsset.GetRefSkeleton().GetParentIndex(BoneIndex);

    while (ParentIndex > INDEX_NONE && ParentIndex != InParentIndex)
    {
        Transform *= MeshAsset.GetRefSkeleton().GetRefBonePose()[ParentIndex];
        ParentIndex = MeshAsset.GetRefSkeleton().GetParentIndex(ParentIndex);
    }
    return Transform;
}


static bool IsRuntimeSecondaryPhysicsBone(const USkeletalMeshComponent &SkeletalMesh, const FName &BoneName)
{
    FName CurrentBone = BoneName;
    while (CurrentBone != NAME_None)
    {
        if (CurrentBone == FName(BONE_HAIR_ROOT) || CurrentBone == FName(BONE_DYN_ROOT))
        {
            return true;
        }
        CurrentBone = SkeletalMesh.GetParentBone(CurrentBone);
    }
    return false;
}

void UCharacterFunctionLibrary::ConfigureBodyPhysics(USkeletalMeshComponent &SkeletalMesh, const FName &RootBone, const bool bSimulate, const float BlendWeight, const bool bIncludeSelf)
{
    if (SkeletalMesh.GetBoneIndex(RootBone) == INDEX_NONE)
    {
        return;
    }

    SkeletalMesh.SetAllBodiesBelowSimulatePhysics(RootBone, bSimulate, bIncludeSelf);
    SkeletalMesh.SetAllBodiesBelowPhysicsBlendWeight(RootBone, BlendWeight, false, bIncludeSelf);
}

void UCharacterFunctionLibrary::SetBodiesBelowPhysics(USkeletalMeshComponent &SkeletalMesh)
{
    ConfigureBodyPhysics(SkeletalMesh, BONE_HAIR_ROOT, true, BONE_HAIR_ROOT_WEIGHT, false);
    ConfigureBodyPhysics(SkeletalMesh, BONE_DYN_ROOT, true, DYN_ROOT_WEIGHT, false);
}

void UCharacterFunctionLibrary::KeepSecondaryPhysicsBodies(USkeletalMeshComponent &SkeletalMesh)
{
    SetBodiesBelowPhysics(SkeletalMesh);
}

void UCharacterFunctionLibrary::DisableRagdollPhysicsButKeepSecondary(USkeletalMeshComponent &SkeletalMesh)
{
    SkeletalMesh.SetAllBodiesPhysicsBlendWeight(0.0f);
    SkeletalMesh.SetAllBodiesSimulatePhysics(false);
    SkeletalMesh.SetSimulatePhysics(false);
    SetBodiesBelowPhysics(SkeletalMesh);
}

bool UCharacterFunctionLibrary::HasNonSecondarySimulatingPhysicsBodies(USkeletalMeshComponent &SkeletalMesh)
{
    const int32 BoneCount = SkeletalMesh.GetNumBones();
    for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
    {
        const FName BoneName = SkeletalMesh.GetBoneName(BoneIndex);
        const FBodyInstance* BodyInstance = SkeletalMesh.GetBodyInstance(BoneName);
        if (!BodyInstance || !BodyInstance->IsInstanceSimulatingPhysics())
        {
            continue;
        }

        // Only pay the parent-chain check for bodies that are actually simulating.
        // In normal gameplay most bones are kinematic, so this removes a large per-tick cost.
        if (!IsRuntimeSecondaryPhysicsBone(SkeletalMesh, BoneName))
        {
            return true;
        }
    }

    return false;
}

// Bitwise state test. Keeps the caller independent from enum wrappers.
bool UCharacterFunctionLibrary::IsStateActive(int32 State, int32 BitFlag)
{
    return (State & BitFlag) != 0;
}

void UCharacterFunctionLibrary::BlendRagdoll(USkeletalMeshComponent &SkeletalMesh, const float Weight, const float Thershold)
{
    SkeletalMesh.SetAllBodiesPhysicsBlendWeight(Weight);
    if (Weight <= Thershold)
    {
        SkeletalMesh.SetAllBodiesSimulatePhysics(false);
    }
    SetBodiesBelowPhysics(SkeletalMesh);
}

UPhysicsAsset *UCharacterFunctionLibrary::MergePhysicsAsset(UPhysicsAsset *Target, UPhysicsAsset *Source)
{
    if (!IsValid(Target) || !IsValid(Source))
    {
        UE_LOG(LogTemp, Warning, TEXT("Null PhysicsAsset given."));
        return nullptr;
    }

    static const TArray<ECollisionChannel> Channels = {ECC_WorldStatic, ECC_WorldDynamic};

    // Reset target bodies to no collision and a tiny mass.
    for (USkeletalBodySetup *TargetBodySetup : Target->SkeletalBodySetups)
    {
        if (!IsValid(TargetBodySetup))
            continue;

        // int32 Index = Target->FindBodyIndex(TargetBodySetup->BoneName);

        auto &DefaultInstance = TargetBodySetup->DefaultInstance;
        for (ECollisionChannel Channel : Channels)
        {
            DefaultInstance.SetResponseToChannel(Channel, ECollisionResponse::ECR_Ignore);
        }
        TargetBodySetup->CollisionReponse = EBodyCollisionResponse::BodyCollision_Disabled;
        DefaultInstance.SetCollisionEnabled(ECollisionEnabled::NoCollision);
        DefaultInstance.SetMassScale(0.001f);
        // DefaultInstance.SetEnableGravity(false);
    }

    // Replace matching source bodies on the target or append missing bodies.
    for (const USkeletalBodySetup *SourceBodySetup : Source->SkeletalBodySetups)
    {
        if (!IsValid(SourceBodySetup))
            continue;

        int32 TargetIndex = Target->FindBodyIndex(SourceBodySetup->BoneName);

        if (TargetIndex != INDEX_NONE)
        {
            // Replace an existing target body with a duplicated source body.
            Target->SkeletalBodySetups.RemoveAt(TargetIndex);

            USkeletalBodySetup *NewBodySetup = DuplicateObject<USkeletalBodySetup>(SourceBodySetup, Target);
            for (ECollisionChannel Channel : Channels)
            {
                NewBodySetup->DefaultInstance.SetResponseToChannel(Channel, ECollisionResponse::ECR_Block);
            }
            Target->SkeletalBodySetups.Insert(NewBodySetup, TargetIndex);
        }
        else
        {
            // Append bodies that do not exist on the target asset.
            USkeletalBodySetup *NewBodySetup = DuplicateObject<USkeletalBodySetup>(SourceBodySetup, Target);
            for (ECollisionChannel Channel : Channels)
            {
                NewBodySetup->DefaultInstance.SetResponseToChannel(Channel, ECollisionResponse::ECR_Block);
            }
            Target->SkeletalBodySetups.Add(NewBodySetup);
        }
    }

    // Reset target constraints.
    for (UPhysicsConstraintTemplate *TargetConstraint : Target->ConstraintSetup)
    {
        if (!TargetConstraint)
            continue;
        auto &DefaultInstance = TargetConstraint->DefaultInstance;
        DefaultInstance.SetDisableCollision(true);
        DefaultInstance.EnableProjection();
    }

    // Replace matching source constraints on the target or append missing constraints.
    for (const UPhysicsConstraintTemplate *SourceConstraint : Source->ConstraintSetup)
    {
        if (!IsValid(SourceConstraint))
            continue;
        int32 ExistingIndex = INDEX_NONE;
        for (int32 i = 0; i < Target->ConstraintSetup.Num(); ++i)
        {
            UPhysicsConstraintTemplate *TargetConstraint = Target->ConstraintSetup[i];
            if (TargetConstraint &&
                TargetConstraint->DefaultInstance.ConstraintBone1 == SourceConstraint->DefaultInstance.ConstraintBone1 &&
                TargetConstraint->DefaultInstance.ConstraintBone2 == SourceConstraint->DefaultInstance.ConstraintBone2)
            {
                ExistingIndex = i;
                break;
            }
        }

        if (ExistingIndex != INDEX_NONE)
        {
            // Replace the existing constraint with a duplicated source constraint.
            Target->ConstraintSetup.RemoveAt(ExistingIndex);
            UPhysicsConstraintTemplate *NewConstraint = DuplicateObject<UPhysicsConstraintTemplate>(SourceConstraint, Target);
            Target->ConstraintSetup.Insert(NewConstraint, ExistingIndex);
        }
        else
        {
            // Add constraints that do not exist on the target asset.
            UPhysicsConstraintTemplate *NewConstraint = DuplicateObject<UPhysicsConstraintTemplate>(SourceConstraint, Target);
            Target->ConstraintSetup.Add(NewConstraint);
        }
    }

    Target->UpdateBodySetupIndexMap();
    Target->UpdateBoundsBodiesArray();

#if WITH_EDITOR
    Target->InvalidateAllPhysicsMeshes();
    Target->RefreshPhysicsAssetChange();
#endif

    return Target;
}

void UCharacterFunctionLibrary::SetupAllBodiesBelowCollidersAndConstraints(
    UPhysicsAsset *PhysicsAsset,
    USkeletalMeshComponent *SkeletalMesh,
    const FName &RootBoneName)
{
    if (!IsValid(PhysicsAsset) || !IsValid(SkeletalMesh))
    {
        UE_LOG(LogTemp, Warning, TEXT("Invalid PhysicsAsset or SkeletalMesh"));
        return;
    }

    USkeletalMesh *MeshAsset = SkeletalMesh->GetSkeletalMeshAsset();
    if (!IsValid(MeshAsset))
        return;

    int32 RootBoneIndex = MeshAsset->GetRefSkeleton().FindBoneIndex(RootBoneName);
    if (RootBoneIndex == INDEX_NONE)
    {
        UE_LOG(LogTemp, Warning, TEXT("Root bone not found: %s"), *RootBoneName.ToString());
        return;
    }

    TArray<int32> BoneIndicesToProcess;
    BoneIndicesToProcess.Add(RootBoneIndex);

    int32 NumBones = MeshAsset->GetRefSkeleton().GetNum();

    // Traverse child bones iteratively instead of recursively.
    for (int32 i = 0; i < BoneIndicesToProcess.Num(); ++i)
    {
        int32 CurrentBoneIndex = BoneIndicesToProcess[i];
        for (int32 ChildIdx = 0; ChildIdx < NumBones; ++ChildIdx)
        {
            if (MeshAsset->GetRefSkeleton().GetParentIndex(ChildIdx) == CurrentBoneIndex)
            {
                BoneIndicesToProcess.Add(ChildIdx);
            }
        }
    }

    // Add colliders.
    for (int32 BoneIndex : BoneIndicesToProcess)
    {
        FName BoneName = MeshAsset->GetRefSkeleton().GetBoneName(BoneIndex);

        int32 BodyIndex = PhysicsAsset->FindBodyIndex(BoneName);
        if (BodyIndex != INDEX_NONE)
        {
            PhysicsAsset->SkeletalBodySetups[BodyIndex]->RemoveSimpleCollision();
        }

        USkeletalBodySetup *NewBodySetup = NewObject<USkeletalBodySetup>(PhysicsAsset, NAME_None, RF_Public);
        NewBodySetup->BoneName = BoneName;

        FKSphereElem Sphere;
        Sphere.Radius = DEFAULT_RADIUS;
        Sphere.SetName(BoneName);
        NewBodySetup->AggGeom.SphereElems.Add(Sphere);
        NewBodySetup->DefaultInstance.SetUpdateKinematicFromSimulation(true);
        PhysicsAsset->SkeletalBodySetups.Add(NewBodySetup);

#if WITH_EDITOR
        UE_LOG(LogTemp, Log, TEXT("Added collider to bone %s"), *BoneName.ToString());
#endif
    }

    // Add parent-child constraints, including the requested root bone.
    for (int32 i = 0; i < BoneIndicesToProcess.Num(); ++i)
    {
        int32 ChildBoneIndex = BoneIndicesToProcess[i];
        int32 ParentBoneIndex = MeshAsset->GetRefSkeleton().GetParentIndex(ChildBoneIndex);
        if (ParentBoneIndex == INDEX_NONE)
            continue;

        FName ParentBoneName = MeshAsset->GetRefSkeleton().GetBoneName(ParentBoneIndex);
        FName ChildBoneName = MeshAsset->GetRefSkeleton().GetBoneName(ChildBoneIndex);

        int32 ConstraintIndex = PhysicsAsset->FindConstraintIndex(ParentBoneName, ChildBoneName);
        if (ConstraintIndex != INDEX_NONE)
        {
            PhysicsAsset->ConstraintSetup.RemoveAt(ConstraintIndex);
        }

        UPhysicsConstraintTemplate *NewConstraint = NewObject<UPhysicsConstraintTemplate>(PhysicsAsset, NAME_None, RF_Public);
        FTransform ChildToParentTransform = GetBoneDeltaTransform(*MeshAsset, ChildBoneIndex, ParentBoneIndex);

        auto &DefaultInstance = NewConstraint->DefaultInstance;
        DefaultInstance.JointName = ChildBoneName;
        DefaultInstance.ConstraintBone1 = ChildBoneName;
        DefaultInstance.ConstraintBone2 = ParentBoneName;

        DefaultInstance.SetRefPosition(EConstraintFrame::Frame2, ChildToParentTransform.GetLocation());
        DefaultInstance.SetRefOrientation(EConstraintFrame::Frame2, ChildToParentTransform.GetUnitAxis(EAxis::X), ChildToParentTransform.GetUnitAxis(EAxis::Y));
        DefaultInstance.SetAngularSwing1Motion(EAngularConstraintMotion::ACM_Limited);
        DefaultInstance.SetAngularSwing2Motion(EAngularConstraintMotion::ACM_Limited);
        DefaultInstance.SetAngularTwistMotion(EAngularConstraintMotion::ACM_Limited);

#if WITH_EDITOR
        NewConstraint->SetDefaultProfile(DefaultInstance);
#endif
        PhysicsAsset->ConstraintSetup.Add(NewConstraint);
        PhysicsAsset->DisableCollision(ChildBoneIndex, ParentBoneIndex);
#if WITH_EDITOR
        UE_LOG(LogTemp, Log, TEXT("Added constraint between %s and %s"), *ParentBoneName.ToString(), *ChildBoneName.ToString());
#endif
    }

    PhysicsAsset->UpdateBodySetupIndexMap();
    PhysicsAsset->UpdateBoundsBodiesArray();

#if WITH_EDITOR
    PhysicsAsset->InvalidateAllPhysicsMeshes();
    PhysicsAsset->RefreshPhysicsAssetChange();
#endif
}

void UCharacterFunctionLibrary::CopyBoneTransforms(USkeletalMesh *SourceMeshAsset, USkeletalMeshComponent *TargetMesh)
{
    if (!IsValid(SourceMeshAsset) || !IsValid(TargetMesh))
    {
        UE_LOG(LogTemp, Warning, TEXT("SourceMeshAsset or TargetMesh is invalid."));
        return;
    }

    // Read skeleton data from the source USkeletalMesh.
    USkeleton *SourceSkeleton = SourceMeshAsset->GetSkeleton();
    if (!IsValid(SourceSkeleton))
    {
        UE_LOG(LogTemp, Warning, TEXT("Could not find the skeleton for SourceMeshAsset."));
        return;
    }

    const int32 BoneCount = SourceSkeleton->GetReferenceSkeleton().GetNum();

    // Apply source skeleton bone transforms to the target mesh.
    for (int32 i = 0; i < BoneCount; ++i)
    {
        FName BoneName = SourceSkeleton->GetReferenceSkeleton().GetBoneName(i);
        int32 TargetBoneIndex = TargetMesh->GetBoneIndex(BoneName);

        if (TargetBoneIndex != INDEX_NONE)
        {
            // Read the target bone transform in component space.
            // Cast EBoneSpaces::Type to ERelativeTransformSpace.
            FTransform BoneTransform = TargetMesh->GetBoneTransform(BoneName, static_cast<ERelativeTransformSpace>(EBoneSpaces::ComponentSpace));

            // Write the updated bone transform back to the target mesh.
            if (TargetMesh->GetEditableComponentSpaceTransforms().IsValidIndex(TargetBoneIndex))
            {
                TargetMesh->GetEditableComponentSpaceTransforms()[TargetBoneIndex] = BoneTransform;
            }
        }
    }

    // Refresh transforms.
    TargetMesh->RefreshBoneTransforms();
    TargetMesh->UpdateComponentToWorld();
    TargetMesh->FinalizeBoneTransform();
    TargetMesh->MarkRenderTransformDirty();
    TargetMesh->MarkRenderDynamicDataDirty();

    UE_LOG(LogTemp, Log, TEXT("Bone transform copy completed."));
}

TMap<FString, FTransform> UCharacterFunctionLibrary::GenerateBoneTransformMap(USkeletalMeshComponent *SkeletonComp)
{
    // Example placeholder: MySkeletalMeshComponent should be your actual USkeletalMeshComponent pointer.
    TMap<FString, FTransform> BoneTransformMap;
    if (IsValid(SkeletonComp))
    {
        USkeletalMesh *MeshAsset = SkeletonComp->GetSkeletalMeshAsset();
        if (IsValid(MeshAsset))
        {
            const USkeleton *Skeleton = MeshAsset->GetSkeleton();
            if (IsValid(Skeleton))
            {
                const int32 NumBones = SkeletonComp->GetNumBones();
                for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
                {
                    FString BoneName = SkeletonComp->GetBoneName(BoneIndex).ToString();
                    FTransform BoneTransform = SkeletonComp->GetBoneTransform(BoneIndex);
                    BoneTransformMap.Add(BoneName, BoneTransform);
                }
            }
        }
    }
    return BoneTransformMap;
}

USkeleton *UCharacterFunctionLibrary::DuplicateSkeleton(const USkeleton *SourceSkeleton)
{
    if (!IsValid(SourceSkeleton))
    {
        return nullptr;
    }
    // Deep-copy into the runtime transient package, which is the safest ownership model here.
    USkeleton *NewSkeleton = DuplicateObject<USkeleton>(
        SourceSkeleton,
        GetTransientPackage(),
        FName(TEXT("Skeleton")));

    NewSkeleton->SetFlags(RF_Transient);
    return NewSkeleton;
}

USkeleton *UCharacterFunctionLibrary::MergeSkeleton(const USkeleton *Source, const USkeleton *Target)
{
    if (!Source || !Target)
        return nullptr;

    // 1. Start by duplicating the source skeleton.
    USkeleton *NewSkeleton = DuplicateObject<USkeleton>(Source, nullptr);

    // 2. Get the editable reference skeleton.
    const FReferenceSkeleton &TargetRefSkeleton = Target->GetReferenceSkeleton();
    FReferenceSkeletonModifier SkeletonModifier(NewSkeleton);

    // 3. Iterate all target bones and add missing bones.
    for (int32 i = 0; i < TargetRefSkeleton.GetRawBoneNum(); ++i)
    {
        const FMeshBoneInfo &BoneInfo = TargetRefSkeleton.GetRawRefBoneInfo()[i];
        const FTransform &BoneTransform = TargetRefSkeleton.GetRawRefBonePose()[i];

        // Skip bones that already exist to avoid duplicates.
        if (SkeletonModifier.FindBoneIndex(BoneInfo.Name) == INDEX_NONE)
        {
            // Resolve the parent bone name.
            int32 ParentIndex = TargetRefSkeleton.GetParentIndex(i);
            FName ParentName = (ParentIndex != INDEX_NONE) ? TargetRefSkeleton.GetRawRefBoneInfo()[ParentIndex].Name : NAME_None;
            int32 NewParentIndex = SkeletonModifier.FindBoneIndex(ParentName);

            // Add the bone by name, parent index, and transform.
            FMeshBoneInfo NewBoneInfo(BoneInfo.Name, BoneInfo.Name.ToString(), NewParentIndex);
            SkeletonModifier.Add(NewBoneInfo, BoneTransform);
        }
    }

    // 4. Apply changes and rebuild skeleton data.
    // SkeletonModifier also rebuilds on destruction, but an explicit rebuild is safer here.
    // NewSkeleton->RecreateBoneTree();
    return NewSkeleton;
}

FVector UCharacterFunctionLibrary::GetBoneLocation(const USkeletalMeshComponent &SkeletalMesh, const FName &BoneName)
{
    if (SkeletalMesh.DoesSocketExist(BoneName))
    {
        return SkeletalMesh.GetSocketLocation(BoneName);
    }
    return FVector::ZeroVector;
}

FRotator UCharacterFunctionLibrary::GetBoneRotation(const USkeletalMeshComponent &SkeletalMesh, const FName &BoneName)
{
    if (SkeletalMesh.DoesSocketExist(BoneName))
    {
        return SkeletalMesh.GetSocketRotation(BoneName);
    }
    return FRotator::ZeroRotator;
}