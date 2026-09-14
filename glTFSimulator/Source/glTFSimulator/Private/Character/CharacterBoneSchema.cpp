// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Character/CharacterBoneSchema.h"

#include "ReferenceSkeleton.h"
#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace CharacterBoneSchema
{
    const TArray<FName>& GetRequiredKeys()
    {
        static const TArray<FName> Keys = {
            TEXT("Root"),
            TEXT("chest"),
            TEXT("head"),
            TEXT("hips"),
            TEXT("leftEye"),
            TEXT("leftFoot"),
            TEXT("leftHand"),
            TEXT("leftIndexDistal"),
            TEXT("leftIndexIntermediate"),
            TEXT("leftIndexProximal"),
            TEXT("leftLittleDistal"),
            TEXT("leftLittleIntermediate"),
            TEXT("leftLittleProximal"),
            TEXT("leftLowerArm"),
            TEXT("leftLowerLeg"),
            TEXT("leftMiddleDistal"),
            TEXT("leftMiddleIntermediate"),
            TEXT("leftMiddleProximal"),
            TEXT("leftRingDistal"),
            TEXT("leftRingIntermediate"),
            TEXT("leftRingProximal"),
            TEXT("leftShoulder"),
            TEXT("leftThumbDistal"),
            TEXT("leftThumbIntermediate"),
            TEXT("leftThumbProximal"),
            TEXT("leftToes"),
            TEXT("leftUpperArm"),
            TEXT("leftUpperLeg"),
            TEXT("neck"),
            TEXT("rightEye"),
            TEXT("rightFoot"),
            TEXT("rightHand"),
            TEXT("rightIndexDistal"),
            TEXT("rightIndexIntermediate"),
            TEXT("rightIndexProximal"),
            TEXT("rightLittleDistal"),
            TEXT("rightLittleIntermediate"),
            TEXT("rightLittleProximal"),
            TEXT("rightLowerArm"),
            TEXT("rightLowerLeg"),
            TEXT("rightMiddleDistal"),
            TEXT("rightMiddleIntermediate"),
            TEXT("rightMiddleProximal"),
            TEXT("rightRingDistal"),
            TEXT("rightRingIntermediate"),
            TEXT("rightRingProximal"),
            TEXT("rightShoulder"),
            TEXT("rightThumbDistal"),
            TEXT("rightThumbIntermediate"),
            TEXT("rightThumbProximal"),
            TEXT("rightToes"),
            TEXT("rightUpperArm"),
            TEXT("rightUpperLeg"),
            TEXT("spine"),
            TEXT("upperChest")
        };
        return Keys;
    }

    bool BuildSourceToCanonicalMap(
        const TSharedPtr<FJsonObject>& BonesObject,
        TMap<FString, FString>& OutAliases,
        FString& OutError)
    {
        OutAliases.Reset();
        OutError.Reset();
        if (!BonesObject.IsValid())
        {
            OutError = TEXT("Character requires a Bones object.");
            return false;
        }

        const TArray<FName>& Required = GetRequiredKeys();
        if (BonesObject->Values.Num() != Required.Num())
        {
            OutError = FString::Printf(
                TEXT("Character Bones must contain exactly %d canonical keys; found %d."),
                Required.Num(), BonesObject->Values.Num());
            return false;
        }

        TSet<FString> RequiredStrings;
        RequiredStrings.Reserve(Required.Num());
        for (const FName Key : Required)
        {
            RequiredStrings.Add(Key.ToString());
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : BonesObject->Values)
        {
            if (!RequiredStrings.Contains(Pair.Key))
            {
                OutError = FString::Printf(
                    TEXT("Character Bones contains unsupported key '%s'. Canonical key names and case are fixed."),
                    *Pair.Key);
                return false;
            }
        }

        TSet<FString> UsedSourceBones;
        UsedSourceBones.Reserve(Required.Num());
        for (const FName CanonicalKey : Required)
        {
            const FString Canonical = CanonicalKey.ToString();
            FString SourceBone;
            if (!BonesObject->TryGetStringField(Canonical, SourceBone))
            {
                OutError = FString::Printf(TEXT("Character Bones is missing required key '%s'."), *Canonical);
                return false;
            }
            SourceBone.TrimStartAndEndInline();
            if (SourceBone.IsEmpty())
            {
                OutError = FString::Printf(TEXT("Character bone mapping '%s' cannot be empty."), *Canonical);
                return false;
            }
            if (UsedSourceBones.Contains(SourceBone))
            {
                OutError = FString::Printf(
                    TEXT("Character source bone '%s' is assigned to more than one canonical key."),
                    *SourceBone);
                return false;
            }
            UsedSourceBones.Add(SourceBone);
            OutAliases.Add(SourceBone, Canonical);
        }
        return true;
    }


    bool ValidateSourceToCanonicalMap(
        const TMap<FString, FString>& Aliases,
        FString& OutError)
    {
        OutError.Reset();
        const TArray<FName>& Required = GetRequiredKeys();
        if (Aliases.Num() != Required.Num())
        {
            OutError = FString::Printf(
                TEXT("Character bone alias map must contain exactly %d entries; found %d."),
                Required.Num(), Aliases.Num());
            return false;
        }

        TSet<FString> ExpectedCanonical;
        ExpectedCanonical.Reserve(Required.Num());
        for (const FName Key : Required) ExpectedCanonical.Add(Key.ToString());

        TSet<FString> SeenCanonical;
        SeenCanonical.Reserve(Required.Num());
        for (const TPair<FString, FString>& Pair : Aliases)
        {
            FString Source = Pair.Key.TrimStartAndEnd();
            FString Canonical = Pair.Value.TrimStartAndEnd();
            if (Source.IsEmpty() || Canonical.IsEmpty())
            {
                OutError = TEXT("Character bone aliases cannot contain empty source or canonical names.");
                return false;
            }
            if (!ExpectedCanonical.Contains(Canonical))
            {
                OutError = FString::Printf(
                    TEXT("Character bone alias targets unsupported canonical key '%s'."), *Canonical);
                return false;
            }
            if (SeenCanonical.Contains(Canonical))
            {
                OutError = FString::Printf(
                    TEXT("Character canonical bone '%s' is mapped more than once."), *Canonical);
                return false;
            }
            SeenCanonical.Add(Canonical);
        }

        for (const FString& RequiredCanonical : ExpectedCanonical)
        {
            if (!SeenCanonical.Contains(RequiredCanonical))
            {
                OutError = FString::Printf(
                    TEXT("Character bone alias map is missing canonical key '%s'."), *RequiredCanonical);
                return false;
            }
        }
        return true;
    }

    bool ValidateCanonicalReferenceSkeleton(
        const FReferenceSkeleton& ReferenceSkeleton,
        FString& OutError)
    {
        OutError.Reset();
        if (ReferenceSkeleton.GetNum() <= 0)
        {
            OutError = TEXT("Character reference skeleton is empty.");
            return false;
        }

        const TArray<FName>& Required = GetRequiredKeys();
        for (const FName RequiredBone : Required)
        {
            const int32 BoneIndex = ReferenceSkeleton.FindBoneIndex(RequiredBone);
            if (BoneIndex == INDEX_NONE)
            {
                OutError = FString::Printf(
                    TEXT("Character skeleton is missing canonical bone '%s'."),
                    *RequiredBone.ToString());
                return false;
            }
        }

        const int32 RootIndex = ReferenceSkeleton.FindBoneIndex(TEXT("Root"));
        if (RootIndex == INDEX_NONE || ReferenceSkeleton.GetParentIndex(RootIndex) != INDEX_NONE)
        {
            OutError = TEXT("Canonical character bone 'Root' must be the single root of the reference skeleton.");
            return false;
        }
        return true;
    }

    bool ValidateCanonicalSkeleton(const USkeleton* Skeleton, FString& OutError)
    {
        if (!IsValid(Skeleton))
        {
            OutError = TEXT("Character skeleton is invalid.");
            return false;
        }
        return ValidateCanonicalReferenceSkeleton(Skeleton->GetReferenceSkeleton(), OutError);
    }

    bool ValidateCanonicalHierarchyMatches(
        const FReferenceSkeleton& Candidate,
        const FReferenceSkeleton& Target,
        FString& OutError)
    {
        OutError.Reset();
        if (!ValidateCanonicalReferenceSkeleton(Candidate, OutError)) return false;
        if (!ValidateCanonicalReferenceSkeleton(Target, OutError)) return false;

        for (const FName BoneName : GetRequiredKeys())
        {
            const int32 CandidateIndex = Candidate.FindBoneIndex(BoneName);
            const int32 TargetIndex = Target.FindBoneIndex(BoneName);
            const int32 CandidateParent = Candidate.GetParentIndex(CandidateIndex);
            const int32 TargetParent = Target.GetParentIndex(TargetIndex);
            const FName CandidateParentName = CandidateParent == INDEX_NONE
                ? NAME_None : Candidate.GetBoneName(CandidateParent);
            const FName TargetParentName = TargetParent == INDEX_NONE
                ? NAME_None : Target.GetBoneName(TargetParent);
            if (CandidateParentName != TargetParentName)
            {
                OutError = FString::Printf(
                    TEXT("Canonical bone '%s' has parent '%s' but the target skeleton requires '%s'."),
                    *BoneName.ToString(), *CandidateParentName.ToString(), *TargetParentName.ToString());
                return false;
            }
        }
        return true;
    }

    bool ValidateCanonicalReferenceRotationsMatch(
        const FReferenceSkeleton& Candidate,
        const FReferenceSkeleton& Target,
        const float ToleranceDegrees,
        FString& OutError)
    {
        OutError.Reset();
        if (!ValidateCanonicalReferenceSkeleton(Candidate, OutError)) return false;
        if (!ValidateCanonicalReferenceSkeleton(Target, OutError)) return false;

        const float SafeTolerance = FMath::Max(0.0f, ToleranceDegrees);
        const TArray<FTransform>& CandidatePose = Candidate.GetRefBonePose();
        const TArray<FTransform>& TargetPose = Target.GetRefBonePose();
        for (const FName BoneName : GetRequiredKeys())
        {
            const int32 CandidateIndex = Candidate.FindBoneIndex(BoneName);
            const int32 TargetIndex = Target.FindBoneIndex(BoneName);
            const FQuat CandidateRotation = CandidatePose[CandidateIndex].GetRotation().GetNormalized();
            const FQuat TargetRotation = TargetPose[TargetIndex].GetRotation().GetNormalized();
            const double Dot = FMath::Clamp(
                static_cast<double>(FMath::Abs(CandidateRotation | TargetRotation)), 0.0, 1.0);
            const double AngularErrorDegrees = FMath::RadiansToDegrees(2.0 * FMath::Acos(Dot));
            if (!FMath::IsFinite(AngularErrorDegrees) || AngularErrorDegrees > SafeTolerance)
            {
                OutError = FString::Printf(
                    TEXT("Canonical bone '%s' reference rotation differs from the target by %.3f degrees."),
                    *BoneName.ToString(), AngularErrorDegrees);
                return false;
            }
        }
        return true;
    }

}
