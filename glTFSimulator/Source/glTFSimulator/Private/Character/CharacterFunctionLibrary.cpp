// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file CharacterFunctionLibrary.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Character/CharacterFunctionLibrary.h"
#include "Editor/GlTFSimulatorEditorServices.h"
#include "System/MacroLibrary.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "PhysicsEngine/ShapeElem.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Misc/PackageName.h" // GetTransientPackage
#include "ReferenceSkeleton.h"

#define SECONDARY_BODY_RADIUS 1.0f
#define BONE_HAIR_ROOT_WEIGHT 1.0f
#define DYN_ROOT_WEIGHT 0.85f

namespace SecondaryPhysicsTuning
{
    constexpr float HairSwingLimitDegrees = 38.0f;
    constexpr float HairTwistLimitDegrees = 24.0f;
    constexpr float DynamicSwingLimitDegrees = 32.0f;
    constexpr float DynamicTwistLimitDegrees = 20.0f;
}

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

// Secondary motion is driven exclusively by Chaos. Canonical hairRoot/dynRoot names are preferred,
// but glTF skeletons may carry normalized or hair-like top-level names. Root discovery therefore
// mirrors the older working secondary-physics path instead of silently disabling hair simulation.
namespace SecondaryMotionRouting
{
    static FString NormalizeSecondaryBoneName(const FName BoneName)
    {
        FString Name = BoneName.ToString().ToLower();
        Name.ReplaceInline(TEXT("_"), TEXT(""));
        Name.ReplaceInline(TEXT("-"), TEXT(""));
        Name.ReplaceInline(TEXT("."), TEXT(""));
        Name.ReplaceInline(TEXT(" "), TEXT(""));
        Name.ReplaceInline(TEXT(":"), TEXT(""));
        return Name;
    }

    static bool IsHairLikeBoneName(const FName BoneName)
    {
        const FString Name = NormalizeSecondaryBoneName(BoneName);
        return Name.Contains(TEXT("hair"))
            || Name.Contains(TEXT("ponytail"))
            || Name.Contains(TEXT("bang"))
            || Name.Contains(TEXT("fringe"))
            || Name.Contains(TEXT("ahoge"));
    }

    static bool MatchesCanonicalRoot(const FName BoneName, const TCHAR* CanonicalRoot)
    {
        if (BoneName == FName(CanonicalRoot))
        {
            return true;
        }
        const FString Bone = NormalizeSecondaryBoneName(BoneName);
        const FString Canonical = NormalizeSecondaryBoneName(FName(CanonicalRoot));
        return Bone == Canonical || Bone.EndsWith(Canonical, ESearchCase::CaseSensitive);
    }

    static bool IsSecondaryRoot(const FName BoneName)
    {
        return MatchesCanonicalRoot(BoneName, BONE_HAIR_ROOT)
            || MatchesCanonicalRoot(BoneName, BONE_DYN_ROOT);
    }

    static bool IsBoneInSubtree(const USkeletalMeshComponent& SkeletalMesh, FName BoneName, const FName RootBone)
    {
        const int32 MaxDepth = FMath::Max(1, SkeletalMesh.GetNumBones());
        for (int32 Depth = 0; Depth < MaxDepth && BoneName != NAME_None; ++Depth)
        {
            if (BoneName == RootBone)
            {
                return true;
            }
            const FName Parent = SkeletalMesh.GetParentBone(BoneName);
            if (Parent == BoneName)
            {
                break;
            }
            BoneName = Parent;
        }
        return false;
    }

    static void GatherHairPhysicsRoots(const USkeletalMeshComponent& SkeletalMesh, TArray<FName>& OutRoots)
    {
        OutRoots.Reset();
        const FName Canonical(BONE_HAIR_ROOT);
        if (SkeletalMesh.GetBoneIndex(Canonical) != INDEX_NONE)
        {
            OutRoots.Add(Canonical);
            return;
        }

        const int32 BoneCount = SkeletalMesh.GetNumBones();
        for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
        {
            const FName BoneName = SkeletalMesh.GetBoneName(BoneIndex);
            if (MatchesCanonicalRoot(BoneName, BONE_HAIR_ROOT))
            {
                OutRoots.AddUnique(BoneName);
                continue;
            }
            if (!IsHairLikeBoneName(BoneName))
            {
                continue;
            }
            const FName Parent = SkeletalMesh.GetParentBone(BoneName);
            if (Parent == NAME_None || !IsHairLikeBoneName(Parent))
            {
                OutRoots.AddUnique(BoneName);
            }
        }
    }

    static void GatherHairPhysicsRoots(const USkeletalMesh& MeshAsset, TArray<FName>& OutRoots)
    {
        OutRoots.Reset();
        const FReferenceSkeleton& RefSkeleton = MeshAsset.GetRefSkeleton();
        const FName Canonical(BONE_HAIR_ROOT);
        if (RefSkeleton.FindBoneIndex(Canonical) != INDEX_NONE)
        {
            OutRoots.Add(Canonical);
            return;
        }

        for (int32 BoneIndex = 0; BoneIndex < RefSkeleton.GetNum(); ++BoneIndex)
        {
            const FName BoneName = RefSkeleton.GetBoneName(BoneIndex);
            if (MatchesCanonicalRoot(BoneName, BONE_HAIR_ROOT))
            {
                OutRoots.AddUnique(BoneName);
                continue;
            }
            if (!IsHairLikeBoneName(BoneName))
            {
                continue;
            }
            const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
            const FName Parent = ParentIndex != INDEX_NONE ? RefSkeleton.GetBoneName(ParentIndex) : NAME_None;
            if (Parent == NAME_None || !IsHairLikeBoneName(Parent))
            {
                OutRoots.AddUnique(BoneName);
            }
        }
    }

    static void GatherDynPhysicsRoots(const USkeletalMeshComponent& SkeletalMesh, TArray<FName>& OutRoots)
    {
        OutRoots.Reset();
        const FName Canonical(BONE_DYN_ROOT);
        if (SkeletalMesh.GetBoneIndex(Canonical) != INDEX_NONE)
        {
            OutRoots.Add(Canonical);
            return;
        }
        for (int32 BoneIndex = 0; BoneIndex < SkeletalMesh.GetNumBones(); ++BoneIndex)
        {
            const FName BoneName = SkeletalMesh.GetBoneName(BoneIndex);
            if (MatchesCanonicalRoot(BoneName, BONE_DYN_ROOT))
            {
                OutRoots.AddUnique(BoneName);
            }
        }
    }

    static void GatherDynPhysicsRoots(const USkeletalMesh& MeshAsset, TArray<FName>& OutRoots)
    {
        OutRoots.Reset();
        const FReferenceSkeleton& RefSkeleton = MeshAsset.GetRefSkeleton();
        const FName Canonical(BONE_DYN_ROOT);
        if (RefSkeleton.FindBoneIndex(Canonical) != INDEX_NONE)
        {
            OutRoots.Add(Canonical);
            return;
        }
        for (int32 BoneIndex = 0; BoneIndex < RefSkeleton.GetNum(); ++BoneIndex)
        {
            const FName BoneName = RefSkeleton.GetBoneName(BoneIndex);
            if (MatchesCanonicalRoot(BoneName, BONE_DYN_ROOT))
            {
                OutRoots.AddUnique(BoneName);
            }
        }
    }

    static bool IsSecondaryPhysicsBone(const USkeletalMeshComponent& SkeletalMesh, const FName BoneName)
    {
        if (BoneName == NAME_None)
        {
            return false;
        }

        FName CurrentBone = BoneName;
        const int32 MaxDepth = FMath::Max(1, SkeletalMesh.GetNumBones());
        for (int32 Depth = 0; Depth < MaxDepth && CurrentBone != NAME_None; ++Depth)
        {
            if (IsSecondaryRoot(CurrentBone) || IsHairLikeBoneName(CurrentBone))
            {
                return true;
            }
            const FName ParentBone = SkeletalMesh.GetParentBone(CurrentBone);
            if (ParentBone == CurrentBone)
            {
                break;
            }
            CurrentBone = ParentBone;
        }
        return false;
    }
}

void UCharacterFunctionLibrary::ConfigureBodyPhysics(
    USkeletalMeshComponent &SkeletalMesh,
    const FName &RootBone,
    const bool bSimulate,
    const float BlendWeight,
    const bool bIncludeSelf)
{
    if (SkeletalMesh.GetBoneIndex(RootBone) == INDEX_NONE)
    {
        return;
    }

    if (!bSimulate)
    {
        SkeletalMesh.SetAllBodiesBelowSimulatePhysics(RootBone, false, bIncludeSelf);
        SkeletalMesh.SetEnableGravityOnAllBodiesBelow(false, RootBone, bIncludeSelf);
        SkeletalMesh.SetAllBodiesBelowPhysicsBlendWeight(RootBone, 0.0f, false, bIncludeSelf);
        SkeletalMesh.SetAllBodiesBelowPhysicsDisabled(RootBone, true, bIncludeSelf);
        return;
    }

    // Repair actual body instances after asset/state recreation. Change simulation state
    // only when necessary so periodic audits preserve velocity and sleeping bodies.
    SkeletalMesh.SetAllBodiesBelowPhysicsDisabled(RootBone, false, true);

    const int32 BoneCount = SkeletalMesh.GetNumBones();
    for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
    {
        const FName BoneName = SkeletalMesh.GetBoneName(BoneIndex);
        if (!SecondaryMotionRouting::IsBoneInSubtree(SkeletalMesh, BoneName, RootBone))
        {
            continue;
        }

        FBodyInstance* Body = SkeletalMesh.GetBodyInstance(BoneName);
        if (!Body || !Body->IsValidBodyInstance())
        {
            continue;
        }

        const bool bIsAnchor = BoneName == RootBone && !bIncludeSelf;
        Body->SetPhysicsDisabled(false);
        Body->SetUpdateKinematicFromSimulation(false);
        const bool bWasSimulating = Body->IsInstanceSimulatingPhysics();
        if (bWasSimulating != !bIsAnchor)
        {
            Body->SetInstanceSimulatePhysics(!bIsAnchor, true, true);
        }
        Body->SetEnableGravity(!bIsAnchor);
        Body->PhysicsBlendWeight = bIsAnchor ? 0.0f : BlendWeight;
        if (!bIsAnchor && !bWasSimulating)
        {
            Body->WakeInstance();
        }
    }

    // Explicit holder roots stay animation-driven in normal motion. Discovered strands
    // include their first bone and use the parent body as their kinematic anchor.
    if (!bIncludeSelf)
    {
        SkeletalMesh.SetBodySimulatePhysics(RootBone, false);
        SkeletalMesh.SetEnableBodyGravity(false, RootBone);
        if (FBodyInstance* RootBody = SkeletalMesh.GetBodyInstance(RootBone))
        {
            RootBody->SetPhysicsDisabled(false);
            RootBody->SetUpdateKinematicFromSimulation(false);
            RootBody->PhysicsBlendWeight = 0.0f;
        }
    }
}

void UCharacterFunctionLibrary::SetBodiesBelowPhysics(
    USkeletalMeshComponent &SkeletalMesh,
    const bool bWakeSecondaryBodies)
{
    // Partial ragdoll requires physics blending to remain enabled even while the component itself is
    // not in full-ragdoll mode. Non-secondary bodies keep zero blend weight, so this does not promote
    // normal character bodies into simulation.
    // CharacterMesh is commonly QueryOnly. Partial Chaos simulation still needs physics
    // enabled on the component; preserve the profile's channel responses.
    if (SkeletalMesh.GetCollisionEnabled() != ECollisionEnabled::QueryAndPhysics)
    {
        SkeletalMesh.SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    }
    SkeletalMesh.SetEnablePhysicsBlending(true);
    SkeletalMesh.KinematicBonesUpdateType = EKinematicBonesUpdateToPhysics::SkipSimulatingBones;
    SkeletalMesh.PhysicsTransformUpdateMode = EPhysicsTransformUpdateMode::ComponentTransformIsKinematic;
    SkeletalMesh.bDeferKinematicBoneUpdate = false;
    SkeletalMesh.bSkipKinematicUpdateWhenInterpolating = false;
    SkeletalMesh.bUpdateMeshWhenKinematic = false;
    SkeletalMesh.bUpdateJointsFromAnimation = false;

    TArray<FName> HairRoots;
    SecondaryMotionRouting::GatherHairPhysicsRoots(SkeletalMesh, HairRoots);
    for (const FName HairRoot : HairRoots)
    {
        // A discovered strand is itself movable; only an explicit hairRoot is a holder.
        const bool bIncludeRoot = !SecondaryMotionRouting::MatchesCanonicalRoot(HairRoot, BONE_HAIR_ROOT);
        ConfigureBodyPhysics(SkeletalMesh, HairRoot, true, BONE_HAIR_ROOT_WEIGHT, bIncludeRoot);
    }

    TArray<FName> DynRoots;
    SecondaryMotionRouting::GatherDynPhysicsRoots(SkeletalMesh, DynRoots);
    for (const FName DynRoot : DynRoots)
    {
        ConfigureBodyPhysics(SkeletalMesh, DynRoot, true, DYN_ROOT_WEIGHT, false);
    }

    if (bWakeSecondaryBodies)
    {
        int32 SimulatingSecondaryBodies = 0;
        for (int32 BoneIndex = 0; BoneIndex < SkeletalMesh.GetNumBones(); ++BoneIndex)
        {
            const FName BoneName = SkeletalMesh.GetBoneName(BoneIndex);
            const FBodyInstance* Body = SkeletalMesh.GetBodyInstance(BoneName);
            if (Body && Body->IsInstanceSimulatingPhysics()
                && SecondaryMotionRouting::IsSecondaryPhysicsBone(SkeletalMesh, BoneName))
            {
                ++SimulatingSecondaryBodies;
            }
        }

        if (!HairRoots.IsEmpty() || !DynRoots.IsEmpty())
        {
            // Initial activation/recovery gets one explicit wake. Periodic audits do not force wake,
            // so settled tips can still sleep instead of fluttering forever.
            SkeletalMesh.WakeAllRigidBodies();
        }

        if (SimulatingSecondaryBodies > 0)
        {
            UE_LOG(LogTemp, Display,
                TEXT("Secondary physics activation: HairRoots=%d DynRoots=%d SimulatingBodies=%d Mesh=%s"),
                HairRoots.Num(), DynRoots.Num(), SimulatingSecondaryBodies, *GetNameSafe(&SkeletalMesh));
        }
        else
        {
            UE_LOG(LogTemp, Warning,
                TEXT("Secondary physics activation found no simulating bodies. HairRoots=%d DynRoots=%d Mesh=%s"),
                HairRoots.Num(), DynRoots.Num(), *GetNameSafe(&SkeletalMesh));
        }
    }
}

void UCharacterFunctionLibrary::KeepSecondaryPhysicsBodies(USkeletalMeshComponent &SkeletalMesh)
{
    SetBodiesBelowPhysics(SkeletalMesh, false);
}

void UCharacterFunctionLibrary::DisableRagdollPhysicsButKeepSecondary(USkeletalMeshComponent &SkeletalMesh)
{
    SkeletalMesh.SetAllBodiesPhysicsBlendWeight(0.0f);
    SkeletalMesh.SetAllBodiesSimulatePhysics(false);
    SkeletalMesh.SetSimulatePhysics(false);
    SetBodiesBelowPhysics(SkeletalMesh, true);
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

        if (!SecondaryMotionRouting::IsSecondaryPhysicsBone(SkeletalMesh, BoneName))
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

void UCharacterFunctionLibrary::BlendRagdoll(USkeletalMeshComponent &SkeletalMesh, const float Weight, const float Threshold)
{
    if (Weight <= Threshold)
    {
        DisableRagdollPhysicsButKeepSecondary(SkeletalMesh);
        return;
    }

    // Full ragdoll owns ALL bodies, including secondary anchors. Never run the
    // ordinary secondary setup here: it pins roots back to animation.
    SkeletalMesh.SetEnablePhysicsBlending(true);
    SkeletalMesh.SetAllBodiesSimulatePhysics(true);
    SkeletalMesh.SetAllBodiesPhysicsBlendWeight(FMath::Clamp(Weight, 0.0f, 1.0f));
    SkeletalMesh.SetEnableGravityOnAllBodiesBelow(true, NAME_None, true);
}

namespace
{
    static bool PhysicsBoneExists(const USkeletalMesh* MeshAsset, const FName BoneName)
    {
        return IsValid(MeshAsset) && BoneName != NAME_None &&
            MeshAsset->GetRefSkeleton().FindBoneIndex(BoneName) != INDEX_NONE;
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

}

bool UCharacterFunctionLibrary::SanitizeRuntimePhysicsAsset(UPhysicsAsset *PhysicsAsset, const USkeletalMesh *MeshAsset)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("SanitizeRuntimePhysicsAsset must run on the game thread")))
    {
        return false;
    }

    if (!IsValid(PhysicsAsset) || !IsValid(MeshAsset))
    {
        return false;
    }

    // The runtime asset is a private DuplicateObject of the authored default PhysicsAsset. Remove
    // only entries that cannot be used by the newly imported skeleton; preserve authored body and
    // constraint settings instead of replacing them with another layer of duplicated subobjects.
    TSet<FName> SeenBones;
    SeenBones.Reserve(PhysicsAsset->SkeletalBodySetups.Num());
    for (int32 Index = PhysicsAsset->SkeletalBodySetups.Num() - 1; Index >= 0; --Index)
    {
        USkeletalBodySetup* BodySetup = PhysicsAsset->SkeletalBodySetups[Index];
        if (!IsValid(BodySetup) || !PhysicsBoneExists(MeshAsset, BodySetup->BoneName) ||
            SeenBones.Contains(BodySetup->BoneName))
        {
            PhysicsAsset->SkeletalBodySetups.RemoveAt(Index);
            continue;
        }

        SeenBones.Add(BodySetup->BoneName);
        BodySetup->ClearFlags(RF_Public | RF_Standalone);
        BodySetup->SetFlags(RF_Transient);
    }

    TSet<FString> SeenConstraintPairs;
    SeenConstraintPairs.Reserve(PhysicsAsset->ConstraintSetup.Num());
    for (int32 Index = PhysicsAsset->ConstraintSetup.Num() - 1; Index >= 0; --Index)
    {
        UPhysicsConstraintTemplate* Constraint = PhysicsAsset->ConstraintSetup[Index];
        if (!IsValid(Constraint))
        {
            PhysicsAsset->ConstraintSetup.RemoveAt(Index);
            continue;
        }

        const FName BoneA = Constraint->DefaultInstance.ConstraintBone1;
        const FName BoneB = Constraint->DefaultInstance.ConstraintBone2;
        if (!PhysicsBoneExists(MeshAsset, BoneA) || !PhysicsBoneExists(MeshAsset, BoneB) ||
            !SeenBones.Contains(BoneA) || !SeenBones.Contains(BoneB))
        {
            PhysicsAsset->ConstraintSetup.RemoveAt(Index);
            continue;
        }

        FString A = BoneA.ToString();
        FString B = BoneB.ToString();
        if (A.Compare(B, ESearchCase::CaseSensitive) > 0)
        {
            Swap(A, B);
        }
        const FString PairKey = A + TEXT("\n") + B;
        if (SeenConstraintPairs.Contains(PairKey))
        {
            PhysicsAsset->ConstraintSetup.RemoveAt(Index);
            continue;
        }

        SeenConstraintPairs.Add(PairKey);
        Constraint->ClearFlags(RF_Public | RF_Standalone);
        Constraint->SetFlags(RF_Transient);
        Constraint->DefaultInstance.SetDisableCollision(true);
        Constraint->DefaultInstance.EnableProjection();
    }

    // Body indices may have changed while invalid/duplicate bodies were removed. Rebuild all
    // index-based collision-disable state from the surviving constraints to prevent stale indices.
    PhysicsAsset->CollisionDisableTable.Empty();
    PhysicsAsset->UpdateBodySetupIndexMap();
    PhysicsAsset->UpdateBoundsBodiesArray();
    for (const UPhysicsConstraintTemplate* Constraint : PhysicsAsset->ConstraintSetup)
    {
        if (!IsValid(Constraint))
        {
            continue;
        }

        const int32 BodyA = PhysicsAsset->FindBodyIndex(Constraint->DefaultInstance.ConstraintBone1);
        const int32 BodyB = PhysicsAsset->FindBodyIndex(Constraint->DefaultInstance.ConstraintBone2);
        if (BodyA != INDEX_NONE && BodyB != INDEX_NONE && BodyA != BodyB)
        {
            PhysicsAsset->DisableCollision(BodyA, BodyB);
        }
    }
    return true;
}

void UCharacterFunctionLibrary::FinalizeRuntimePhysicsAsset(UPhysicsAsset *PhysicsAsset)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("FinalizeRuntimePhysicsAsset must run on the game thread")) ||
        !IsValid(PhysicsAsset))
    {
        return;
    }

    PhysicsAsset->UpdateBodySetupIndexMap();
    PhysicsAsset->UpdateBoundsBodiesArray();
    FGlTFSimulatorEditorServices::RefreshPhysicsAsset(PhysicsAsset);
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

    const bool bHairChain = SecondaryMotionRouting::MatchesCanonicalRoot(RootBoneName, BONE_HAIR_ROOT)
        || SecondaryMotionRouting::IsHairLikeBoneName(RootBoneName);
    const bool bDynChain = SecondaryMotionRouting::MatchesCanonicalRoot(RootBoneName, BONE_DYN_ROOT);
    if (!bHairChain && !bDynChain)
    {
        return;
    }

    const FReferenceSkeleton& RefSkeleton = MeshAsset->GetRefSkeleton();
    const int32 RootBoneIndex = RefSkeleton.FindBoneIndex(RootBoneName);
    if (RootBoneIndex == INDEX_NONE)
    {
        // Canonical labels are authoring conventions, not a reason to disable motion when an importer
        // keeps a namespace/prefix or exposes top-level hair chains directly.
        TArray<FName> ResolvedRoots;
        if (bHairChain)
        {
            SecondaryMotionRouting::GatherHairPhysicsRoots(*MeshAsset, ResolvedRoots);
        }
        else
        {
            SecondaryMotionRouting::GatherDynPhysicsRoots(*MeshAsset, ResolvedRoots);
        }
        for (const FName ResolvedRoot : ResolvedRoots)
        {
            if (ResolvedRoot != RootBoneName)
            {
                SetupAllBodiesBelowCollidersAndConstraints(PhysicsAsset, MeshAsset, ResolvedRoot);
            }
        }
        return;
    }

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

    // Bound the actual secondary subtree, not the total imported skeleton. Face/finger/helper bones
    // outside hairRoot/dynRoot must never disable secondary motion.
    constexpr int32 MaxGeneratedSecondaryBodiesPerRoot = 512;
    if (BoneIndicesToProcess.Num() > MaxGeneratedSecondaryBodiesPerRoot)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Secondary physics subtree is too large; generated fallback skipped. Root=%s Bodies=%d Limit=%d"),
            *RootBoneName.ToString(), BoneIndicesToProcess.Num(), MaxGeneratedSecondaryBodiesPerRoot);
        return;
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

    // A standalone strand needs an animation-driven parent even when the imported
    // asset has no head body. Add only a missing immediate parent, never replace it.
    int32 GeneratedAnchorIndex = INDEX_NONE;
    const int32 AnchorIndex = RefSkeleton.GetParentIndex(RootBoneIndex);
    if (AnchorIndex != INDEX_NONE)
    {
        const FName AnchorName = RefSkeleton.GetBoneName(AnchorIndex);
        if (!BodyByBone.Contains(AnchorName))
        {
            USkeletalBodySetup* Anchor = NewObject<USkeletalBodySetup>(PhysicsAsset, NAME_None, RF_Transient);
            if (IsValid(Anchor))
            {
                Anchor->BoneName = AnchorName;
                Anchor->PhysicsType = PhysType_Default;
                FKSphereElem Shape;
                Shape.Radius = SECONDARY_BODY_RADIUS;
                Anchor->AggGeom.SphereElems.Add(Shape);
                Anchor->DefaultInstance.SetCollisionEnabled(ECollisionEnabled::PhysicsOnly);
                Anchor->DefaultInstance.SetResponseToAllChannels(ECR_Ignore);
                PhysicsAsset->SkeletalBodySetups.Add(Anchor);
                BodyByBone.Add(AnchorName, Anchor);
                GeneratedAnchorIndex = AnchorIndex;
            }
        }
    }

    TArray<int32> SecondaryBodyIndices;
    SecondaryBodyIndices.Reserve(BoneIndicesToProcess.Num());
    for (const int32 BoneIndex : BoneIndicesToProcess)
    {
        if (BoneIndex < 0 || BoneIndex >= RefSkeleton.GetNum())
        {
            continue;
        }

        const FName BoneName = RefSkeleton.GetBoneName(BoneIndex);
        USkeletalBodySetup* BodySetup = BodyByBone.FindRef(BoneName);
        const bool bHasAuthoredBody = IsValid(BodySetup);
        if (!bHasAuthoredBody)
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

            // Generate a bounded fallback collider only when the imported PhysicsAsset has no body.
            // Never clear an authored collider from the working asset.
            FKSphereElem Sphere;
            Sphere.Radius = SECONDARY_BODY_RADIUS;
            // A sphere centered exactly on the locked joint has no gravity lever arm.
            // Place fallback mass along the segment so even a terminal bone can swing.
            FVector Segment = FVector::ZeroVector;
            for (const int32 ChildIndex : ChildrenByParent[BoneIndex])
            {
                Segment = RefSkeleton.GetRefBonePose()[ChildIndex].GetLocation();
                if (!Segment.IsNearlyZero()) break;
            }
            if (Segment.IsNearlyZero())
            {
                const FTransform& LocalPose = RefSkeleton.GetRefBonePose()[BoneIndex];
                Segment = LocalPose.GetRotation().UnrotateVector(LocalPose.GetLocation());
            }
            if (!Segment.ContainsNaN() && !Segment.IsNearlyZero())
            {
                Sphere.Center = Segment.GetClampedToMaxSize(20.0f) * 0.5f;
            }
            Sphere.SetName(BoneName);
            BodySetup->AggGeom.SphereElems.Add(Sphere);

            BodySetup->DefaultInstance.InertiaTensorScale = FVector(1.0f);
            BodySetup->DefaultInstance.bOverrideMaxAngularVelocity = true;
            BodySetup->DefaultInstance.MaxAngularVelocity = 720.0f;
            BodySetup->DefaultInstance.SetMaxDepenetrationVelocity(120.0f);
            BodySetup->DefaultInstance.SetOverrideIterationCounts(true);
            BodySetup->DefaultInstance.SetPositionSolverIterationCount(8);
            BodySetup->DefaultInstance.SetVelocitySolverIterationCount(2);
            BodySetup->DefaultInstance.SetProjectionSolverIterationCount(1);
            BodySetup->DefaultInstance.SleepFamily = ESleepFamily::Sensitive;

            const bool bIsAnchor = BoneIndex == RootBoneIndex;
            BodySetup->DefaultInstance.SetCollisionEnabled(
                bIsAnchor ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryAndPhysics);
            BodySetup->CollisionReponse = bIsAnchor
                ? EBodyCollisionResponse::BodyCollision_Disabled
                : EBodyCollisionResponse::BodyCollision_Enabled;
            BodySetup->DefaultInstance.SetResponseToChannel(ECC_WorldStatic, ECR_Ignore);
            BodySetup->DefaultInstance.SetResponseToChannel(ECC_WorldDynamic, ECR_Ignore);
        }

        // Keep an active Chaos particle and mass-bearing shape. Suppress contacts
        // with response filters, not NoCollision (which removes physics participation).
        BodySetup->DefaultInstance.SetResponseToAllChannels(ECR_Ignore);
        BodySetup->DefaultInstance.SetCollisionEnabled(ECollisionEnabled::PhysicsOnly);
        BodySetup->CollisionReponse = EBodyCollisionResponse::BodyCollision_Enabled;
        BodySetup->DefaultInstance.SetMassScale(bHairChain ? 0.005f : 0.02f);
        BodySetup->DefaultInstance.LinearDamping = bHairChain ? 0.35f : 0.5f;
        BodySetup->DefaultInstance.AngularDamping = bHairChain ? 1.25f : 1.75f;
        BodySetup->DefaultInstance.InertiaTensorScale = FVector(1.0f);
        BodySetup->DefaultInstance.SleepFamily = ESleepFamily::Sensitive;
        BodySetup->DefaultInstance.bStartAwake = true;

        // Runtime state selects kinematic anchors in normal motion. Do not hard-code
        // Kinematic in the asset: full ragdoll must also simulate these bodies.
        BodySetup->PhysicsType = PhysType_Default;
        // Kinematic anchors must receive animation targets immediately; simulation-update mode is for
        // reading back a kinematic actor from Chaos and can leave the anchor stale for this use case.
        BodySetup->DefaultInstance.SetUpdateKinematicFromSimulation(false);
    }

    PhysicsAsset->UpdateBodySetupIndexMap();

    for (const int32 BoneIndex : BoneIndicesToProcess)
    {
        const int32 BodyIndex = PhysicsAsset->FindBodyIndex(RefSkeleton.GetBoneName(BoneIndex));
        if (BodyIndex != INDEX_NONE)
        {
            SecondaryBodyIndices.AddUnique(BodyIndex);
        }
    }

    // Keep the asset-level collision-disable table consistent for every pair in the secondary
    // subtree. Channel filters suppress other contacts while these pairs remain disabled
    // even if a later profile refresh changes the filter responses.
    for (int32 A = 0; A < SecondaryBodyIndices.Num(); ++A)
    {
        for (int32 B = A + 1; B < SecondaryBodyIndices.Num(); ++B)
        {
            PhysicsAsset->DisableCollision(SecondaryBodyIndices[A], SecondaryBodyIndices[B]);
        }
    }

    TArray<int32> ConstraintBoneIndices = BoneIndicesToProcess;
    if (GeneratedAnchorIndex != INDEX_NONE)
    {
        ConstraintBoneIndices.Add(GeneratedAnchorIndex);
    }
    for (const int32 ChildBoneIndex : ConstraintBoneIndices)
    {
        // Include the secondary root itself. The older working Chaos path constrained hairRoot/dynRoot
        // to the nearest authored parent body (for example head/chest) when one exists. Descendants
        // are then constrained to their direct secondary parents. This anchors the chain without
        // importing any of the old ragdoll behavior.
        int32 ParentBoneIndex = RefSkeleton.GetParentIndex(ChildBoneIndex);
        while (ParentBoneIndex != INDEX_NONE &&
            PhysicsAsset->FindBodyIndex(RefSkeleton.GetBoneName(ParentBoneIndex)) == INDEX_NONE)
        {
            ParentBoneIndex = RefSkeleton.GetParentIndex(ParentBoneIndex);
        }
        if (ParentBoneIndex == INDEX_NONE || ParentBoneIndex >= RefSkeleton.GetNum())
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
        if (ExistingConstraintIndex != INDEX_NONE && PhysicsAsset->ConstraintSetup.IsValidIndex(ExistingConstraintIndex))
        {
            // Secondary chains need a known, non-locked angular profile. Imported constraints can be
            // fixed/locked and make the chain look completely rigid, so replace only constraints
            // whose child belongs to the canonical hairRoot/dynRoot subtree. No ragdoll constraint
            // outside this subtree is touched.
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

        const float SwingLimitDegrees = bHairChain
            ? SecondaryPhysicsTuning::HairSwingLimitDegrees
            : SecondaryPhysicsTuning::DynamicSwingLimitDegrees;
        const float TwistLimitDegrees = bHairChain
            ? SecondaryPhysicsTuning::HairTwistLimitDegrees
            : SecondaryPhysicsTuning::DynamicTwistLimitDegrees;

        // A newly required parent follows its authored ancestor rigidly during full
        // ragdoll. Only secondary joints receive the hair/dynamic angular limits.
        const bool bGeneratedAnchor = ChildBoneIndex == GeneratedAnchorIndex;
        const EAngularConstraintMotion AngularMotion = bGeneratedAnchor
            ? EAngularConstraintMotion::ACM_Locked : EAngularConstraintMotion::ACM_Limited;
        DefaultInstance.SetLinearXLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
        DefaultInstance.SetLinearYLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
        DefaultInstance.SetLinearZLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
        DefaultInstance.SetAngularSwing1Limit(AngularMotion, SwingLimitDegrees);
        DefaultInstance.SetAngularSwing2Limit(AngularMotion, SwingLimitDegrees);
        DefaultInstance.SetAngularTwistLimit(AngularMotion, TwistLimitDegrees);
        DefaultInstance.SetDisableCollision(true);
        // Keep the known working secondary-joint behavior: free angular response inside the explicit
        // cone/twist limits with projection for error correction, but no dominance/drive that can
        // make a light hair chain follow the parent as if it were rigid.
        DefaultInstance.EnableProjection();
#if WITH_EDITOR
        NewConstraint->SetDefaultProfile(DefaultInstance);
#endif
        PhysicsAsset->ConstraintSetup.Add(NewConstraint);
        PhysicsAsset->DisableCollision(ChildBodyIndex, ParentBodyIndex);
    }

    PhysicsAsset->UpdateBodySetupIndexMap();
    PhysicsAsset->UpdateBoundsBodiesArray();
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