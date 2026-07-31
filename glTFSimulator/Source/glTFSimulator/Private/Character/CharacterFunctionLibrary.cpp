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
    const FReferenceSkeleton& RefSkeleton = MeshAsset.GetRefSkeleton();
    if (!RefSkeleton.GetRefBonePose().IsValidIndex(BoneIndex))
    {
        return FTransform::Identity;
    }

    FTransform Transform = RefSkeleton.GetRefBonePose()[BoneIndex];
    int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
    int32 SafetyCounter = 0;
    const int32 MaxBoneCount = RefSkeleton.GetNum();

    while (ParentIndex > INDEX_NONE && ParentIndex != InParentIndex && SafetyCounter++ < MaxBoneCount)
    {
        if (!RefSkeleton.GetRefBonePose().IsValidIndex(ParentIndex))
        {
            return FTransform::Identity;
        }
        Transform *= RefSkeleton.GetRefBonePose()[ParentIndex];
        ParentIndex = RefSkeleton.GetParentIndex(ParentIndex);
    }
    return Transform.ContainsNaN() ? FTransform::Identity : Transform;
}


static bool IsSecondaryPhysicsBone(const USkeletalMeshComponent &SkeletalMesh, const FName &BoneName)
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
        if (!IsSecondaryPhysicsBone(SkeletalMesh, BoneName))
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

namespace
{
    static bool PhysicsBoneExists(const USkeletalMesh* MeshAsset, const FName BoneName)
    {
        return IsValid(MeshAsset) && BoneName != NAME_None &&
            MeshAsset->GetRefSkeleton().FindBoneIndex(BoneName) != INDEX_NONE;
    }

    static int32 FindBodySetupByBone(const UPhysicsAsset* PhysicsAsset, const FName BoneName)
    {
        if (!IsValid(PhysicsAsset) || BoneName == NAME_None)
        {
            return INDEX_NONE;
        }

        for (int32 Index = 0; Index < PhysicsAsset->SkeletalBodySetups.Num(); ++Index)
        {
            const USkeletalBodySetup* BodySetup = PhysicsAsset->SkeletalBodySetups[Index];
            if (IsValid(BodySetup) && BodySetup->BoneName == BoneName)
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }

    static int32 FindConstraintByBones(const UPhysicsAsset* PhysicsAsset, const FName BoneA, const FName BoneB)
    {
        if (!IsValid(PhysicsAsset))
        {
            return INDEX_NONE;
        }

        for (int32 Index = 0; Index < PhysicsAsset->ConstraintSetup.Num(); ++Index)
        {
            const UPhysicsConstraintTemplate* Constraint = PhysicsAsset->ConstraintSetup[Index];
            if (!IsValid(Constraint))
            {
                continue;
            }

            const FConstraintInstance& Instance = Constraint->DefaultInstance;
            const bool bSameOrder = Instance.ConstraintBone1 == BoneA && Instance.ConstraintBone2 == BoneB;
            const bool bReverseOrder = Instance.ConstraintBone1 == BoneB && Instance.ConstraintBone2 == BoneA;
            if (bSameOrder || bReverseOrder)
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }

    static void ConfigureGeneratedBody(USkeletalBodySetup& BodySetup)
    {
        static const TArray<ECollisionChannel> Channels = {ECC_WorldStatic, ECC_WorldDynamic};
        for (const ECollisionChannel Channel : Channels)
        {
            BodySetup.DefaultInstance.SetResponseToChannel(Channel, ECollisionResponse::ECR_Ignore);
        }
        BodySetup.CollisionReponse = EBodyCollisionResponse::BodyCollision_Disabled;
        BodySetup.DefaultInstance.SetCollisionEnabled(ECollisionEnabled::NoCollision);
        BodySetup.DefaultInstance.SetMassScale(0.001f);
    }

    static void ConfigureSourceBody(USkeletalBodySetup& BodySetup)
    {
        static const TArray<ECollisionChannel> Channels = {ECC_WorldStatic, ECC_WorldDynamic};
        for (const ECollisionChannel Channel : Channels)
        {
            BodySetup.DefaultInstance.SetResponseToChannel(Channel, ECollisionResponse::ECR_Block);
        }
        BodySetup.CollisionReponse = EBodyCollisionResponse::BodyCollision_Enabled;
        BodySetup.DefaultInstance.SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    }
}

UPhysicsAsset *UCharacterFunctionLibrary::MergePhysicsAsset(UPhysicsAsset *Target, UPhysicsAsset *Source, const USkeletalMesh *MeshAsset)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("MergePhysicsAsset must run on the game thread")))
    {
        return nullptr;
    }

    if (!IsValid(Target) || !IsValid(MeshAsset))
    {
        UE_LOG(LogTemp, Warning, TEXT("MergePhysicsAsset skipped: target physics asset or mesh is invalid."));
        return nullptr;
    }

    TSet<FName> SeenTargetBones;
    for (int32 Index = Target->SkeletalBodySetups.Num() - 1; Index >= 0; --Index)
    {
        USkeletalBodySetup* BodySetup = Target->SkeletalBodySetups[Index];
        if (!IsValid(BodySetup) || !PhysicsBoneExists(MeshAsset, BodySetup->BoneName) || SeenTargetBones.Contains(BodySetup->BoneName))
        {
            Target->SkeletalBodySetups.RemoveAt(Index);
            continue;
        }
        SeenTargetBones.Add(BodySetup->BoneName);
        ConfigureGeneratedBody(*BodySetup);
    }

    for (int32 Index = Target->ConstraintSetup.Num() - 1; Index >= 0; --Index)
    {
        const UPhysicsConstraintTemplate* Constraint = Target->ConstraintSetup[Index];
        if (!IsValid(Constraint) ||
            !PhysicsBoneExists(MeshAsset, Constraint->DefaultInstance.ConstraintBone1) ||
            !PhysicsBoneExists(MeshAsset, Constraint->DefaultInstance.ConstraintBone2) ||
            FindBodySetupByBone(Target, Constraint->DefaultInstance.ConstraintBone1) == INDEX_NONE ||
            FindBodySetupByBone(Target, Constraint->DefaultInstance.ConstraintBone2) == INDEX_NONE)
        {
            Target->ConstraintSetup.RemoveAt(Index);
        }
    }

    if (IsValid(Source))
    {
        for (const USkeletalBodySetup* SourceBodySetup : Source->SkeletalBodySetups)
        {
            if (!IsValid(SourceBodySetup) || !PhysicsBoneExists(MeshAsset, SourceBodySetup->BoneName))
            {
                continue;
            }

            USkeletalBodySetup* NewBodySetup = DuplicateObject<USkeletalBodySetup>(
                SourceBodySetup,
                Target,
                MakeUniqueObjectName(Target, USkeletalBodySetup::StaticClass(), SourceBodySetup->GetFName()));
            if (!IsValid(NewBodySetup))
            {
                continue;
            }
            NewBodySetup->ClearFlags(RF_Public | RF_Standalone);
            NewBodySetup->SetFlags(RF_Transient);
            ConfigureSourceBody(*NewBodySetup);

            const int32 ExistingIndex = FindBodySetupByBone(Target, SourceBodySetup->BoneName);
            if (ExistingIndex != INDEX_NONE && Target->SkeletalBodySetups.IsValidIndex(ExistingIndex))
            {
                Target->SkeletalBodySetups[ExistingIndex] = NewBodySetup;
            }
            else
            {
                Target->SkeletalBodySetups.Add(NewBodySetup);
            }
        }

        for (const UPhysicsConstraintTemplate* SourceConstraint : Source->ConstraintSetup)
        {
            if (!IsValid(SourceConstraint))
            {
                continue;
            }

            const FName BoneA = SourceConstraint->DefaultInstance.ConstraintBone1;
            const FName BoneB = SourceConstraint->DefaultInstance.ConstraintBone2;
            if (!PhysicsBoneExists(MeshAsset, BoneA) || !PhysicsBoneExists(MeshAsset, BoneB) ||
                FindBodySetupByBone(Target, BoneA) == INDEX_NONE || FindBodySetupByBone(Target, BoneB) == INDEX_NONE)
            {
                continue;
            }

            UPhysicsConstraintTemplate* NewConstraint = DuplicateObject<UPhysicsConstraintTemplate>(
                SourceConstraint,
                Target,
                MakeUniqueObjectName(Target, UPhysicsConstraintTemplate::StaticClass(), SourceConstraint->GetFName()));
            if (!IsValid(NewConstraint))
            {
                continue;
            }
            NewConstraint->ClearFlags(RF_Public | RF_Standalone);
            NewConstraint->SetFlags(RF_Transient);

            const int32 ExistingIndex = FindConstraintByBones(Target, BoneA, BoneB);
            if (ExistingIndex != INDEX_NONE && Target->ConstraintSetup.IsValidIndex(ExistingIndex))
            {
                Target->ConstraintSetup[ExistingIndex] = NewConstraint;
            }
            else
            {
                Target->ConstraintSetup.Add(NewConstraint);
            }
        }
    }

    Target->CollisionDisableTable.Empty();
    Target->UpdateBodySetupIndexMap();
    Target->UpdateBoundsBodiesArray();

    for (UPhysicsConstraintTemplate* Constraint : Target->ConstraintSetup)
    {
        if (!IsValid(Constraint))
        {
            continue;
        }
        Constraint->DefaultInstance.SetDisableCollision(true);
        Constraint->DefaultInstance.EnableProjection();

        const int32 BodyA = Target->FindBodyIndex(Constraint->DefaultInstance.ConstraintBone1);
        const int32 BodyB = Target->FindBodyIndex(Constraint->DefaultInstance.ConstraintBone2);
        if (BodyA != INDEX_NONE && BodyB != INDEX_NONE && BodyA != BodyB)
        {
            Target->DisableCollision(BodyA, BodyB);
        }
    }

#if WITH_EDITOR
    Target->InvalidateAllPhysicsMeshes();
    Target->RefreshPhysicsAssetChange();
#endif
    return Target;
}

void UCharacterFunctionLibrary::SetupAllBodiesBelowCollidersAndConstraints(
    UPhysicsAsset *PhysicsAsset,
    const USkeletalMesh *MeshAsset,
    const FName &RootBoneName)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("PhysicsAsset body/constraint creation must run on the game thread")))
    {
        return;
    }

    if (!IsValid(PhysicsAsset) || !IsValid(MeshAsset))
    {
        return;
    }

    const FReferenceSkeleton& RefSkeleton = MeshAsset->GetRefSkeleton();
    const int32 RootBoneIndex = RefSkeleton.FindBoneIndex(RootBoneName);
    if (RootBoneIndex == INDEX_NONE)
    {
        return;
    }

    // Build a child adjacency table once. The previous implementation rescanned the whole
    // skeleton for every queued bone (O(N^2)), which could hitch on large character rigs.
    TArray<TArray<int32>> ChildrenByParent;
    ChildrenByParent.SetNum(RefSkeleton.GetNum());
    for (int32 BoneIndex = 0; BoneIndex < RefSkeleton.GetNum(); ++BoneIndex)
    {
        const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
        if (ChildrenByParent.IsValidIndex(ParentIndex))
        {
            ChildrenByParent[ParentIndex].Add(BoneIndex);
        }
    }

    TArray<int32> BoneIndicesToProcess;
    BoneIndicesToProcess.Reserve(RefSkeleton.GetNum());
    BoneIndicesToProcess.Add(RootBoneIndex);
    for (int32 QueueIndex = 0; QueueIndex < BoneIndicesToProcess.Num(); ++QueueIndex)
    {
        const int32 ParentIndex = BoneIndicesToProcess[QueueIndex];
        if (ChildrenByParent.IsValidIndex(ParentIndex))
        {
            BoneIndicesToProcess.Append(ChildrenByParent[ParentIndex]);
        }
    }

    TMap<FName, USkeletalBodySetup*> BodyByBone;
    BodyByBone.Reserve(PhysicsAsset->SkeletalBodySetups.Num() + BoneIndicesToProcess.Num());
    for (USkeletalBodySetup* ExistingBody : PhysicsAsset->SkeletalBodySetups)
    {
        if (IsValid(ExistingBody) && ExistingBody->BoneName != NAME_None)
        {
            BodyByBone.FindOrAdd(ExistingBody->BoneName) = ExistingBody;
        }
    }

    for (const int32 BoneIndex : BoneIndicesToProcess)
    {
        if (!RefSkeleton.GetRawRefBoneInfo().IsValidIndex(BoneIndex))
        {
            continue;
        }
        const FName BoneName = RefSkeleton.GetBoneName(BoneIndex);
        USkeletalBodySetup* BodySetup = BodyByBone.FindRef(BoneName);
        if (!IsValid(BodySetup))
        {
            BodySetup = NewObject<USkeletalBodySetup>(
                PhysicsAsset,
                MakeUniqueObjectName(PhysicsAsset, USkeletalBodySetup::StaticClass(), BoneName),
                RF_Transient);
            if (!IsValid(BodySetup))
            {
                continue;
            }
            BodySetup->BoneName = BoneName;
            PhysicsAsset->SkeletalBodySetups.Add(BodySetup);
            BodyByBone.Add(BoneName, BodySetup);
        }

        BodySetup->RemoveSimpleCollision();
        FKSphereElem Sphere;
        Sphere.Radius = DEFAULT_RADIUS;
        Sphere.SetName(BoneName);
        BodySetup->AggGeom.SphereElems.Add(Sphere);
        BodySetup->DefaultInstance.SetUpdateKinematicFromSimulation(true);
        ConfigureGeneratedBody(*BodySetup);
    }

    PhysicsAsset->UpdateBodySetupIndexMap();

    for (const int32 ChildBoneIndex : BoneIndicesToProcess)
    {
        const int32 ParentBoneIndex = RefSkeleton.GetParentIndex(ChildBoneIndex);
        if (ParentBoneIndex == INDEX_NONE || !RefSkeleton.GetRawRefBoneInfo().IsValidIndex(ChildBoneIndex) ||
            !RefSkeleton.GetRawRefBoneInfo().IsValidIndex(ParentBoneIndex))
        {
            continue;
        }

        const FName ChildBoneName = RefSkeleton.GetBoneName(ChildBoneIndex);
        const FName ParentBoneName = RefSkeleton.GetBoneName(ParentBoneIndex);
        const int32 ChildBodyIndex = PhysicsAsset->FindBodyIndex(ChildBoneName);
        const int32 ParentBodyIndex = PhysicsAsset->FindBodyIndex(ParentBoneName);
        if (ChildBodyIndex == INDEX_NONE || ParentBodyIndex == INDEX_NONE || ChildBodyIndex == ParentBodyIndex)
        {
            continue;
        }

        const int32 ExistingConstraintIndex = FindConstraintByBones(PhysicsAsset, ChildBoneName, ParentBoneName);
        if (ExistingConstraintIndex != INDEX_NONE)
        {
            PhysicsAsset->ConstraintSetup.RemoveAt(ExistingConstraintIndex);
        }

        UPhysicsConstraintTemplate* NewConstraint = NewObject<UPhysicsConstraintTemplate>(
            PhysicsAsset,
            MakeUniqueObjectName(PhysicsAsset, UPhysicsConstraintTemplate::StaticClass(), ChildBoneName),
            RF_Transient);
        if (!IsValid(NewConstraint))
        {
            continue;
        }

        const FTransform ChildToParentTransform = GetBoneDeltaTransform(*MeshAsset, ChildBoneIndex, ParentBoneIndex);
        FConstraintInstance& DefaultInstance = NewConstraint->DefaultInstance;
        DefaultInstance.JointName = ChildBoneName;
        DefaultInstance.ConstraintBone1 = ChildBoneName;
        DefaultInstance.ConstraintBone2 = ParentBoneName;
        DefaultInstance.SetRefPosition(EConstraintFrame::Frame2, ChildToParentTransform.GetLocation());
        DefaultInstance.SetRefOrientation(
            EConstraintFrame::Frame2,
            ChildToParentTransform.GetUnitAxis(EAxis::X),
            ChildToParentTransform.GetUnitAxis(EAxis::Y));
        DefaultInstance.SetAngularSwing1Motion(EAngularConstraintMotion::ACM_Limited);
        DefaultInstance.SetAngularSwing2Motion(EAngularConstraintMotion::ACM_Limited);
        DefaultInstance.SetAngularTwistMotion(EAngularConstraintMotion::ACM_Limited);
        DefaultInstance.SetDisableCollision(true);
#if WITH_EDITOR
        NewConstraint->SetDefaultProfile(DefaultInstance);
#endif
        PhysicsAsset->ConstraintSetup.Add(NewConstraint);

        PhysicsAsset->DisableCollision(ChildBodyIndex, ParentBodyIndex);
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
    if (!ensureMsgf(IsInGameThread(), TEXT("DuplicateSkeleton must run on the game thread")))
    {
        return nullptr;
    }

    if (!IsValid(SourceSkeleton))
    {
        return nullptr;
    }

    USkeleton* NewSkeleton = DuplicateObject<USkeleton>(
        SourceSkeleton,
        GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), USkeleton::StaticClass(), FName(TEXT("RuntimeCharacterSkeleton"))));
    if (IsValid(NewSkeleton))
    {
        NewSkeleton->ClearFlags(RF_Public | RF_Standalone);
        NewSkeleton->SetFlags(RF_Transient);
    }
    return NewSkeleton;
}

USkeleton *UCharacterFunctionLibrary::MergeSkeleton(const USkeleton *Source, const USkeleton *Target)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("MergeSkeleton must run on the game thread")))
    {
        return nullptr;
    }

    if (!IsValid(Source))
    {
        return nullptr;
    }

    USkeleton* NewSkeleton = DuplicateSkeleton(Source);
    if (!IsValid(NewSkeleton) || !IsValid(Target))
    {
        return NewSkeleton;
    }

    const FReferenceSkeleton& TargetRefSkeleton = Target->GetReferenceSkeleton();
    FReferenceSkeletonModifier SkeletonModifier(NewSkeleton);

    for (int32 BoneIndex = 0; BoneIndex < TargetRefSkeleton.GetRawBoneNum(); ++BoneIndex)
    {
        const FMeshBoneInfo& BoneInfo = TargetRefSkeleton.GetRawRefBoneInfo()[BoneIndex];
        if (BoneInfo.Name == NAME_None || SkeletonModifier.FindBoneIndex(BoneInfo.Name) != INDEX_NONE)
        {
            continue;
        }

        const int32 TargetParentIndex = TargetRefSkeleton.GetParentIndex(BoneIndex);
        if (TargetParentIndex == INDEX_NONE || !TargetRefSkeleton.GetRawRefBoneInfo().IsValidIndex(TargetParentIndex))
        {
            // A merged runtime skeleton must keep a single valid root. Ignore an unrelated root.
            continue;
        }

        const FName ParentName = TargetRefSkeleton.GetRawRefBoneInfo()[TargetParentIndex].Name;
        const int32 NewParentIndex = SkeletonModifier.FindBoneIndex(ParentName);
        if (NewParentIndex == INDEX_NONE)
        {
            // Parentless/orphaned target bones are unsafe for runtime animation evaluation.
            continue;
        }

        const FTransform& BoneTransform = TargetRefSkeleton.GetRawRefBonePose()[BoneIndex];
        if (BoneTransform.ContainsNaN())
        {
            continue;
        }

        FMeshBoneInfo NewBoneInfo(BoneInfo.Name, BoneInfo.Name.ToString(), NewParentIndex);
        SkeletonModifier.Add(NewBoneInfo, BoneTransform);
    }
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