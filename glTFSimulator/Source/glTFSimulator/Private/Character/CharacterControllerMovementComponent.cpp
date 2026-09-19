// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Character/CharacterControllerMovementComponent.h"
#include "GameFramework/PhysicsVolume.h"
#include "Character/CharacterComponent.h"
#include "GameFramework/Character.h"

void UCharacterControllerMovementComponent::PhysSwimming(const float DeltaTime, const int32 Iterations)
{
    if (DeltaTime <= UE_SMALL_NUMBER)
    {
        return;
    }

    // Physics/recovery owns the capsule anchor in this interval. Keep the Swimming mode
    // for animation/water queries, but do not run a second movement integrator.
    const UCharacterComponent* State = CharacterOwner ? CharacterOwner->FindComponentByClass<UCharacterComponent>() : nullptr;
    if (State && State->IsRagdollTransitionInProgress())
    {
        StopMovementImmediately();
        ClearAccumulatedForces();
        ConsumeInputVector();
        return;
    }

    // UCharacterMovementComponent::PhysSwimming ultimately uses the current
    // PhysicsVolume's bWaterVolume flag (including inside Swim()) to decide that
    // the character has left water. glTFSimulator water is intentionally queried
    // through UWaterQuerySubsystem instead, so calling the stock implementation
    // would make MOVE_Swimming oscillate back to Falling every movement tick.
    //
    // PhysFlying already provides the collision-safe unrestricted 3D movement we
    // need here and does not require a water PhysicsVolume. MovementMode remains
    // MOVE_Swimming, therefore GetMaxSpeed()/GetMaxBrakingDeceleration() continue
    // to use MaxSwimSpeed and BrakingDecelerationSwimming rather than flying values.
    PhysFlying(DeltaTime, Iterations);
}

void UCharacterControllerMovementComponent::PhysicsVolumeChanged(APhysicsVolume* NewVolume)
{
    if (MovementMode == MOVE_Swimming && (!NewVolume || !NewVolume->bWaterVolume))
    {
        // The authoritative exit decision is made by ACharacterController using
        // UWaterQuerySubsystem. The stock implementation would immediately switch
        // MOVE_Swimming to MOVE_Falling merely because this is not a water volume.
        return;
    }

    Super::PhysicsVolumeChanged(NewVolume);
}
