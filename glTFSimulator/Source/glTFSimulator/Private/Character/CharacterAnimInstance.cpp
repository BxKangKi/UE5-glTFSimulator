// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Character/CharacterAnimInstance.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "System/PhysicsHelper.h"
#include "System/MacroLibrary.h"
#include "Character/CharacterController.h"
#include "Character/CharacterComponent.h"

#define MIN_DIVING_VELOCITY 1000.0f
#define GET_UP_DELAY 0.2f

void UCharacterAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
    // 1. Validate and cache pointers/variables used by the animation graph.
    if (!IsValid(Movement) || !IsValid(Component.Get()))
        return;

    // 2. Refresh the filtered ragdoll water snapshot before animation variables are read.
    Component->RefreshRagdollWaterStateForAnimation();

    // 3. Cache physics data once to avoid duplicate movement queries.
    const FVector CurrentVelocity = Movement->Velocity;
    const FVector CurrentAccel = Movement->GetCurrentAcceleration();

    // 4. Compute speed values with built-in vector helpers.
    // Size2D() avoids custom XY-length code and is easier to read.
    Velocity = CurrentVelocity;
    Speed = Velocity.Size2D();
    MoveSpeed = Velocity.Size();

    // 5. Update state flags directly from CharacterMovement and the filtered ragdoll snapshot.
    bIsRagdoll = Component->IsRagdollActive();
    const bool bRagdollLikeState = bIsRagdoll || Component->IsGettingUp() || Component->GetRagdollWeight() > KINDA_SMALL_NUMBER;
    const FCharacterRagdollEnvironmentState RagdollEnvironmentState = Component->GetRagdollEnvironmentState();
    bRagdollEnvironmentOnGround = RagdollEnvironmentState.bIsOnGround;
    bRagdollEnvironmentInWater = RagdollEnvironmentState.bIsInWater;
    bTreatRagdollWaterAsGround = RagdollEnvironmentState.bTreatWaterAsGround;
    bShouldRecoverRagdollInWater = RagdollEnvironmentState.bShouldRecoverInWater;
    RagdollEnvironmentWaterLevel = RagdollEnvironmentState.WaterLevel;
    bRagdollMeaningfullySubmerged = RagdollEnvironmentState.bRagdollMeaningfullySubmerged;
    RagdollMaxSubmersionDepth = RagdollEnvironmentState.RagdollMaxSubmersionDepth;
    RagdollAverageSubmersionDepth = RagdollEnvironmentState.RagdollAverageSubmersionDepth;

    float DetectedRagdollWaterLevel = RagdollEnvironmentState.WaterLevel;
    const bool bRagdollWaterTreatedAsGround = bRagdollLikeState && RagdollEnvironmentState.bTreatWaterAsGround;
    const bool bRagdollBodyDetectedInWater = bRagdollLikeState && !bRagdollWaterTreatedAsGround && Component->FindRagdollWaterLevel(DetectedRagdollWaterLevel);
    const bool bMovementCurrentlyFlying = Movement->IsFlying();
    const bool bMovementCurrentlySwimming = Movement->IsSwimming();
    const bool bCurrentWaterAnimationAllowed = !bMovementCurrentlyFlying
        && !bRagdollWaterTreatedAsGround
        && (bRagdollBodyDetectedInWater || (bMovementCurrentlySwimming && !Movement->IsMovingOnGround()));
    const bool bComponentWaterRagdollState = Component->IsWaterRagdollAnimationState() && bCurrentWaterAnimationAllowed;
    const bool bKeepSwimmingAfterWaterRagdoll = Component->ShouldKeepSwimmingAfterWaterRagdoll() && bCurrentWaterAnimationAllowed;
    const bool bMovementSwimming = !bMovementCurrentlyFlying
        && bMovementCurrentlySwimming
        && (!bRagdollLikeState || bComponentWaterRagdollState || bRagdollBodyDetectedInWater || bKeepSwimmingAfterWaterRagdoll);
    const bool bWaterRagdollState = bComponentWaterRagdollState
        || bRagdollBodyDetectedInWater
        || (bRagdollLikeState && bMovementSwimming)
        || bKeepSwimmingAfterWaterRagdoll;
    const bool bWaterRagdollRecovery = (Component->IsRecoveringRagdollInWater() && bCurrentWaterAnimationAllowed) || (Component->IsGettingUp() && bWaterRagdollState);
    bIsWaterRagdollRecovery = bWaterRagdollRecovery;
    bIsSwimming = !bMovementCurrentlyFlying && (bMovementSwimming || bWaterRagdollState || bKeepSwimmingAfterWaterRagdoll);
    bIsFlying = bMovementCurrentlyFlying;
    bIsGrounded = Movement->IsMovingOnGround() || (bRagdollLikeState && RagdollEnvironmentState.bIsOnGround && !RagdollEnvironmentState.bShouldRecoverInWater);
    bIsFalling = Movement->IsFalling() && !bIsSwimming && !bIsFlying && !bIsGrounded;
    bIsCrouch = Movement->IsCrouching();
    // AnimBP now handles water recovery explicitly. Keep GetUp true underwater too,
    // while bIsSwimming/bIsFalling above keep the transition out of the falling branch.
    bIsGettingUp = Component->IsGettingUp();
    bGetUpTrigger = bIsGettingUp && Component->GetRagdollWeight() < (MAX_RAGDOLL_WEIGHT - GET_UP_DELAY);
    IsLieOnBack = Component->IsLieOnBack() ? 1.0f: 0.0f;
    CapturedMeshLocation = Component->GetCapturedMeshLocation();
    CapturedMeshRotation = Component->GetCapturedMeshRotation();

    // 6. Movement intent: acceleration must be non-zero and speed must be visible.
    // Built-in IsNearlyZero keeps this branch cheap and readable.
    bShouldMove = (!CurrentAccel.IsNearlyZero() && Speed > 3.0f);

    // 7. Vertical swim ratio with a guarded divide.
    const float MaxSwim = Movement->MaxSwimSpeed;
    // A direct zero check is faster than a generic safe-divide helper.
    UpSpeed = (MaxSwim > KINDA_SMALL_NUMBER)
                  ? FMath::Clamp(Velocity.Z / MaxSwim, -0.9f, 0.9f)
                  : 0.0f;

    // 8. Diving state and ground trace.
    // Keep owner lookup inside the branch so normal animation frames do no extra work.

    if (!bWaterRagdollState && bIsSwimming && Velocity.Z < -MIN_DIVING_VELOCITY)
    {
        // Use the already-known movement location as the trace start.
        const FVector Start = Movement->GetActorLocation();
        // End is derived from current downward speed so fast dives trace farther.
        const FVector End = Start + (FVector::UpVector * -(Velocity.Z * DeltaSeconds + MIN_DIVING_VELOCITY));

        // Cache CharacterOwner once for the raycast.
        if (ACharacter *Owner = Movement->GetCharacterOwner())
        {
            if(FPhysicsHelper::Raycast(Owner, Start, End))
            {
                bIsDiving = false;
            }
            else
            {
                bIsDiving = true;
            }
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("Diving Ground Check: CharacterMovement's Owner Actor is nullptr."));
            bIsDiving = false;
        }
    }
    else
    {
        bIsDiving = false; // Reset when the dive condition is not active.
    }
}

void UCharacterAnimInstance::NativeInitializeAnimation()
{
    AActor *Owner = GetOwningActor();
    if (!IsValid(Owner))
        return;
    ACharacterController *Character = Cast<ACharacterController>(Owner);
    if (!IsValid(Character))
        return;
    Component = Character->GetCharacterComponent();
    Movement = Character->GetCharacterMovement();
}