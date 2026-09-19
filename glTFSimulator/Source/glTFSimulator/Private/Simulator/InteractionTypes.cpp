/**
 * @file InteractionTypes.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Simulator/InteractionTypes.h"

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
}
