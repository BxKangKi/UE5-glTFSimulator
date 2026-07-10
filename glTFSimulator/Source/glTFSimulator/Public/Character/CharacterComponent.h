// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "CharacterComponent.generated.h"

// Character state bit flags used by controller input and movement code.
#define STATE_NONE 0
#define STATE_WATER (1 << 0)
#define STATE_JUMPING (1 << 1)
#define STATE_SPRINT (1 << 2)
#define STATE_CROUCH (1 << 3)
#define STATE_FLYING (1 << 4)
#define STATET_FLOATING (1 << 5)

namespace CharacterConstants
{
    static constexpr float MaxRagdollWeight = 3.0f;
}

class ACharacterController;
class UCharacterMovementComponent;
class USpringArmComponent;
class USkeletalMeshComponent;
struct FTraceDatum;
struct FTraceHandle;
struct FHitResult;

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

    /** Clears only the transient water-surface clamp without stopping all movement. */
    void ClearSwimmingSurfaceConstraintState();

    /** Clears any cached waterline reference while a new mesh is still loading. */
    void InvalidateWaterReferenceForPendingMeshLoad();

    /** Schedules waterline reference sampling after the final mesh has loaded and produced bone transforms. */
    void RequestWaterReferenceRefreshAfterMeshLoad();

    /** Re-samples BONE_HEAD from the loaded mesh, then stores a stable capsule-local water reference. */
    void RefreshWaterReferenceOffsetFromHead();

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

    /**
     * Returns water depth against the cached head/fallback point.
     * The head bone is sampled once after mesh load so animation bobbing cannot flicker Swimming.
     */
    float GetDirectWaterImmersionDepth(float InWaterLevel) const;

    /** Builds the stable cached head/fallback point from the capsule-local offset. */
    bool TryGetStableWaterReferenceLocation(FVector& OutLocation) const;
    bool TryGetWaterReferenceOrCapsuleFallbackLocation(FVector& OutLocation) const;

    /** Computes swim enter, ceiling padding, and surface-correction depths from fixed native ratios and capsule size. */
    void GetCapsuleSwimmingDepths(float& OutEnterDepth, float& OutExitDepth, float& OutSurfaceLockDepth) const;

    /** Uses hysteresis so shallow surface touches do not immediately enter Swimming, but real swimming does not flicker off. */
    bool ShouldUseDirectWaterState(float InWaterLevel, bool bCurrentlySwimming) const;

    /** Returns true when a raised, self-ignored trace finds walkable support close to the capsule. */
    bool IsCharacterSupportedByWalkableGround(float ExtraDownDistance = 24.0f) const;

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
    FVector LastPreRagdollVelocity = FVector::ZeroVector;
    float LastPreRagdollVelocityAge = TNumericLimits<float>::Max();


    /** Stable capsule-local head/fallback offset sampled only after the final runtime mesh has loaded. */
    FVector WaterReferenceOffsetFromCapsule = FVector::ZeroVector;

    /** True after WaterReferenceOffsetFromCapsule has been initialized from loaded-mesh head/capsule fallback data. */
    bool bHasWaterReferenceOffsetFromCapsule = false;

    /** True only after the final mesh-load completion path authorizes waterline sampling. */
    bool bWaterReferenceMeshLoadComplete = false;

    /** Latched while the stable head reference is at the visible surface ceiling, so held upward input cannot punch through the cap. */
    bool bSwimmingSurfaceCeilingLocked = false;

    // Swimming surface thresholds are fixed native constants in CharacterComponent.cpp.
    // They are intentionally not UPROPERTY values because they are gameplay invariants
    // derived from capsule size, not per-character tuning knobs.

    // Ragdoll/water recovery tuning is fixed in CharacterComponent.cpp as native constants.
    // These values are gameplay invariants, not per-character Blueprint knobs.

    float RagdollResistance = 1000.0f;
    float WaterOffset = 0.0f;
    float HalfHeight = 0.0f;
    float Radius = 0.0f;
    FVector StandingMeshRelativeLocation = FVector(0.0f, 0.0f, -90.0f);
    FRotator StandingMeshRelativeRotation = FRotator(0.0f, 270.0f, 0.0f);
    float StandingCapsuleHalfHeight = 0.0f;
    float StandingCapsuleRadius = 0.0f;
    bool bCrouchVisualOffsetApplied = false;

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
    float RagdollCameraStabilizeRemainingTime = 0.0f;
    bool bSavedRagdollCameraState = false;
    bool bSavedSpringArmCameraLag = false;
    bool bSavedSpringArmCollisionTest = true;
    bool bSavedSpringArmUseCameraLagSubstepping = false;
    float SavedSpringArmCameraLagSpeed = 0.0f;
    float SavedSpringArmCameraLagMaxTimeStep = 0.0f;
    float SavedSpringArmCameraLagMaxDistance = 0.0f;
    bool bSavedRagdollCapsuleCollisionState = false;
    bool bSavedRagdollCapsuleGenerateOverlapEvents = true;
    ECollisionEnabled::Type SavedRagdollCapsuleCollisionEnabled = ECollisionEnabled::QueryAndPhysics;


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
    uint32 RagdollReleaseGroundTraceRequestId = 0;
    int32 PendingRagdollReleaseGroundTraceCount = 0;
    bool bRagdollReleaseGroundTraceInFlight = false;
    bool bRagdollReleaseGroundTraceHitWalkable = false;
    bool bUseAsyncRagdollReleaseGroundResult = false;
    bool bAsyncRagdollReleaseGroundResult = false;

    UPROPERTY(Transient)
    FCharacterRagdollEnvironmentState RagdollEnvironmentState;

    FTimerHandle RagdollCheckTimerHandle;

    FVector CapturedMeshLocation;
    FRotator CapturedMeshRotation;
    FVector ActorTargetLocation;
    FRotator ActorTargetRotation;

    void InitializeCrouchSettings();
    void ApplyCrouchState(bool bShouldCrouch);
    void ProcessRagdollCheck();
    void ClearRagdollWaterIntent(bool bClearSwimLock = true);
    void SetMovementModeAfterRagdollRecovery(UCharacterMovementComponent* CharacterMovement, const FCharacterRagdollEnvironmentState& RecoveryEnvironmentState) const;
    void ResetRagdollRecoveryState(bool bKeepWaterIntent);
    bool TryGetHeadWaterReferenceLocation(FVector& OutLocation) const;
    float GetDirectWaterCapsuleImmersionDepth(float InWaterLevel) const;
    float GetStableHeadEmergenceHeight() const;
    float GetSwimEntryReferenceDepth() const;
    bool IsCapsuleAtLeastHalfSubmerged(float InWaterLevel) const;
    bool HasStableHeadReachedShoreExitLine(float DirectImmersionDepth) const;
    float GetGroundedWaterSwimOverrideDepth() const;
    float GetGroundedWaterWalkSpeedMultiplier(float InWaterLevel) const;
    bool TraceCharacterWalkableGroundFromAbove(FHitResult& OutHit, float ExtraDownDistance = 0.0f) const;
    bool IsGroundSupportBlockingDirectWater(float InWaterLevel, float DirectImmersionDepth, bool bCurrentlySwimming) const;
    bool IsRagdollLikeState() const { return bIsRagdoll || bGettingUp || RagdollWeight > 0.0f || bPendingWaterRagdollDeactivation; }
    bool RefreshRagdollWaterDetection(float* OutDetectedWaterLevel = nullptr);
    FCharacterRagdollEnvironmentState UpdateRagdollEnvironmentStateForRelease(float InitialWaterLevel = 0.0f, bool bUseGroundOverride = false, bool bGroundOverride = false);
    bool ApplyRagdollReleaseEnvironmentStateToOwner(ACharacterController *InOwner, FCharacterRagdollEnvironmentState &ReleaseEnvironmentState);
    bool IsRagdollTouchingWalkableGround(float TraceDistance = 42.0f, bool bCoreOnly = false) const;
    void RequestAsyncRagdollReleaseGroundTrace();
    void OnAsyncRagdollReleaseGroundTraceCompleted(const FTraceHandle& TraceHandle, FTraceDatum& TraceDatum);
    void FinishAsyncRagdollReleaseGroundTrace(bool bWalkableGround);
    bool ShouldDelayWaterRagdollDeactivation(float WaterLevel, bool bKnownWalkableGround) const;
    FVector GetRagdollRecoveryActorLocationFromHips(const FVector& HipsLocation, bool bWaterRecovery) const;
    FVector ResolveRagdollRecoveryGroundPenetration(const FVector& DesiredActorLocation) const;
    bool ShouldUseRagdollWaterRecoveryForState(const FCharacterRagdollEnvironmentState& State) const;
    void BeginPendingWaterRagdollDeactivation(float WaterLevel);
    bool UpdatePendingWaterRagdollDeactivation(float DeltaTime, ACharacterController *InOwner, USkeletalMeshComponent *SkeletalMesh);
    void ClearPendingWaterRagdollDeactivation();
    void UpdateRagdollVelocityHistory(float DeltaTime, const FVector& CurrentVelocity);
    FVector CapturePreRagdollVelocity(ACharacterController *InOwner, UCharacterMovementComponent *CharacterMovement) const;
    FVector GetInitialRagdollActivationVelocity(ACharacterController *InOwner, UCharacterMovementComponent *CharacterMovement, USkeletalMeshComponent *SkeletalMesh) const;
    void ApplyInitialRagdollVelocity(USkeletalMeshComponent *SkeletalMesh, const FVector &InitialVelocity) const;
    void BeginRagdollCameraStabilization();
    void UpdateRagdollCameraStabilization(float DeltaTime);
    void RestoreRagdollCameraState();
    void BeginRagdollCapsuleCollisionIsolation();
    void RestoreRagdollCapsuleCollision();
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