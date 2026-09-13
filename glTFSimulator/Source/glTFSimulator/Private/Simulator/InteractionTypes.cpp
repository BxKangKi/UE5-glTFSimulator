/**
 * @file InteractionTypes.cpp
 * 역할: 장비·객체 상호작용 공통 타입을 정의합니다.
 * 핵심 기능: 부착 위치·상호작용 상태 표현.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
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
