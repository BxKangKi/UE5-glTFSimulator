// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CharacterComponent.generated.h"

// Character state bit flags used by controller input and movement code.
#define STATE_NONE 0
#define STATE_WATER (1 << 0)
#define STATE_JUMPING (1 << 1)
#define STATE_SPRINT (1 << 2)
#define STATE_CROUCH (1 << 3)
#define STATE_FLYING (1 << 4)
#define STATET_FLOATING (1 << 5)

#define MAX_RAGDOLL_WEIGHT 3.0f

class ACharacterController;
class UCharacterMovementComponent;
class USpringArmComponent;
class USkeletalMeshComponent;

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FCharacterRagdollEnvironmentState
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    bool bIsValid = false;

    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    bool bIsOnGround = false;

    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    bool bIsInWater = false;

    /** True when water is detected but the ragdoll should recover through the land path because it is supported by walkable ground. */
    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    bool bTreatWaterAsGround = false;

    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    bool bShouldRecoverInWater = false;

    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    bool bShouldDelayDeactivation = false;

    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    bool bForcedLandRecovery = false;

    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    bool bMovementWasSwimming = false;

    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    bool bMovementWasFalling = false;

    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    bool bMovementWasOnGround = false;

    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    float WaterLevel = 0.0f;

    /** Average of the current ragdoll body/bone probe positions used for release-state decisions. */
    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    FVector RagdollReferenceLocation = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    FVector RagdollLowestLocation = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    FVector RagdollHighestLocation = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    int32 RagdollProbeLocationCount = 0;

    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    float RagdollMaxSubmersionDepth = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    float RagdollAverageSubmersionDepth = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    int32 RagdollSubmergedProbeCount = 0;

    /** True when core/current ragdoll body probes are meaningfully below the water surface. */
    UPROPERTY(BlueprintReadOnly, Category="Character|Ragdoll")
    bool bRagdollMeaningfullySubmerged = false;
};

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class GLTFSIMULATOR_API UCharacterComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UCharacterComponent();

protected:
    virtual void BeginPlay() override;

public:
    // Called by the controller once per tick to update movement and ragdoll recovery.
    void UpdateComponent(float DeltaTime, const FVector &MoveInput, const int32 CharacterState, const float WaterLevel);

    UFUNCTION(BlueprintCallable)
    void ResetMovementState();

    UFUNCTION(BlueprintCallable)
    void SetRagdollWaterState(bool bInWater, bool bForce = false);

    bool FindRagdollWaterLevel(float &OutWaterLevel) const;

    FVector GetImpactVelocity() const { return ImpactVelocity; }
    bool IsRagdollDamage();

    UFUNCTION(BlueprintCallable)
    void SetRagdollActive(bool bActive);

    UFUNCTION(BlueprintCallable)
    float GetRagdollWeight() const { return RagdollWeight; }

    UFUNCTION(BlueprintCallable)
    bool IsRagdollActive() const { return bIsRagdoll; }

    UFUNCTION(BlueprintCallable)
    bool IsGettingUp() const { return bGettingUp; }

    UFUNCTION(BlueprintCallable)
    bool IsRecoveringRagdollInWater() const
    {
        const bool bWaterIntent = bRagdollInWater || bRagdollRecoveryWantsSwimming;
        return bWaterIntent && (bGettingUp || RagdollRecoverySwimLockTime > 0.0f);
    }

    UFUNCTION(BlueprintCallable)
    bool IsRagdollInWater() const { return IsWaterRagdollAnimationState(); }

    UFUNCTION(BlueprintCallable)
    bool IsWaterRagdollAnimationState() const
    {
        const bool bWaterIntent = bRagdollInWater || bRagdollRecoveryWantsSwimming;
        return bWaterIntent && ((bIsRagdoll || bGettingUp || RagdollWeight > 0.0f) || RagdollRecoverySwimLockTime > 0.0f);
    }

    UFUNCTION(BlueprintCallable)
    bool ShouldKeepSwimmingAfterWaterRagdoll() const
    {
        const bool bWaterIntent = bRagdollInWater || bRagdollRecoveryWantsSwimming;
        return bWaterIntent && (RagdollRecoverySwimLockTime > 0.0f || bIsRagdoll || bGettingUp || RagdollWeight > 0.0f);
    }

    UFUNCTION(BlueprintPure, Category="Character|Ragdoll")
    bool IsRagdollTransitionInProgress() const
    {
        return bIsRagdoll || bGettingUp || RagdollWeight > KINDA_SMALL_NUMBER || bPendingWaterRagdollDeactivation;
    }

    UFUNCTION(BlueprintPure, Category="Character|Ragdoll")
    bool IsLandRagdollRecoveryOverridingWater() const { return bLandRagdollRecoveryOverridesWater; }

    UFUNCTION(BlueprintPure, Category="Character|Ragdoll")
    FCharacterRagdollEnvironmentState GetRagdollEnvironmentState() const { return RagdollEnvironmentState; }

    UFUNCTION(BlueprintPure, Category="Character|Ragdoll")
    bool IsRagdollEnvironmentOnGround() const { return RagdollEnvironmentState.bIsOnGround; }

    UFUNCTION(BlueprintPure, Category="Character|Ragdoll")
    bool IsRagdollEnvironmentInWater() const { return RagdollEnvironmentState.bIsInWater; }

    UFUNCTION(BlueprintPure, Category="Character|Ragdoll")
    bool ShouldTreatRagdollWaterAsGround() const { return RagdollEnvironmentState.bTreatWaterAsGround; }

    UFUNCTION(BlueprintPure, Category="Character|Ragdoll")
    bool ShouldRecoverRagdollInWaterFromEnvironment() const { return RagdollEnvironmentState.bShouldRecoverInWater; }

    /** Clears only the post-recovery swim animation/movement lock. Used when the player explicitly switches to Flying. */
    UFUNCTION(BlueprintCallable)
    void ClearRagdollSwimmingRecoveryLock(bool bKeepCurrentWaterState = true);

    /** Refreshes the ragdoll/recovery water flags immediately before animation variables are read. */
    bool RefreshRagdollWaterStateForAnimation();

    UFUNCTION(BlueprintCallable)
    bool IsLieOnBack() const { return bIsLieOnBack; }

    UFUNCTION(BlueprintCallable)
    bool IsFixedRotation() const { return bFixedRotation; }

    UFUNCTION(BlueprintCallable)
    FVector GetCapturedMeshLocation() const { return CapturedMeshLocation; }

    UFUNCTION(BlueprintCallable)
    FRotator GetCapturedMeshRotation() const { return CapturedMeshRotation; }

    UFUNCTION(BlueprintCallable)
    bool CheckRagdollStay();

private:
    UPROPERTY()
    TObjectPtr<ACharacterController> OwnerCharacter;

    UPROPERTY()
    TObjectPtr<USkeletalMeshComponent> MeshComp;

    UPROPERTY()
    TObjectPtr<USpringArmComponent> SpringArm;

    UPROPERTY()
    TObjectPtr<UCharacterMovementComponent> Movement;

    FVector ImpactVelocity = FVector::ZeroVector;
    FVector CurrentSpeed = FVector::ZeroVector;
    FVector PrevVelocity = FVector::ZeroVector;

    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="1.0"))
    float RagdollGetUpSpeedThreshold = 350.0f;

    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="1.0"))
    float RagdollWaterGetUpSpeedThreshold = 320.0f;

    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="0.0"))
    float RagdollMinimumActiveTime = 2.0f;

    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="0.0"))
    float RagdollLowSpeedConfirmTime = 1.0f;

    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="0.0"))
    float RagdollWaterLowSpeedConfirmTime = 0.75f;

    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="0.1"))
    float RagdollRecoveryBlendDuration = 3.0f;

    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="0.1"))
    float WaterRagdollTransformBlendDuration = 3.0f;

    /** In-water ragdoll recovery should not inherit the tumbling body yaw directly. Keep the actor yaw stable while the mesh blends back. */
    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="0.0"))
    float WaterRagdollStableYawBlendSpeed = 7.5f;

    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="0.0"))
    float WaterRagdollSwimLockAfterRecovery = 0.35f;

    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="0.0"))
    float WaterRecoveryTransformSnapDistance = 0.0f;

    /** Extra vertical offset applied when rebuilding the actor/capsule origin from the ragdoll hips during underwater recovery. Positive values raise the origin. */
    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="-100.0", ClampMax="100.0"))
    float WaterRagdollRecoveryActorZOffset = 0.0f;

    /** The rebuilt actor/capsule origin is allowed to start this far below the water surface before underwater recovery begins. Lower values make the origin recover higher. */
    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="0.0"))
    float WaterRagdollReleaseDepthBelowSurface = 38.0f;

    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="0.0"))
    float WaterRagdollReleaseSinkSpeed = 35.0f;

    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="0.0"))
    float WaterRagdollReleaseSinkForce = 9000.0f;

    /** Water recovery wins over land recovery when the core ragdoll probes are this far below the surface. */
    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="0.0"))
    float RagdollWaterRecoveryCoreDepth = 24.0f;

    /** Average probe depth needed to consider the ragdoll genuinely underwater. */
    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="0.0"))
    float RagdollWaterRecoveryAverageDepth = 10.0f;

    /** Ground recovery is allowed in water only while the ragdoll is shallower than this. */
    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="0.0"))
    float RagdollTreatWaterAsGroundMaxDepth = 14.0f;

    /** Extra depth margin that filters one-frame water-surface noise during the final release decision. */
    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="0.0"))
    float RagdollWaterReleaseHysteresisDepth = 6.0f;

    /** Minimum core probes that must be underwater before a shallow/no-ground release may choose swimming. */
    UPROPERTY(EditAnywhere, Category="Ragdoll|Recovery", meta=(ClampMin="1", ClampMax="8"))
    int32 RagdollWaterReleaseMinCoreProbes = 2;

    float RagdollResistance = 1000.0f;
    float WaterOffset = 0.0f;
    float HalfHeight = 0.0f;
    float Radius = 0.0f;
    float RagdollWeight = 0.0f;
    float RagdollActiveTime = 0.0f;
    float RagdollLowSpeedTime = 0.0f;
    float GetUpActiveTime = 0.0f;
    float RagdollRecoverySwimLockTime = 0.0f;
    float WaterRagdollRecoveryElapsed = 0.0f;
    FVector WaterRecoveryActorStartLocation = FVector::ZeroVector;
    FVector WaterRecoveryActorTargetLocation = FVector::ZeroVector;
    FRotator WaterRecoveryActorStartRotation = FRotator::ZeroRotator;
    FRotator WaterRecoveryActorTargetRotation = FRotator::ZeroRotator;
    FVector WaterRecoveryMeshStartRelativeLocation = FVector::ZeroVector;
    FRotator WaterRecoveryMeshStartRelativeRotation = FRotator::ZeroRotator;
    FRotator RagdollPrePhysicsActorRotation = FRotator::ZeroRotator;

    bool bInvincible = false;
    bool bIsRagdoll = false;
    bool bGettingUp = false;
    bool bCheckingRagdollStay = false;
    bool bIsLieOnBack = false;
    bool bFixedRotation = false;
    bool bRagdollInWater = false;
    bool bRagdollRecoveryWantsSwimming = false;
    bool bWaterRecoveryTransformInitialized = false;
    bool bPendingWaterRagdollDeactivation = false;
    bool bForceLandRagdollRecoveryOnce = false;
    bool bLandRagdollRecoveryOverridesWater = false;
    bool bHasRagdollPrePhysicsActorRotation = false;
    float PendingWaterRagdollDeactivationLevel = 0.0f;
    uint64 RagdollEnvironmentStateFrame = 0;

    UPROPERTY(Transient)
    FCharacterRagdollEnvironmentState RagdollEnvironmentState;

    FTimerHandle RagdollCheckTimerHandle;

    FVector CapturedMeshLocation;
    FRotator CapturedMeshRotation;
    FVector ActorTargetLocation;
    FRotator ActorTargetRotation;

    void ProcessRagdollCheck();
    void ResetRagdollRecoveryState(bool bKeepWaterIntent);
    bool IsRagdollLikeState() const { return bIsRagdoll || bGettingUp || RagdollWeight > 0.0f || bPendingWaterRagdollDeactivation; }
    bool RefreshRagdollWaterDetection(float* OutDetectedWaterLevel = nullptr);
    FCharacterRagdollEnvironmentState UpdateRagdollEnvironmentStateForRelease(float InitialWaterLevel = 0.0f);
    bool ApplyRagdollReleaseEnvironmentStateToOwner(ACharacterController *InOwner, FCharacterRagdollEnvironmentState &ReleaseEnvironmentState);
    bool IsRagdollTouchingWalkableGround(float TraceDistance = 42.0f, bool bCoreOnly = false) const;
    bool ShouldDelayWaterRagdollDeactivation(float WaterLevel) const;
    FVector GetRagdollRecoveryActorLocationFromHips(const FVector& HipsLocation, bool bWaterRecovery) const;
    bool ShouldUseRagdollWaterRecoveryForState(const FCharacterRagdollEnvironmentState& State) const;
    void BeginPendingWaterRagdollDeactivation(float WaterLevel);
    bool UpdatePendingWaterRagdollDeactivation(float DeltaTime, ACharacterController *InOwner, USkeletalMeshComponent *SkeletalMesh);
    void ClearPendingWaterRagdollDeactivation();
    void ActiveRagdoll(ACharacterController *InOwner, USkeletalMeshComponent *SkeletalMesh);
    void DeactiveRagdoll(ACharacterController *InOwner, USkeletalMeshComponent *SkeletalMesh, const FCharacterRagdollEnvironmentState &ReleaseEnvironmentState);
    void FinalizeRagdollRecovery(ACharacterController *InOwner, USkeletalMeshComponent *SkeletalMesh);
    void StartRagdollStayChecking();

    void SetSkeletalMeshLocationAndRotation(USkeletalMeshComponent *SkeletalMesh, const FVector &Location, const FRotator &Rotation, const float InvTime = 0.0f);
    void SetCharacterLocationAndRotation(ACharacterController *InOwner, const FVector &Location, const FRotator &Rotation, const float InvTime = 0.0f);

    FORCEINLINE float CalculateAcceleration(const float A, const float B, const float T);
    FORCEINLINE float ClampGroundSpeed(const float Speed, const float Normal, const float Min);
    void ApplyMoveRightForward(ACharacterController *InOwner, const FRotator &ControlRotation, const FVector &Speed);
    bool CheckIfLieOnBack(const USkeletalMeshComponent *SkeletalMesh);
    float GetMeshForwardYaw(const bool Back, const USkeletalMeshComponent *SkeletalMesh);
    float GetRagdollReleaseSpeedSquared(USkeletalMeshComponent *SkeletalMesh) const;
    void UpdateRagdoll(const float DeltaTime, ACharacterController *InOwner, USkeletalMeshComponent *SkeletalMesh);
    FORCEINLINE FVector CalculateImpactVelocity(const FVector &CurrentVelocity);
};