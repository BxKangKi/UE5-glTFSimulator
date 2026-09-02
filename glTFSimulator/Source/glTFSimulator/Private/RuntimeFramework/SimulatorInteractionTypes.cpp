#include "RuntimeFramework/SimulatorInteractionTypes.h"

void FSimulatorFingerPose::Clamp()
{
    ThumbCurl = FMath::Clamp(ThumbCurl, 0.0f, 1.0f);
    IndexCurl = FMath::Clamp(IndexCurl, 0.0f, 1.0f);
    MiddleCurl = FMath::Clamp(MiddleCurl, 0.0f, 1.0f);
    RingCurl = FMath::Clamp(RingCurl, 0.0f, 1.0f);
    LittleCurl = FMath::Clamp(LittleCurl, 0.0f, 1.0f);
    Spread = FMath::Clamp(Spread, -1.0f, 1.0f);
}

ESimulatorHand FSimulatorEquipmentInteractionConfig::ResolvePrimaryHand(const ESimulatorHand CharacterDominantHand) const
{
    return bOverridePrimaryHand ? PrimaryHandOverride : CharacterDominantHand;
}

const FSimulatorGripPoint* FSimulatorEquipmentInteractionConfig::GetGrip(const ESimulatorHand Hand) const
{
    const FSimulatorGripPoint& Grip = Hand == ESimulatorHand::Right ? RightGrip : LeftGrip;
    return Grip.bEnabled && Grip.Role != ESimulatorGripRole::Disabled ? &Grip : nullptr;
}

void FSimulatorEquipmentInteractionConfig::Sanitize()
{
    HeldPreviewLongestDimensionCm = FMath::Clamp(HeldPreviewLongestDimensionCm, 1.0f, 100.0f);
    RightGrip.Hand = ESimulatorHand::Right;
    LeftGrip.Hand = ESimulatorHand::Left;
    RightGrip.IKAlpha = FMath::Clamp(RightGrip.IKAlpha, 0.0f, 1.0f);
    LeftGrip.IKAlpha = FMath::Clamp(LeftGrip.IKAlpha, 0.0f, 1.0f);
    RightGrip.Fingers.Clamp();
    LeftGrip.Fingers.Clamp();
    FullBodyIK.TransformInterpSpeed = FMath::Max(0.001f, FullBodyIK.TransformInterpSpeed);
    FullBodyIK.AlphaInterpSpeed = FMath::Max(0.001f, FullBodyIK.AlphaInterpSpeed);
    FullBodyIK.MaxArmStretchRatio = FMath::Clamp(FullBodyIK.MaxArmStretchRatio, 0.0f, 2.0f);
}
