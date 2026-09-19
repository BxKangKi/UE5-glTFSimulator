// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "CharacterControllerMovementComponent.generated.h"

class APhysicsVolume;

/**
 * Character movement implementation used by glTFSimulator.
 *
 * The stock MOVE_Swimming implementation assumes that the character is inside an
 * APhysicsVolume whose bWaterVolume flag is set. glTFSimulator deliberately keeps
 * global/local water gameplay queries independent from PhysicsVolume so the same
 * UWaterQuerySubsystem can be used by characters, ragdolls, vehicles and props.
 *
 * When gameplay has already selected MOVE_Swimming, this component therefore uses
 * collision-safe 3D flying physics while keeping MovementMode == MOVE_Swimming.
 * MaxSwimSpeed / BrakingDecelerationSwimming / acceleration are still selected from
 * the normal CharacterMovement swimming settings because the movement mode remains
 * MOVE_Swimming throughout the physics step.
 */
UCLASS(ClassGroup=(Movement), meta=(BlueprintSpawnableComponent))
class GLTFSIMULATOR_API UCharacterControllerMovementComponent : public UCharacterMovementComponent
{
    GENERATED_BODY()

protected:
    virtual void PhysSwimming(float DeltaTime, int32 Iterations) override;
    virtual void PhysicsVolumeChanged(APhysicsVolume* NewVolume) override;
};
