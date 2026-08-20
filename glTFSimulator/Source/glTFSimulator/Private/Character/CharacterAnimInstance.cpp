// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Character/CharacterAnimInstance.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "System/PhysicsHelper.h"
#include "System/MathHelper.h"
#include "System/MacroLibrary.h"
#include "Character/CharacterController.h"
#include "Character/CharacterComponent.h"
#include "System/GameManagerSubSystem.h"
#include "Weapon/WeaponActor.h"
#include "Components/SkeletalMeshComponent.h"


namespace CharacterAnimTuning
{
    constexpr float MinDivingVelocity = 1000.0f;
    constexpr float GetUpDelay = 0.2f;
    constexpr float SwimVerticalSpeedInterpRate = 7.0f;
    constexpr float SwimVerticalSpeedReturnInterpRate = 9.0f;
    constexpr float SwimVerticalSpeedDeadZone = 0.01f;
}

void UCharacterAnimInstance::ResetRuntimeAnimationState()
{
    Velocity = FVector::ZeroVector;
    Speed = 0.0f;
    MoveSpeed = 0.0f;
    UpSpeed = 0.0f;
    YawAngularVelocity = 0.0f;
    PreviousActorYaw = 0.0f;
    bHasPreviousActorYaw = false;
    bShouldMove = false;
    bIsFlying = false;
    bIsSwimming = false;
    bIsFalling = false;
    bIsGrounded = false;
    bIsCrouch = false;
    bIsDiving = false;
    bIsRagdoll = false;
    bIsGettingUp = false;
    bIsWaterRagdollRecovery = false;
    bRagdollEnvironmentOnGround = false;
    bRagdollEnvironmentInWater = false;
    bTreatRagdollWaterAsGround = false;
    bShouldRecoverRagdollInWater = false;
    RagdollEnvironmentWaterLevel = 0.0f;
    bRagdollMeaningfullySubmerged = false;
    RagdollMaxSubmersionDepth = 0.0f;
    RagdollAverageSubmersionDepth = 0.0f;
    bGetUpTrigger = false;
    IsLieOnBack = 0.0f;
    CapturedMeshLocation = FVector::ZeroVector;
    CapturedMeshRotation = FRotator::ZeroRotator;
    bHasWeaponIK = false;
    WeaponRightHandIKLocationCS = FVector::ZeroVector;
    WeaponRightHandIKRotationCS = FRotator::ZeroRotator;
    WeaponLeftHandIKLocationCS = FVector::ZeroVector;
    WeaponLeftHandIKRotationCS = FRotator::ZeroRotator;
    WeaponMuzzleLocationWS = FVector::ZeroVector;
}


void UCharacterAnimInstance::RefreshWeaponIKState()
{
    bHasWeaponIK = false;
    WeaponRightHandIKLocationCS = FVector::ZeroVector;
    WeaponRightHandIKRotationCS = FRotator::ZeroRotator;
    WeaponLeftHandIKLocationCS = FVector::ZeroVector;
    WeaponLeftHandIKRotationCS = FRotator::ZeroRotator;
    WeaponMuzzleLocationWS = FVector::ZeroVector;

    USkeletalMeshComponent* MeshComponent = GetSkelMeshComponent();
    UWorld* World = GetWorld();
    if (!IsValid(MeshComponent) || !IsValid(World))
    {
        return;
    }

    UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(World);
    AWeaponActor* Weapon = Manager ? Manager->GetEquippedWeaponActor() : nullptr;
    if (!IsValid(Weapon))
    {
        return;
    }

    const FTransform MeshToWorld = MeshComponent->GetComponentTransform();
    const FTransform RightHandCS = Weapon->GetRightHandIKWorldTransform().GetRelativeTransform(MeshToWorld);
    const FTransform LeftHandCS = Weapon->GetLeftHandIKWorldTransform().GetRelativeTransform(MeshToWorld);

    bHasWeaponIK = true;
    WeaponRightHandIKLocationCS = RightHandCS.GetLocation();
    WeaponRightHandIKRotationCS = RightHandCS.Rotator();
    WeaponLeftHandIKLocationCS = LeftHandCS.GetLocation();
    WeaponLeftHandIKRotationCS = LeftHandCS.Rotator();
    WeaponMuzzleLocationWS = Weapon->GetMuzzleWorldLocation();
}

void UCharacterAnimInstance::RefreshCachedReferences()
{
    AActor* Owner = GetOwningActor();
    ACharacterController* Character = IsValid(Owner) ? Cast<ACharacterController>(Owner) : nullptr;
    if (!IsValid(Character))
    {
        Component = nullptr;
        Movement = nullptr;
        return;
    }

    Component = Character->GetCharacterComponent();
    Movement = Character->GetCharacterMovement();
}

void UCharacterAnimInstance::RefreshCharacterAnimationState(float DeltaSeconds)
{
    // 1. Always re-cache the owner/component pair. Runtime mesh replacement can recreate
    // the AnimInstance or swap ownership while the old weak-looking UObject pointers are
    // still technically valid for a frame; relying on the cached pair can leave the AnimBP
    // with stale ragdoll/get-up variables.
    RefreshCachedReferences();

    if (!IsValid(Movement.Get()) || !IsValid(Component.Get()))
    {
        ResetRuntimeAnimationState();
        return;
    }

    // 2. Calculate signed yaw angular velocity for turn animation blending.
    if (const AActor* Owner = GetOwningActor())
    {
        const float CurrentActorYaw = Owner->GetActorRotation().Yaw;
        YawAngularVelocity = (bHasPreviousActorYaw && DeltaSeconds > SMALL_NUMBER)
            ? FMath::FindDeltaAngleDegrees(PreviousActorYaw, CurrentActorYaw) / DeltaSeconds
            : 0.0f;
        PreviousActorYaw = CurrentActorYaw;
        bHasPreviousActorYaw = true;
    }
    else
    {
        YawAngularVelocity = 0.0f;
        bHasPreviousActorYaw = false;
    }

    RotationSpeed = FMath::Clamp(YawAngularVelocity * 0.00556f, -1.0f, 1.0f);

    // 3. Refresh the filtered ragdoll water snapshot before animation variables are read.
    Component->RefreshRagdollWaterStateForAnimation();

    // 4. Cache physics data once to avoid duplicate movement queries.
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
    bGetUpTrigger = bIsGettingUp && Component->GetRagdollWeight() < (CharacterConstants::MaxRagdollWeight - CharacterAnimTuning::GetUpDelay);
    IsLieOnBack = Component->IsLieOnBack() ? 1.0f: 0.0f;
    CapturedMeshLocation = Component->GetCapturedMeshLocation();
    CapturedMeshRotation = Component->GetCapturedMeshRotation();
    RefreshWeaponIKState();

    // 6. Movement intent: acceleration must be non-zero and speed must be visible.
    // Built-in IsNearlyZero keeps this branch cheap and readable.
    bShouldMove = (!CurrentAccel.IsNearlyZero() && Speed > 3.0f);

    // 7. Vertical swim ratio with temporal smoothing.
    // Surface correction can clamp Velocity.Z instantly, but the animation graph
    // should see a continuous up/down value so swim poses do not pop on sharp input
    // changes, ceiling hits, or mode transitions.
    const float MaxSwim = Movement->MaxSwimSpeed;
    const float TargetUpSpeed = (bIsSwimming && MaxSwim > KINDA_SMALL_NUMBER)
        ? FMath::Clamp(Velocity.Z / MaxSwim, -0.9f, 0.9f)
        : 0.0f;
    const float UpSpeedInterpRate = bIsSwimming
        ? CharacterAnimTuning::SwimVerticalSpeedInterpRate
        : CharacterAnimTuning::SwimVerticalSpeedReturnInterpRate;
    UpSpeed = FMath::FInterpTo(UpSpeed, TargetUpSpeed, DeltaSeconds, UpSpeedInterpRate);
    if (FMath::Abs(UpSpeed) < CharacterAnimTuning::SwimVerticalSpeedDeadZone
        && FMath::Abs(TargetUpSpeed) < CharacterAnimTuning::SwimVerticalSpeedDeadZone)
    {
        UpSpeed = 0.0f;
    }

    // 8. Diving state and ground trace.
    // Keep owner lookup inside the branch so normal animation frames do no extra work.

    if (!bWaterRagdollState && bIsSwimming && Velocity.Z < -CharacterAnimTuning::MinDivingVelocity)
    {
        // Use the already-known movement location as the trace start.
        const FVector Start = Movement->GetActorLocation();
        // End is derived from current downward speed so fast dives trace farther.
        const FVector End = Start + (FVector::UpVector * -(Velocity.Z * DeltaSeconds + CharacterAnimTuning::MinDivingVelocity));

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

void UCharacterAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
    RefreshCharacterAnimationState(DeltaSeconds);
}

void UCharacterAnimInstance::NativeInitializeAnimation()
{
    ResetRuntimeAnimationState();
    RefreshCachedReferences();
    RefreshCharacterAnimationState(0.0f);
}
