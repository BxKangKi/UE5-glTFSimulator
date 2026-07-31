// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Character/CharacterComponent.h"
#include "Character/CharacterController.h"
#include "Character/CharacterFunctionLibrary.h"
#include "Character/CharacterAnimInstance.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "System/ActorHelper.h"
#include "System/MacroLibrary.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "TimerManager.h"
#include "Engine/World.h"
#include "PhysicsEngine/BodyInstance.h"
#include "World/WaterActor.h"
#include "CollisionQueryParams.h"
#include "CoreGlobals.h"

using FRagdollProbeLocationArray = TArray<FVector, TInlineAllocator<16>>;
using FRagdollSmallProbeLocationArray = TArray<FVector, TInlineAllocator<4>>;

namespace CharacterRagdollTuning
{
    static const FName PoseSnapshotName(TEXT("RagdollPose"));
    static const FVector MeshRecoveryRelativeLocation(0.0f, 0.0f, -90.0f);
    static const FRotator MeshRecoveryRelativeRotation(0.0f, 270.0f, 0.0f);

    constexpr float MaxBlendWeight = 1.0f;
    constexpr float CameraLagSpeed = 6.0f;
    constexpr float GetUpSpeedThreshold = 350.0f;
    constexpr float WaterGetUpSpeedThreshold = 320.0f;
    constexpr float MinimumActiveTime = 2.0f;
    constexpr float LowSpeedConfirmTime = 1.0f;
    constexpr float WaterLowSpeedConfirmTime = 0.75f;
    constexpr float WaterTransformBlendDuration = 3.0f;
    constexpr float WaterStableYawBlendSpeed = 7.5f;
    constexpr float WaterSwimLockAfterRecovery = 0.35f;
    constexpr float WaterRecoveryActorZOffset = 0.0f;
    constexpr float WaterReleaseDepthBelowSurface = 38.0f;
    constexpr float WaterReleaseSinkSpeed = 35.0f;
    constexpr float WaterReleaseSinkForce = 9000.0f;
    constexpr float WaterRecoveryCoreDepth = 24.0f;
    constexpr float WaterRecoveryAverageDepth = 10.0f;
    constexpr float TreatWaterAsGroundMaxDepth = 14.0f;
    constexpr float WaterReleaseHysteresisDepth = 6.0f;
    constexpr float InitialRagdollVelocityDeadZone = 1.0f;
    constexpr float MeaningfulVelocityDeadZone = 2.0f;
    constexpr float PreRagdollVelocityGraceSeconds = 0.20f;
    constexpr float PreRagdollVelocityDropPreserveRatio = 0.75f;
    constexpr float MaxInitialRagdollSpeed = 10000.0f;
    constexpr float InitialRagdollVelocityScale = 1.0f;
    constexpr float MaxInitialRagdollPlanarSpeed = 10000.0f;
    constexpr float MaxInitialRagdollUpSpeed = 5000.0f;
    constexpr float MaxInitialRagdollDownSpeed = 10000.0f;
    constexpr float RagdollCameraStabilizeDuration = 0.0f;
    constexpr float RagdollCameraAnchorDeadZone = 0.25f;
    constexpr float RagdollSpringArmLagSpeed = 3.75f;
    constexpr float RagdollSpringArmMaxLagDistance = 0.0f;
    constexpr float RagdollSpringArmLagMaxTimeStep = 1.0f / 90.0f;
    constexpr float LandRecoveryGroundClearance = 6.0f;
    constexpr float LandRecoveryGroundProbeUp = 96.0f;
    constexpr float LandRecoveryGroundProbeDown = 320.0f;
    constexpr int32 WaterReleaseMinCoreProbes = 2;
}

namespace CharacterMovementTuning
{
    constexpr float BaseAccelerationTimeScale = 5.0f;
    constexpr float FlyingRagdollResistance = 1000000.0f;
    constexpr float GroundWaterRagdollResistance = 1200.0f;
    constexpr float FlyingMaxAcceleration = 15000.0f;
    constexpr float FlyingMaxSpeed = 3000.0f;
    constexpr float FlyingSprintMaxSpeed = 15000.0f;
    constexpr float SwimmingLinearResistance = 4.75f;
    constexpr float SwimmingQuadraticResistanceScale = 0.006f;
    constexpr float SwimmingMinBrakingDeceleration = 80.0f;
    constexpr float SwimmingMaxBrakingDeceleration = 9000.0f;
    constexpr float SwimmingMaxAcceleration = 2200.0f;
    constexpr float SwimmingMaxSpeed = 210.0f;
    constexpr float SwimmingSprintMaxSpeed = 420.0f;
    constexpr float SwimmingInputDeadZone = 0.05f;
    constexpr float SwimmingAccelerationTimeScale = 0.58f;
    constexpr float SwimmingBrakeTimeScale = 1.35f;
    constexpr float DryAirborneWalkSpeed = 100.0f;
    constexpr float GroundWalkSpeed = 200.0f;
    constexpr float GroundSprintSpeed = 533.3f;
}

namespace CharacterWaterTuning
{
    // Native waterline constants. These are derived from the capsule and stable
    // head reference every frame. They are intentionally not exposed as Blueprint
    // knobs, because desynced values can make the character walk while submerged
    // or keep the head trapped under the surface.
    constexpr float ReferenceMinHalfHeightRatio = 0.48f;
    constexpr float ReferenceMaxHalfHeightRatio = 0.90f;
    constexpr float ReferenceTopMarginRadiusRatio = 0.18f;
    constexpr float FallbackHeadOffsetHalfHeightRatio = 0.86f;
    constexpr float SwimSurfaceCaptureDepthRadiusRatio = 0.95f;
    constexpr float SwimExitPaddingRadiusRatio = 0.32f;
    constexpr float HeadSurfaceClearanceRadiusRatio = 1.12f;
    constexpr float HeadSurfaceClearanceMinCm = 38.0f;
    constexpr float SurfaceLockDepthRadiusRatio = 1.10f;
    constexpr float GroundedSwimOverrideDepthRadiusRatio = 1.25f;
    constexpr float CapsuleSwimLockSubmergedRatio = 0.50f;
    constexpr float CapsuleSwimLockHysteresisCm = 2.0f;
    constexpr float SurfaceRiseAssistDeadZoneCm = 4.0f;
    constexpr float SurfaceRiseAssistVelocityPerCm = 4.0f;
    constexpr float SurfaceRiseAssistMinUpSpeed = 0.0f;
    constexpr float SurfaceRiseAssistMaxUpSpeed = 160.0f;
    constexpr float SurfaceRiseAssistInterpSpeed = 3.0f;
    constexpr float ShoreExitHeadClearanceRatio = 0.18f;
    constexpr float ShoreExitGroundSurfaceMarginRadiusRatio = 0.65f;
    constexpr float ShoreExitGroundSurfaceMinMarginCm = 24.0f;
    constexpr float SlopeExitSurfaceBypassMarginRadiusRatio = 0.72f;
    constexpr float SlopeExitSurfaceBypassMinMarginCm = 26.0f;
    constexpr float SlopeExitNormalZMin = 0.20f;
    constexpr float SlopeExitNormalZMax = 0.97f;
    constexpr float WalkWaterSlowStartSubmergedRatio = 0.06f;
    constexpr float WalkWaterSlowFullSubmergedRatio = 0.82f;
    constexpr float WalkWaterMinSpeedMultiplier = 0.42f;
}

static FORCEINLINE FVector MakeForwardVectorFromYaw(const float YawDegrees)
{
    float SinYaw = 0.0f;
    float CosYaw = 1.0f;
    FMath::SinCos(&SinYaw, &CosYaw, FMath::DegreesToRadians(YawDegrees));
    return FVector(CosYaw, SinYaw, 0.0f);
}

static FORCEINLINE FVector MakeRightVectorFromYaw(const float YawDegrees)
{
    float SinYaw = 0.0f;
    float CosYaw = 1.0f;
    FMath::SinCos(&SinYaw, &CosYaw, FMath::DegreesToRadians(YawDegrees));
    return FVector(-SinYaw, CosYaw, 0.0f);
}

static FORCEINLINE void StopMovementAndSetMode(UCharacterMovementComponent* Movement, const EMovementMode NewMode)
{
    if (!IsValid(Movement))
    {
        return;
    }

    Movement->StopMovementImmediately();
    if (Movement->MovementMode != NewMode)
    {
        Movement->SetMovementMode(NewMode);
    }
}

static FORCEINLINE void StopMovementAndDisable(UCharacterMovementComponent* Movement)
{
    if (!IsValid(Movement))
    {
        return;
    }

    Movement->StopMovementImmediately();
    Movement->DisableMovement();
}

static FRotator MakeFlatYawRotation(const float Yaw)
{
    return FRotator(0.0f, FRotator::NormalizeAxis(Yaw), 0.0f);
}

static FRotator MakeFlatYawRotationNear(const float DesiredYaw, const FRotator& ReferenceRotation)
{
    const float ReferenceYaw = FRotator::NormalizeAxis(ReferenceRotation.Yaw);
    const float DeltaYaw = FMath::FindDeltaAngleDegrees(ReferenceYaw, DesiredYaw);
    return FRotator(0.0f, ReferenceYaw + DeltaYaw, 0.0f);
}

static float ComputeExponentialDampingFactor(const float DampingRate, const float DeltaTime)
{
    return FMath::Exp(-FMath::Max(0.0f, DampingRate) * FMath::Max(0.0f, DeltaTime));
}

static FORCEINLINE float ComputeCapsuleWaterImmersionDepth(
    const FVector& ActorLocation,
    const float CapsuleHalfHeight,
    const float WaterLevel)
{
    // Positive depth means the water surface is above the capsule bottom.  This is
    // more stable than using the capsule origin or a one-frame overlap flag near the surface.
    return WaterLevel - (ActorLocation.Z - FMath::Max(0.0f, CapsuleHalfHeight));
}

static void GetWaterReferenceZBounds(
    const float CapsuleHalfHeight,
    const float CapsuleRadius,
    float& OutMinZ,
    float& OutMaxZ)
{
    const float SafeHalfHeight = FMath::Max(1.0f, CapsuleHalfHeight);
    const float SafeRadius = FMath::Max(1.0f, CapsuleRadius);

    // The stored reference is still BONE_HEAD-driven, but it must stay inside the
    // believable upper capsule band.  Imported rigs can report a head socket/bone
    // above the visual mesh during early pose setup, which would place the waterline
    // target far above the character and make surface correction fight animation.
    OutMinZ = FMath::Max(SafeRadius + 1.0f, SafeHalfHeight * CharacterWaterTuning::ReferenceMinHalfHeightRatio);
    const float MaxByHalfHeight = SafeHalfHeight * CharacterWaterTuning::ReferenceMaxHalfHeightRatio;
    const float MaxByTopMargin = SafeHalfHeight - SafeRadius * CharacterWaterTuning::ReferenceTopMarginRadiusRatio;
    OutMaxZ = FMath::Max(OutMinZ + 1.0f, FMath::Min(MaxByHalfHeight, MaxByTopMargin));
}

static FVector SanitizeWaterReferenceOffset(
    const FVector& CandidateOffset,
    const float CapsuleHalfHeight,
    const float CapsuleRadius,
    const bool bClampLowerBound)
{
    float MinReferenceZ = 0.0f;
    float MaxReferenceZ = 0.0f;
    GetWaterReferenceZBounds(CapsuleHalfHeight, CapsuleRadius, MinReferenceZ, MaxReferenceZ);

    FVector SanitizedOffset = CandidateOffset;
    SanitizedOffset.Z = bClampLowerBound
        ? FMath::Clamp(SanitizedOffset.Z, MinReferenceZ, MaxReferenceZ)
        : FMath::Min(SanitizedOffset.Z, MaxReferenceZ);

    // The surface test only needs a stable vertical head reference.  Clamp lateral
    // drift from animated/imported poses so water queries do not sample far in front
    // of the capsule while the player swims forward.
    const float MaxPlanarOffset = FMath::Max(1.0f, CapsuleRadius * 0.35f);
    SanitizedOffset.X = FMath::Clamp(SanitizedOffset.X, -MaxPlanarOffset, MaxPlanarOffset);
    SanitizedOffset.Y = FMath::Clamp(SanitizedOffset.Y, -MaxPlanarOffset, MaxPlanarOffset);
    return SanitizedOffset;
}

static bool TryGetSkeletalReferenceLocation(
    const USkeletalMeshComponent* SkeletalMesh,
    const FName BoneName,
    FVector& OutLocation)
{
    if (!IsValid(SkeletalMesh) || BoneName == NAME_None)
    {
        return false;
    }

    const bool bHasBone = SkeletalMesh->GetBoneIndex(BoneName) != INDEX_NONE;
    const bool bHasSocket = SkeletalMesh->DoesSocketExist(BoneName);
    if (!bHasBone && !bHasSocket)
    {
        return false;
    }

    // Waterline references must follow the actual skeleton, not an attachment socket.
    // Some imported head sockets are authored above the mesh for hats/cameras; using
    // those directly makes the stable water reference float far above the character.
    OutLocation = bHasBone ? SkeletalMesh->GetBoneLocation(BoneName) : SkeletalMesh->GetSocketLocation(BoneName);
    return !OutLocation.ContainsNaN();
}

static FVector MakeSwimmingForwardVector(
    const FRotator& ControlRotation,
    const float SurfacePlaneAlpha)
{
    const FVector PitchedForward = ControlRotation.Vector();
    const FVector FlatForward = MakeForwardVectorFromYaw(ControlRotation.Yaw);
    const float SafeAlpha = FMath::Clamp(SurfacePlaneAlpha, 0.0f, 1.0f);

    // Blend into flat-yaw motion as the stable head reference approaches the surface.
    // This removes the hard direction swap that made forward+up movement pop at the waterline.
    return FMath::Lerp(PitchedForward, FlatForward, SafeAlpha).GetSafeNormal();
}

static float ComputeSwimmingSurfacePlaneAlpha(
    const float ImmersionDepth,
    const float SurfaceLockDepth)
{
    const float SafeLockDepth = FMath::Max(1.0f, SurfaceLockDepth);
    const float LinearAlpha = 1.0f - FMath::Clamp(ImmersionDepth / SafeLockDepth, 0.0f, 1.0f);

    // SmoothStep keeps the surface transition continuous while still reaching a full
    // planar lock exactly at and above the water surface.
    return LinearAlpha * LinearAlpha * (3.0f - 2.0f * LinearAlpha);
}

static FVector ComputeSwimmingInputDirection(
    const FVector& MoveInput,
    const FRotator& ControlRotation,
    const float SurfacePlaneAlpha)
{
    const FVector FlatRight = MakeRightVectorFromYaw(ControlRotation.Yaw);
    const FVector Forward = MakeSwimmingForwardVector(ControlRotation, SurfacePlaneAlpha);
    const FVector DesiredDirection = FlatRight * MoveInput.X + Forward * MoveInput.Y + FVector::UpVector * MoveInput.Z;

    return DesiredDirection.GetSafeNormal();
}

static void ApplySwimmingVelocityDamping(
    UCharacterMovementComponent* Movement,
    const FVector& MoveInput,
    const FRotator& ControlRotation,
    const float DeltaTime,
    const float SubmergedAlpha,
    const float SurfacePlaneAlpha,
    const bool bBlockUpwardVelocity)
{
    if (!IsValid(Movement) || DeltaTime <= SMALL_NUMBER)
    {
        return;
    }

    FVector Velocity = Movement->Velocity;
    if (Velocity.IsNearlyZero())
    {
        return;
    }

    const float SafeSubmergedAlpha = FMath::Clamp(SubmergedAlpha, 0.0f, 1.0f);
    const float InputAmount = FMath::Clamp(MoveInput.Size(), 0.0f, 1.0f);

    // Keep light water drag, but do not overdamp active swimming.  The previous
    // surface fix made this run too aggressively after a ceiling latch, which
    // made normal underwater movement feel stuck.
    const float BaseDampingRate = FMath::Lerp(0.35f, 0.95f, SafeSubmergedAlpha);
    Velocity *= ComputeExponentialDampingFactor(BaseDampingRate, DeltaTime);

    const FVector InputDirection = ComputeSwimmingInputDirection(MoveInput, ControlRotation, SurfacePlaneAlpha);
    if (InputAmount > 0.05f && !InputDirection.IsNearlyZero())
    {
        const float AlongSpeed = FVector::DotProduct(Velocity, InputDirection);
        const FVector AlongInputVelocity = InputDirection * FMath::Max(0.0f, AlongSpeed);
        const FVector DriftVelocity = Velocity - AlongInputVelocity;

        // Sideways/backward carry-over should still fade faster than intentional swimming velocity,
        // but not so fast that diagonal underwater movement loses most of its speed.
        const float DriftDampingRate = FMath::Lerp(3.0f, 7.0f, SafeSubmergedAlpha);
        Velocity = AlongInputVelocity + DriftVelocity * ComputeExponentialDampingFactor(DriftDampingRate, DeltaTime);
    }
    else
    {
        // Releasing input should still slow down, while preserving enough water inertia
        // that the swimmer does not feel like they are dragging through glue.
        const float NoInputDampingRate = FMath::Lerp(0.9f, 1.8f, SafeSubmergedAlpha);
        Velocity *= ComputeExponentialDampingFactor(NoInputDampingRate, DeltaTime);
    }

    if (bBlockUpwardVelocity && Velocity.Z > 0.0f)
    {
        const float SurfaceUpDampingRate = FMath::Lerp(2.0f, 5.0f, SafeSubmergedAlpha);
        Velocity.Z *= ComputeExponentialDampingFactor(SurfaceUpDampingRate, DeltaTime);
    }

    if (Velocity.SizeSquared() < FMath::Square(8.0f))
    {
        Velocity = FVector::ZeroVector;
    }

    Movement->Velocity = Velocity;
}

static bool UpdateSwimmingSurfaceCeilingLock(
    const float ReferenceImmersionDepth,
    const float SurfaceCeilingDepth,
    const float SwimExitDepth,
    const float SurfaceLockDepth,
    const bool bWantsUp,
    const bool bWantsDown,
    bool& bInOutLocked)
{
    const float EnterPadding = FMath::Max(0.5f, SwimExitDepth * 0.25f);
    const float ReleasePadding = FMath::Max(2.0f, SwimExitDepth * 0.70f);
    const float FullSwimReleaseDepth = FMath::Max(SurfaceLockDepth, SurfaceCeilingDepth + ReleasePadding);

    if (bInOutLocked)
    {
        // Keep the near-surface lock until the visible-head line is reached.  The
        // previous version released the lock as soon as the player stopped holding
        // Up while the head reference was still a few centimeters under water, so
        // the automatic surface servo never finished lifting the head out.
        const bool bClearlyBackInsideWater = ReferenceImmersionDepth > FullSwimReleaseDepth;
        const bool bDivedBelowReleaseBand = bWantsDown && ReferenceImmersionDepth > SurfaceCeilingDepth + ReleasePadding;
        if (bClearlyBackInsideWater || bDivedBelowReleaseBand)
        {
            bInOutLocked = false;
        }
    }
    else
    {
        // Capture through the whole near-surface band, not only the lower 65%.
        // This lets the servo start before the face is already at the waterline.
        const float SurfaceCaptureDepth = FMath::Max(SurfaceCeilingDepth + EnterPadding, SurfaceLockDepth);
        if (!bWantsDown && ReferenceImmersionDepth <= SurfaceCaptureDepth)
        {
            bInOutLocked = true;
        }
    }

    return bInOutLocked;
}

static void ApplySwimmingSurfaceRiseAssist(
    UCharacterMovementComponent* Movement,
    const float DeltaTime,
    const float ImmersionDepth,
    const float SurfaceCeilingDepth,
    const bool bWantsUp,
    const bool bWantsDown)
{
    if (!IsValid(Movement) || DeltaTime <= SMALL_NUMBER || !bWantsUp || bWantsDown)
    {
        return;
    }

    const float DepthBelowVisibleLine = ImmersionDepth - SurfaceCeilingDepth;
    if (DepthBelowVisibleLine <= CharacterWaterTuning::SurfaceRiseAssistDeadZoneCm)
    {
        return;
    }

    FVector Velocity = Movement->Velocity;
    const float DesiredUpSpeed = FMath::Clamp(
        DepthBelowVisibleLine * CharacterWaterTuning::SurfaceRiseAssistVelocityPerCm,
        CharacterWaterTuning::SurfaceRiseAssistMinUpSpeed,
        CharacterWaterTuning::SurfaceRiseAssistMaxUpSpeed);
    const float CurrentUpSpeed = FMath::Max(0.0f, Velocity.Z);
    Velocity.Z = FMath::Max(
        Velocity.Z,
        FMath::FInterpTo(CurrentUpSpeed, DesiredUpSpeed, DeltaTime, CharacterWaterTuning::SurfaceRiseAssistInterpSpeed));
    Movement->Velocity = Velocity;
}

static void ApplySwimmingSurfaceConstraint(
    UCharacterMovementComponent* Movement,
    FVector& InOutCurrentSpeed,
    const float DeltaTime,
    const float ImmersionDepth,
    const float SurfaceCeilingDepth,
    const float SurfaceLockDepth,
    const bool bCeilingLocked)
{
    if (!IsValid(Movement) || DeltaTime <= SMALL_NUMBER)
    {
        return;
    }

    const bool bAboveCeiling = ImmersionDepth <= SurfaceCeilingDepth + KINDA_SMALL_NUMBER;
    const bool bInsideNormalSwimDepth = ImmersionDepth > SurfaceLockDepth;
    if ((!bCeilingLocked && !bAboveCeiling) || bInsideNormalSwimDepth)
    {
        // Deep water must stay completely free of the surface clamp.  A stale
        // ceiling latch should never keep damping vertical movement after the
        // character has returned to real underwater swimming.
        return;
    }

    // While the lock is only assisting the character up toward the visible-head
    // line, keep the player's upward swimming input alive.  Block upward input
    // only after the stable reference has actually reached or crossed the ceiling.
    const bool bAtOrAboveVisibleCeiling = bAboveCeiling;
    const bool bPlayerDiving = InOutCurrentSpeed.Z < -KINDA_SMALL_NUMBER;
    if (bAtOrAboveVisibleCeiling)
    {
        InOutCurrentSpeed.Z = FMath::Min(InOutCurrentSpeed.Z, 0.0f);
    }

    FVector Velocity = Movement->Velocity;
    if (bAtOrAboveVisibleCeiling && Velocity.Z > 0.0f)
    {
        // Kill player-driven upward carry-over only at the final ceiling. Below
        // that line, upward velocity is useful because it helps the head surface.
        Velocity.Z = 0.0f;
    }

    const float CeilingDeadZone = FMath::Max(0.5f, FMath::Abs(SurfaceCeilingDepth) * 0.15f);
    if (bAboveCeiling)
    {
        const float ExcessAboveCeiling = SurfaceCeilingDepth - ImmersionDepth;
        if (ExcessAboveCeiling > CeilingDeadZone)
        {
            const float CorrectedExcess = ExcessAboveCeiling - CeilingDeadZone;
            const float DesiredDownSpeed = FMath::Clamp(CorrectedExcess * 18.0f, 0.0f, 620.0f);
            const float CurrentDownSpeed = FMath::Max(0.0f, -Velocity.Z);
            const float SmoothedDownSpeed = FMath::FInterpTo(CurrentDownSpeed, DesiredDownSpeed, DeltaTime, 18.0f);
            Velocity.Z = -FMath::Clamp(FMath::Max(CurrentDownSpeed, SmoothedDownSpeed), 0.0f, 620.0f);
        }
    }
    else if (bCeilingLocked && !bPlayerDiving)
    {
        // Do not add passive upward velocity while the player is idle. Strong passive
        // lift makes the character keep bobbing upward even when no input is held.
        // The separate rise assist only runs while the player is actively swimming up.
        if (Velocity.Z > 0.0f)
        {
            Velocity.Z = FMath::FInterpTo(Velocity.Z, 0.0f, DeltaTime, 6.0f);
        }
    }

    Movement->Velocity = Velocity;
}

static FString GetCharacterNormalizedBoneName(const FName BoneName)
{
    FString BoneString = BoneName.ToString().ToLower();
    BoneString.ReplaceInline(TEXT("_"), TEXT(""));
    BoneString.ReplaceInline(TEXT("-"), TEXT(""));
    BoneString.ReplaceInline(TEXT("."), TEXT(""));
    BoneString.ReplaceInline(TEXT(" "), TEXT(""));
    BoneString.ReplaceInline(TEXT(":"), TEXT(""));
    return BoneString;
}

static bool CharacterBoneStringContainsAny(const FString& BoneString, const TCHAR* const* Tokens, const int32 TokenCount)
{
    for (int32 TokenIndex = 0; TokenIndex < TokenCount; ++TokenIndex)
    {
        if (BoneString.Contains(Tokens[TokenIndex]))
        {
            return true;
        }
    }
    return false;
}

static bool IsRagdollCosmeticOrHelperBoneName(const FString& BoneString)
{
    static const TCHAR* IgnoredTokens[] = {
        TEXT("hair"),
        TEXT("cloth"),
        TEXT("skirt"),
        TEXT("cape"),
        TEXT("ponytail"),
        TEXT("accessory"),
        TEXT("jiggle"),
        TEXT("breast"),
        TEXT("twist"),
        TEXT("ik"),
        TEXT("weapon")
    };

    return CharacterBoneStringContainsAny(BoneString, IgnoredTokens, UE_ARRAY_COUNT(IgnoredTokens));
}

static bool IsRagdollLimbBoneName(const FString& BoneString)
{
    static const TCHAR* LimbTokens[] = {
        TEXT("clavicle"),
        TEXT("shoulder"),
        TEXT("upperarm"),
        TEXT("lowerarm"),
        TEXT("forearm"),
        TEXT("arm"),
        TEXT("elbow"),
        TEXT("hand"),
        TEXT("wrist"),
        TEXT("palm"),
        TEXT("finger"),
        TEXT("thumb"),
        TEXT("index"),
        TEXT("middle"),
        TEXT("ring"),
        TEXT("pinky"),
        TEXT("upperleg"),
        TEXT("lowerleg"),
        TEXT("thigh"),
        TEXT("calf"),
        TEXT("shin"),
        TEXT("knee"),
        TEXT("ankle"),
        TEXT("leg"),
        TEXT("foot"),
        TEXT("toe"),
        TEXT("ball")
    };

    return CharacterBoneStringContainsAny(BoneString, LimbTokens, UE_ARRAY_COUNT(LimbTokens));
}

static bool IsRagdollReleaseIgnoredBone(const FName BoneName)
{
    const FString BoneString = GetCharacterNormalizedBoneName(BoneName);
    return IsRagdollCosmeticOrHelperBoneName(BoneString) || IsRagdollLimbBoneName(BoneString);
}

static bool IsRagdollWaterProbeBone(const FName BoneName)
{
    return !IsRagdollCosmeticOrHelperBoneName(GetCharacterNormalizedBoneName(BoneName));
}

static bool IsRagdollPrimaryReleaseBone(const FName BoneName)
{
    const FString BoneString = GetCharacterNormalizedBoneName(BoneName);
    return BoneString == TEXT("root")
        || BoneString.Contains(TEXT("hips"))
        || BoneString.Contains(TEXT("pelvis"));
}

static bool IsRagdollCoreReleaseBone(const FName BoneName)
{
    if (IsRagdollReleaseIgnoredBone(BoneName))
    {
        return false;
    }

    const FString BoneString = GetCharacterNormalizedBoneName(BoneName);
    return IsRagdollPrimaryReleaseBone(BoneName)
        || BoneString.Contains(TEXT("spine"))
        || BoneString.Contains(TEXT("chest"))
        || BoneString.Contains(TEXT("torso"))
        || BoneString.Contains(TEXT("abdomen"))
        || BoneString.Contains(TEXT("neck"))
        || BoneString.Contains(TEXT("head"));
}


static FVector ClampInitialRagdollVelocityForActivation(const FVector& Velocity)
{
    if (Velocity.ContainsNaN())
    {
        return FVector::ZeroVector;
    }

    // The ragdoll inherits only the previous character movement velocity.
    // Do not multiply it or reconstruct impact velocity here; extra energy makes
    // the body launch and causes visible stepping on the spring-arm target.
    FVector Result = Velocity;

    FVector PlanarVelocity(Result.X, Result.Y, 0.0f);
    PlanarVelocity = PlanarVelocity.GetClampedToMaxSize(CharacterRagdollTuning::MaxInitialRagdollPlanarSpeed);

    Result.X = PlanarVelocity.X;
    Result.Y = PlanarVelocity.Y;
    Result.Z = FMath::Clamp(
        Result.Z,
        -CharacterRagdollTuning::MaxInitialRagdollDownSpeed,
        CharacterRagdollTuning::MaxInitialRagdollUpSpeed);

    return Result.GetClampedToMaxSize(CharacterRagdollTuning::MaxInitialRagdollSpeed);
}

static FORCEINLINE bool IsUsefulInitialRagdollVelocity(const FVector& Candidate)
{
    return !Candidate.ContainsNaN()
        && Candidate.SizeSquared() > FMath::Square(CharacterRagdollTuning::InitialRagdollVelocityDeadZone);
}

static FORCEINLINE void ConsiderInitialRagdollVelocity(FVector& BestVelocity, const FVector& Candidate)
{
    if (!IsUsefulInitialRagdollVelocity(Candidate))
    {
        return;
    }

    const FVector SafeCandidate = ClampInitialRagdollVelocityForActivation(Candidate);
    if (SafeCandidate.SizeSquared() > BestVelocity.SizeSquared())
    {
        BestVelocity = SafeCandidate;
    }
}

static FORCEINLINE bool ShouldPreserveCachedPreRagdollVelocity(const FVector& CachedVelocity, const float CachedAge, const FVector& NewVelocity)
{
    if (CachedAge > CharacterRagdollTuning::PreRagdollVelocityGraceSeconds || !IsUsefulInitialRagdollVelocity(CachedVelocity))
    {
        return false;
    }

    // A collision or movement-mode transition can reduce CharacterMovement velocity
    // before SetRagdollActive() runs in the same frame.  Keep the last full-speed
    // gameplay velocity briefly instead of replacing it with that already-damped value.
    const float PreserveRatio = CharacterRagdollTuning::PreRagdollVelocityDropPreserveRatio;
    return NewVelocity.SizeSquared() < CachedVelocity.SizeSquared() * PreserveRatio * PreserveRatio;
}

static bool ShouldApplyInitialRagdollVelocityToBone(const FName BoneName)
{
    const FString BoneString = GetCharacterNormalizedBoneName(BoneName);
    if (IsRagdollCosmeticOrHelperBoneName(BoneString))
    {
        return false;
    }

    // Give the initial gameplay velocity only to the bodies that make up the
    // character's physical mass. Fingers, toes, and accessory/helper chains are
    // left to constraints so they do not amplify the activation energy.
    return IsRagdollPrimaryReleaseBone(BoneName)
        || BoneString.Contains(TEXT("spine"))
        || BoneString.Contains(TEXT("chest"))
        || BoneString.Contains(TEXT("torso"))
        || BoneString.Contains(TEXT("abdomen"))
        || BoneString.Contains(TEXT("neck"))
        || BoneString.Contains(TEXT("head"))
        || BoneString.Contains(TEXT("clavicle"))
        || BoneString.Contains(TEXT("shoulder"))
        || BoneString.Contains(TEXT("upperarm"))
        || BoneString.Contains(TEXT("upper_arm"))
        || BoneString.Contains(TEXT("lowerarm"))
        || BoneString.Contains(TEXT("lower_arm"))
        || BoneString.Contains(TEXT("forearm"))
        || BoneString.Contains(TEXT("thigh"))
        || BoneString.Contains(TEXT("upperleg"))
        || BoneString.Contains(TEXT("upper_leg"))
        || BoneString.Contains(TEXT("lowerleg"))
        || BoneString.Contains(TEXT("lower_leg"))
        || BoneString.Contains(TEXT("calf"))
        || BoneString.Contains(TEXT("shin"));
}

static FORCEINLINE float GetSafeRagdollBodyMass(const FBodyInstance* BodyInstance)
{
    if (!BodyInstance)
    {
        return 1.0f;
    }

    const float BodyMass = BodyInstance->GetBodyMass();
    return FMath::IsFinite(BodyMass) && BodyMass > UE_SMALL_NUMBER ? BodyMass : 1.0f;
}

static FORCEINLINE FVector GetSafeRagdollBodyVelocity(const FBodyInstance* BodyInstance)
{
    if (!BodyInstance)
    {
        return FVector::ZeroVector;
    }

    const FVector Velocity = BodyInstance->GetUnrealWorldVelocity();
    return Velocity.ContainsNaN() ? FVector::ZeroVector : Velocity;
}

static FORCEINLINE bool IsRagdollPreferredProbeBone(const FName BoneName)
{
    return BoneName == FName(BONE_HIPS)
        || BoneName == FName(BONE_NECK)
        || BoneName == FName(BONE_HEAD)
        || BoneName == FName(BONE_LEFT_UPPER_LEG)
        || BoneName == FName(BONE_RIGHT_UPPER_LEG)
        || BoneName == FName(BONE_LEFT_FOOT)
        || BoneName == FName(BONE_RIGHT_FOOT);
}

static FORCEINLINE bool IsRagdollPreferredCoreProbeBone(const FName BoneName)
{
    return BoneName == FName(BONE_HIPS)
        || BoneName == FName(BONE_NECK)
        || BoneName == FName(BONE_HEAD);
}

static bool IsRagdollBodySimulatingForRelease(USkeletalMeshComponent* SkeletalMesh, const FName BoneName)
{
    if (!IsValid(SkeletalMesh) || BoneName == NAME_None)
    {
        return false;
    }

    const FBodyInstance* BodyInstance = SkeletalMesh->GetBodyInstance(BoneName);
    return BodyInstance != nullptr && BodyInstance->IsInstanceSimulatingPhysics();
}

static bool AddRagdollProbeLocation(
    const USkeletalMeshComponent* SkeletalMesh,
    const FName BoneName,
    FRagdollProbeLocationArray& OutLocations,
    const bool bRequireSimulatingBody)
{
    if (!IsValid(SkeletalMesh) || BoneName == NAME_None || SkeletalMesh->GetBoneIndex(BoneName) == INDEX_NONE)
    {
        return false;
    }

    if (bRequireSimulatingBody && !IsRagdollBodySimulatingForRelease(const_cast<USkeletalMeshComponent*>(SkeletalMesh), BoneName))
    {
        return false;
    }

    const FVector Location = SkeletalMesh->GetBoneLocation(BoneName);
    if (Location.ContainsNaN())
    {
        return false;
    }

    OutLocations.Add(Location);
    return true;
}

static void GatherRagdollProbeLocations(
    const USkeletalMeshComponent* SkeletalMesh,
    FRagdollProbeLocationArray& OutLocations,
    const bool bPreferSimulatingBodies,
    const bool bCoreOnly)
{
    OutLocations.Reset();
    if (!IsValid(SkeletalMesh))
    {
        return;
    }

    // Fast path: use a small, deterministic probe set. Scanning every bone every frame was both
    // expensive and noisy because hands/feet/accessories could touch water for one frame.
    static const FName PreferredBones[] = {
        FName(BONE_HIPS),
        FName(BONE_NECK),
        FName(BONE_HEAD),
        FName(BONE_LEFT_UPPER_LEG),
        FName(BONE_RIGHT_UPPER_LEG),
        FName(BONE_LEFT_FOOT),
        FName(BONE_RIGHT_FOOT)
    };

    OutLocations.Reserve(bCoreOnly ? 4 : UE_ARRAY_COUNT(PreferredBones));

    auto GatherPreferredBones = [&](const bool bRequireSimulatingBody)
    {
        for (const FName BoneName : PreferredBones)
        {
            if (!bCoreOnly || IsRagdollPreferredCoreProbeBone(BoneName))
            {
                AddRagdollProbeLocation(SkeletalMesh, BoneName, OutLocations, bRequireSimulatingBody);
            }
        }
    };

    if (bPreferSimulatingBodies)
    {
        GatherPreferredBones(true);
    }

    // If physics has already been disabled, the current mesh pose is still a valid release snapshot.
    if (OutLocations.Num() == 0)
    {
        GatherPreferredBones(false);
    }

    const int32 RequiredFastPathProbeCount = bCoreOnly ? 2 : 4;
    if (OutLocations.Num() >= RequiredFastPathProbeCount)
    {
        return;
    }

    // Slow fallback: only scan the full skeleton when the expected runtime bones are missing.
    // This keeps unusual imported skeletons working without paying the cost on the normal path.
    auto GatherFallbackBones = [&](const bool bRequireSimulatingBody)
    {
        const int32 BoneCount = SkeletalMesh->GetNumBones();
        for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
        {
            const FName BoneName = SkeletalMesh->GetBoneName(BoneIndex);
            if (IsRagdollPreferredProbeBone(BoneName))
            {
                continue;
            }

            if (bCoreOnly)
            {
                if (!IsRagdollCoreReleaseBone(BoneName))
                {
                    continue;
                }
            }
            else if (!IsRagdollWaterProbeBone(BoneName))
            {
                continue;
            }

            AddRagdollProbeLocation(SkeletalMesh, BoneName, OutLocations, bRequireSimulatingBody);
        }
    };

    if (bPreferSimulatingBodies)
    {
        GatherFallbackBones(true);
    }

    if (OutLocations.Num() == 0)
    {
        GatherFallbackBones(false);
    }

    if (OutLocations.Num() == 0)
    {
        const FVector ComponentLocation = SkeletalMesh->GetComponentLocation();
        if (!ComponentLocation.ContainsNaN())
        {
            OutLocations.Add(ComponentLocation);
        }
    }
}

template <typename AllocatorType>
static bool ProbeRagdollWaterLevelFromLocations(const UObject* WorldContext, const TArray<FVector, AllocatorType>& ProbeLocations, float& InOutWaterLevel)
{
    bool bFoundWater = false;
    float DetectedLevel = InOutWaterLevel;

    for (const FVector& WorldLocation : ProbeLocations)
    {
        float ProbeLevel = DetectedLevel;
        if (AWaterActor::FindWaterLevelAtLocationStrict(WorldContext, WorldLocation, ProbeLevel))
        {
            DetectedLevel = bFoundWater ? FMath::Max(DetectedLevel, ProbeLevel) : ProbeLevel;
            bFoundWater = true;
        }
    }

    if (bFoundWater)
    {
        InOutWaterLevel = DetectedLevel;
    }
    return bFoundWater;
}

template <typename AllocatorType>
static void UpdateRagdollProbeReferenceState(const TArray<FVector, AllocatorType>& ProbeLocations, FCharacterRagdollEnvironmentState& State)
{
    State.RagdollProbeLocationCount = ProbeLocations.Num();
    if (ProbeLocations.Num() == 0)
    {
        State.RagdollReferenceLocation = FVector::ZeroVector;
        State.RagdollLowestLocation = FVector::ZeroVector;
        State.RagdollHighestLocation = FVector::ZeroVector;
        return;
    }

    FVector Sum = FVector::ZeroVector;
    FVector Lowest = ProbeLocations[0];
    FVector Highest = ProbeLocations[0];
    for (const FVector& Location : ProbeLocations)
    {
        Sum += Location;
        if (Location.Z < Lowest.Z)
        {
            Lowest = Location;
        }
        if (Location.Z > Highest.Z)
        {
            Highest = Location;
        }
    }

    State.RagdollReferenceLocation = Sum / static_cast<float>(ProbeLocations.Num());
    State.RagdollLowestLocation = Lowest;
    State.RagdollHighestLocation = Highest;
}

struct FRagdollSubmersionMetrics
{
    float MaxDepth = 0.0f;
    float AverageDepth = 0.0f;
    int32 SubmergedCount = 0;
};

template <typename AllocatorType>
static FRagdollSubmersionMetrics ComputeRagdollSubmersionMetrics(const TArray<FVector, AllocatorType>& ProbeLocations, const float WaterLevel)
{
    FRagdollSubmersionMetrics Metrics;
    float SumDepth = 0.0f;

    for (const FVector& Location : ProbeLocations)
    {
        const float Depth = WaterLevel - Location.Z;
        if (Depth <= 0.0f)
        {
            continue;
        }

        Metrics.MaxDepth = FMath::Max(Metrics.MaxDepth, Depth);
        SumDepth += Depth;
        ++Metrics.SubmergedCount;
    }

    if (Metrics.SubmergedCount > 0)
    {
        Metrics.AverageDepth = SumDepth / static_cast<float>(Metrics.SubmergedCount);
    }

    return Metrics;
}

static FORCEINLINE float GetRagdollGroundTraceDistance(const float TraceDistance, const float CapsuleRadius)
{
    return FMath::Max(TraceDistance, FMath::Max(45.0f, CapsuleRadius * 0.75f + 24.0f));
}

static FORCEINLINE float GetRaisedGroundTraceStartLift(const float CapsuleRadius)
{
    // Start ground probes above the sampled point instead of exactly at it.
    // This prevents thin platforms from being skipped when the capsule or ragdoll
    // body moves slightly through the floor before the next water/ground update.
    return FMath::Max(24.0f, CapsuleRadius * 0.45f + 8.0f);
}

static FORCEINLINE FVector GetRagdollGroundTraceStart(const FVector& WorldLocation, const float CapsuleRadius)
{
    return WorldLocation + FVector::UpVector * GetRaisedGroundTraceStartLift(CapsuleRadius);
}

static FORCEINLINE FVector GetRagdollGroundTraceEnd(const FVector& WorldLocation, const float TraceDistance)
{
    return WorldLocation - FVector::UpVector * FMath::Max(1.0f, TraceDistance);
}

static FORCEINLINE bool IsRagdollWalkableGroundHit(const FHitResult& Hit, const UCharacterMovementComponent* Movement)
{
    const float WalkableZ = IsValid(Movement) ? Movement->GetWalkableFloorZ() : 0.55f;
    return Hit.bBlockingHit && Hit.ImpactNormal.Z >= FMath::Max(0.35f, WalkableZ - 0.05f);
}

static bool IsIgnoredWaterTraceHit(const FHitResult& Hit)
{
    // Water is a state trigger, not walkable support. Skip it while searching for
    // dry collision that can block water entry or finish ragdoll recovery on land.
    return Cast<AWaterActor>(Hit.GetActor()) != nullptr;
}

static void AddCharacterSelfIgnore(
    FCollisionQueryParams& QueryParams,
    const ACharacterController* Character,
    const USkeletalMeshComponent* MeshComponent)
{
    if (!IsValid(Character))
    {
        return;
    }

    QueryParams.AddIgnoredActor(Character);
    if (const UCapsuleComponent* Capsule = Character->GetCapsuleComponent())
    {
        QueryParams.AddIgnoredComponent(Capsule);
    }
    if (IsValid(MeshComponent))
    {
        QueryParams.AddIgnoredComponent(MeshComponent);
    }
}

static bool TraceWalkableGroundByChannel(
    UWorld* World,
    const FVector& Start,
    const FVector& End,
    const UCharacterMovementComponent* Movement,
    const FCollisionQueryParams& QueryParams,
    FHitResult* OutHitResult = nullptr)
{
    if (!World)
    {
        return false;
    }

    TArray<FHitResult> Hits;
    if (!World->LineTraceMultiByChannel(Hits, Start, End, ECC_Visibility, QueryParams))
    {
        return false;
    }

    for (const FHitResult& Hit : Hits)
    {
        if (IsIgnoredWaterTraceHit(Hit) || !IsRagdollWalkableGroundHit(Hit, Movement))
        {
            continue;
        }

        if (OutHitResult)
        {
            *OutHitResult = Hit;
        }
        return true;
    }

    return false;
}

UCharacterComponent::UCharacterComponent()
{
    PrimaryComponentTick.bCanEverTick = false; // This component is updated explicitly by the controller.
}



void UCharacterComponent::BeginPlay()
{
    Super::BeginPlay();

    OwnerCharacter = Cast<ACharacterController>(GetOwner());
    if (IsValid(OwnerCharacter))
    {
        Movement = OwnerCharacter->GetCharacterMovement();
        UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent();
        if (Capsule)
        {
            HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
            Radius = Capsule->GetScaledCapsuleRadius();
        }
        MeshComp = OwnerCharacter->GetMesh();
        SpringArm = OwnerCharacter->GetSpringArm();
        InitializeCrouchSettings();

        // Do not sample BONE_HEAD, write WaterReferenceOffsetFromCapsule, or
        // commit a capsule fallback in BeginPlay. Dynamically loaded glTF meshes are assigned
        // asynchronously, so the waterline reference must be resolved only from the
        // mesh-load completion path.
    }
}

void UCharacterComponent::InitializeCrouchSettings()
{
    if (!IsValid(OwnerCharacter))
    {
        return;
    }

    UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent();
    MeshComp = OwnerCharacter->GetMesh();
    Movement = OwnerCharacter->GetCharacterMovement();

    if (Capsule)
    {
        StandingCapsuleHalfHeight = Capsule->GetUnscaledCapsuleHalfHeight();
        StandingCapsuleRadius = Capsule->GetUnscaledCapsuleRadius();
        HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
        Radius = Capsule->GetScaledCapsuleRadius();
    }

    if (IsValid(MeshComp))
    {
        StandingMeshRelativeLocation = MeshComp->GetRelativeLocation();
        StandingMeshRelativeRotation = MeshComp->GetRelativeRotation();
    }

    if (IsValid(Movement))
    {
        Movement->GetNavAgentPropertiesRef().bCanCrouch = true;
        const float SafeStandingHalfHeight = FMath::Max(StandingCapsuleRadius + 2.0f, StandingCapsuleHalfHeight);
        const float DefaultCrouchedHalfHeight = FMath::Max(StandingCapsuleRadius + 2.0f, SafeStandingHalfHeight * 0.58f);
        const float CurrentCrouchedHalfHeight = Movement->GetCrouchedHalfHeight();
        if (CurrentCrouchedHalfHeight <= StandingCapsuleRadius
            || CurrentCrouchedHalfHeight >= SafeStandingHalfHeight - 1.0f)
        {
            Movement->SetCrouchedHalfHeight(DefaultCrouchedHalfHeight);
        }
    }
}

void UCharacterComponent::ApplyCrouchState(bool bShouldCrouch)
{
    if (!IsValid(OwnerCharacter))
    {
        return;
    }

    UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent();
    if (!Capsule || !IsValid(Movement))
    {
        return;
    }

    if (StandingCapsuleHalfHeight <= KINDA_SMALL_NUMBER)
    {
        InitializeCrouchSettings();
    }

    MeshComp = OwnerCharacter->GetMesh();

    if (bShouldCrouch)
    {
        if (!Movement->IsCrouching())
        {
            OwnerCharacter->Crouch(false);
        }

        if (IsValid(MeshComp))
        {
            const float CurrentHalfHeight = Capsule->GetUnscaledCapsuleHalfHeight();
            const float HalfHeightDelta = FMath::Max(0.0f, StandingCapsuleHalfHeight - CurrentHalfHeight);
            const FVector DesiredMeshRelativeLocation = StandingMeshRelativeLocation + FVector::UpVector * HalfHeightDelta;
            MeshComp->SetRelativeLocation(DesiredMeshRelativeLocation, false, nullptr, ETeleportType::TeleportPhysics);
            MeshComp->SetRelativeRotation(StandingMeshRelativeRotation);
            bCrouchVisualOffsetApplied = true;
        }
        return;
    }

    if (Movement->IsCrouching())
    {
        OwnerCharacter->UnCrouch(false);
    }

    if (bCrouchVisualOffsetApplied && IsValid(MeshComp))
    {
        MeshComp->SetRelativeLocation(StandingMeshRelativeLocation, false, nullptr, ETeleportType::TeleportPhysics);
        MeshComp->SetRelativeRotation(StandingMeshRelativeRotation);
    }
    bCrouchVisualOffsetApplied = false;
}

void UCharacterComponent::InvalidateWaterReferenceForPendingMeshLoad()
{
    WaterReferenceOffsetFromCapsule = FVector::ZeroVector;
    WaterOffset = 0.0f;
    bHasWaterReferenceOffsetFromCapsule = false;
    bWaterReferenceMeshLoadComplete = false;
    bSwimmingSurfaceCeilingLocked = false;
}

void UCharacterComponent::RequestWaterReferenceRefreshAfterMeshLoad()
{
    if (!IsValid(OwnerCharacter))
    {
        return;
    }

    // Clear any default-mesh or previous-character reference first. The final
    // cached waterline is allowed to be written only by the deferred post-load
    // sample below.
    MeshComp = OwnerCharacter->GetMesh();
    InvalidateWaterReferenceForPendingMeshLoad();
    bWaterReferenceMeshLoadComplete = true;

    UWorld* World = OwnerCharacter->GetWorld();
    if (!World)
    {
        RefreshWaterReferenceOffsetFromHead();
        return;
    }

    // Defer by one game tick so SetSkinnedAssetAndUpdate, physics-asset setup,
    // and the first skeletal transform refresh have all landed before BONE_HEAD
    // is converted into a stable capsule-local water reference.
    const FTimerDelegate RefreshDelegate = FTimerDelegate::CreateUObject(
        this,
        &UCharacterComponent::RefreshWaterReferenceOffsetFromHead);
    World->GetTimerManager().SetTimerForNextTick(RefreshDelegate);
}

void UCharacterComponent::RefreshWaterReferenceOffsetFromHead()
{
    if (!IsValid(OwnerCharacter) || !bWaterReferenceMeshLoadComplete)
    {
        return;
    }

    UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent();
    MeshComp = OwnerCharacter->GetMesh();
    if (!Capsule)
    {
        return;
    }

    HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
    Radius = Capsule->GetScaledCapsuleRadius();
    WaterReferenceOffsetFromCapsule = FVector::ZeroVector;
    WaterOffset = 0.0f;
    bHasWaterReferenceOffsetFromCapsule = false;

    // This function is intentionally reached from the mesh-load completion path,
    // not from BeginPlay. If the final mesh is still unavailable, leave the
    // reference pending instead of committing a capsule fallback too early.
    if (!IsValid(MeshComp) || !IsValid(MeshComp->GetSkinnedAsset()))
    {
        return;
    }

    if (MeshComp->IsRegistered())
    {
        // Make sure the loaded mesh has current component/bone transforms before
        // converting BONE_HEAD into a stable capsule-local offset.
        MeshComp->UpdateComponentToWorld();
        MeshComp->RefreshBoneTransforms();
    }

    // BONE_HEAD is the only skeletal surface reference. Do not use BONE_NECK as
    // a fallback here, because it makes the surface target sit below the face and
    // causes every waterline clamp to look too low.
    const float MinimumSkeletalReferenceZ = FMath::Max(1.0f, Radius * 0.25f);

    FVector HeadReferenceLocation = FVector::ZeroVector;
    if (TryGetHeadWaterReferenceLocation(HeadReferenceLocation))
    {
        const FVector CandidateOffset = Capsule->GetComponentTransform().InverseTransformPositionNoScale(HeadReferenceLocation);
        if (!CandidateOffset.ContainsNaN() && CandidateOffset.Z >= MinimumSkeletalReferenceZ)
        {
            // Keep the BONE_HEAD source, but clamp impossible capsule-local height.
            // This prevents the cached surface reference from sitting well above
            // the visible character while still preserving valid low head bones.
            WaterReferenceOffsetFromCapsule = SanitizeWaterReferenceOffset(CandidateOffset, HalfHeight, Radius, false);
            WaterOffset = FMath::Max(1.0f, WaterReferenceOffsetFromCapsule.Z);
            bHasWaterReferenceOffsetFromCapsule = true;
        }
    }

    if (!bHasWaterReferenceOffsetFromCapsule)
    {
        // Loaded non-humanoid or incomplete rigs still get a deterministic
        // upper-capsule reference. This fallback is allowed only after mesh-load
        // completion so BeginPlay can never lock in a default/fallback height.
        FVector FallbackOffset(0.0f, 0.0f, FMath::Max(1.0f, HalfHeight * CharacterWaterTuning::FallbackHeadOffsetHalfHeightRatio));
        WaterReferenceOffsetFromCapsule = SanitizeWaterReferenceOffset(FallbackOffset, HalfHeight, Radius, true);
        WaterOffset = FMath::Max(1.0f, WaterReferenceOffsetFromCapsule.Z);
        bHasWaterReferenceOffsetFromCapsule = true;
    }
}

void UCharacterComponent::UpdateComponent(float DeltaTime, const FVector &MoveInput, const int32 CharacterState, const float WaterLevel)
{
    if (!IsValid(OwnerCharacter) || !IsValid(Movement) || !IsValid(MeshComp))
        return;

    const FVector CurrentVelocity = OwnerCharacter->GetVelocity();
    const FRotator ControlRot = OwnerCharacter->GetControlRotation();
    const bool bInWaterState = UCharacterFunctionLibrary::IsStateActive(CharacterState, STATE_WATER);

    if (RagdollRecoverySwimLockTime > 0.0f)
    {
        RagdollRecoverySwimLockTime = FMath::Max(0.0f, RagdollRecoverySwimLockTime - DeltaTime);
    }
    else if (!IsRagdollLikeState())
    {
        bRagdollRecoveryWantsSwimming = false;
    }

    const bool bRagdollLikeState = IsRagdollLikeState();
    if (!bRagdollLikeState)
    {
        UpdateRagdollVelocityHistory(DeltaTime, CurrentVelocity);
        ImpactVelocity = CalculateImpactVelocity(CurrentVelocity);
    }
    else
    {
        // Active ragdoll uses the actor as a location-only camera/control anchor.
        // Do not feed that anchor displacement back into later ragdoll velocity inheritance.
        PrevVelocity = CurrentVelocity;
        ImpactVelocity = FVector::ZeroVector;
    }

    float EffectiveWaterLevel = WaterLevel;
    const bool bRagdollBodyDetectedInWater = bRagdollLikeState && RefreshRagdollWaterDetection(&EffectiveWaterLevel);
    const bool bOwnerInsideStrictWaterColumn = AWaterActor::FindWaterLevelAtLocationStrict(this, OwnerCharacter->GetActorLocation(), EffectiveWaterLevel)
        || AWaterActor::FindWaterLevelAtLocationStrict(this, OwnerCharacter->GetBottomLocation(), EffectiveWaterLevel);
    const bool bDirectWaterStateAccepted = bRagdollLikeState
        || (bInWaterState
            && bOwnerInsideStrictWaterColumn
            && ShouldUseDirectWaterState(EffectiveWaterLevel, Movement->MovementMode == MOVE_Swimming));
    const bool bEffectiveInWaterState = bRagdollLikeState ? bRagdollBodyDetectedInWater : (bInWaterState && bDirectWaterStateAccepted);
    bool bSwimRecoveryLocked = RagdollRecoverySwimLockTime > 0.0f && (bRagdollInWater || bRagdollRecoveryWantsSwimming);

    if (bSwimRecoveryLocked && !bEffectiveInWaterState)
    {
        RagdollRecoverySwimLockTime = 0.0f;
        bSwimRecoveryLocked = false;
    }

    // The short post-recovery swim lock is only an animation bridge. It must not
    // keep normal player control in the heavy ragdoll/swim recovery branch, because
    // that made movement feel sluggish and could immediately cancel an explicit Fly input.
    const bool bActiveRagdollOrRecoverySwimLock = bSwimRecoveryLocked && bRagdollLikeState;

    if (bRagdollLikeState)
    {
        if (bEffectiveInWaterState || bActiveRagdollOrRecoverySwimLock)
        {
            bRagdollInWater = true;
            if (!bIsRagdoll && (bGettingUp || RagdollWeight > 0.0f))
            {
                bRagdollRecoveryWantsSwimming = true;
            }

            StopMovementAndSetMode(Movement, MOVE_Swimming);
        }
        else
        {
            // Current frame probes say the ragdoll/recovery is dry. Clear the sticky water flags
            // before the animation instance reads them so a character that came out of water does
            // not stay in the swimming state during land get-up.
            ClearRagdollWaterIntent();
            if (Movement->MovementMode == MOVE_Swimming)
            {
                StopMovementAndDisable(Movement);
            }
        }

        // Ragdoll/get-up owns actor, capsule and mesh transforms until recovery finishes.
        // Do not fall through to normal movement/crouch code, which can overwrite the mesh
        // relative transform and make the AnimBP appear to snap out of the saved pose.
        UpdateRagdoll(DeltaTime, OwnerCharacter, MeshComp);
        return;
    }

    bRagdollInWater = bEffectiveInWaterState;
    if (!bRagdollInWater)
    {
        ClearRagdollWaterIntent();
    }

    // 1. Cache ground and base movement states once for this frame.
    FHitResult HitResult;
    const bool bIsOnGround = Movement->IsMovingOnGround();
    const bool bIsContactGround = TraceCharacterWalkableGroundFromAbove(HitResult);
    const bool bIsGrounded = bIsOnGround || bIsContactGround;
    const bool bIsFalling = Movement->IsFalling();
    const bool bIsCrouch = UCharacterFunctionLibrary::IsStateActive(CharacterState, STATE_CROUCH);
    const FVector GroundNormal = bIsContactGround ? HitResult.ImpactNormal : FVector::UpVector;

    Movement->bOrientRotationToMovement = (MoveInput.X != 0.0f || MoveInput.Y != 0.0f) && !bIsFalling;

    const float BaseTime = DeltaTime * CharacterMovementTuning::BaseAccelerationTimeScale;

    // 2. Flying mode.
    if (Movement->IsFlying())
    {
        bSwimmingSurfaceCeilingLocked = false;

        RagdollResistance = CharacterMovementTuning::FlyingRagdollResistance;
        CurrentSpeed.X = CalculateAcceleration(CurrentSpeed.X, MoveInput.X, BaseTime);
        CurrentSpeed.Y = CalculateAcceleration(CurrentSpeed.Y, MoveInput.Y, BaseTime);
        CurrentSpeed.Z = CalculateAcceleration(CurrentSpeed.Z, MoveInput.Z, BaseTime);

        Movement->MaxAcceleration = CharacterMovementTuning::FlyingMaxAcceleration;
        Movement->MaxFlySpeed = UCharacterFunctionLibrary::IsStateActive(CharacterState, STATE_SPRINT)
            ? CharacterMovementTuning::FlyingSprintMaxSpeed
            : CharacterMovementTuning::FlyingMaxSpeed;

        ApplyMoveRightForward(OwnerCharacter, ControlRot, CurrentSpeed);
        OwnerCharacter->AddMovementInput(FVector::UpVector, CurrentSpeed.Z);
    }
    // 3. Swimming and ground movement modes.
    else
    {
        RagdollResistance = CharacterMovementTuning::GroundWaterRagdollResistance;
        const float DirectWaterReferenceDepth = GetDirectWaterImmersionDepth(EffectiveWaterLevel);
        const float SurfaceLimitDepth = DirectWaterReferenceDepth;
        const float CapsuleImmersionDepth = GetDirectWaterCapsuleImmersionDepth(EffectiveWaterLevel);
        float SwimSurfaceCaptureDepth = 0.0f;
        float SwimExitDepth = 0.0f;
        float SwimSurfaceLockDepth = 0.0f;
        GetCapsuleSwimmingDepths(SwimSurfaceCaptureDepth, SwimExitDepth, SwimSurfaceLockDepth);
        (void)SwimSurfaceCaptureDepth;

        const bool bUnderSurface = DirectWaterReferenceDepth > 0.0f;
        const bool bIsJumping = UCharacterFunctionLibrary::IsStateActive(CharacterState, STATE_JUMPING);
        const bool bNotLandBehaviour = !(bIsGrounded || bIsJumping || bIsFalling);
        const bool bAlreadySwimming = Movement->MovementMode == MOVE_Swimming;
        const bool bCapsuleAtLeastHalfSubmerged = IsCapsuleAtLeastHalfSubmerged(EffectiveWaterLevel);
        const float HeadSurfaceClearance = GetStableHeadEmergenceHeight();
        const float SurfaceCeilingDepth = -HeadSurfaceClearance;
        const float SwimEntryReferenceDepth = GetSwimEntryReferenceDepth();
        const float ExistingSwimCorrectionDepth = -(SwimSurfaceLockDepth + HeadSurfaceClearance);
        const float RequiredSwimDepth = bAlreadySwimming ? ExistingSwimCorrectionDepth : SwimEntryReferenceDepth;
        const bool bReferenceReachedSwimLine = DirectWaterReferenceDepth >= SwimEntryReferenceDepth;
        const bool bDeepEnoughToSwim = DirectWaterReferenceDepth >= RequiredSwimDepth
            || (bAlreadySwimming && bCapsuleAtLeastHalfSubmerged);
        const bool bGroundSupportShouldStayDry = bIsGrounded
            && IsGroundSupportBlockingDirectWater(EffectiveWaterLevel, DirectWaterReferenceDepth, bAlreadySwimming);
        const float SlopeExitSurfaceBypassMargin = FMath::Max(
            CharacterWaterTuning::SlopeExitSurfaceBypassMinMarginCm,
            Radius * CharacterWaterTuning::SlopeExitSurfaceBypassMarginRadiusRatio);
        const bool bGroundedOnShoreExitSlope = bAlreadySwimming
            && bIsContactGround
            && GroundNormal.Z >= CharacterWaterTuning::SlopeExitNormalZMin
            && GroundNormal.Z <= CharacterWaterTuning::SlopeExitNormalZMax
            && HitResult.ImpactPoint.Z >= EffectiveWaterLevel - SlopeExitSurfaceBypassMargin;
        const bool bCanEnterFromGroundAtVisibleHeadLine = bReferenceReachedSwimLine && bCapsuleAtLeastHalfSubmerged;
        const bool bCheckSwimming = bEffectiveInWaterState
            && !bGroundSupportShouldStayDry
            && bDeepEnoughToSwim
            && (bAlreadySwimming || bUnderSurface || bNotLandBehaviour || bCanEnterFromGroundAtVisibleHeadLine);
        const bool bIsSprint = UCharacterFunctionLibrary::IsStateActive(CharacterState, STATE_SPRINT);

        if (bCheckSwimming)
        {
            if (Movement->MovementMode != MOVE_Swimming)
            {
                Movement->SetMovementMode(MOVE_Swimming);
            }

            const float VelocitySize = Movement->Velocity.Size();
            // Use moderate resistance for normal swimming.  Surface clamping handles
            // the waterline; underwater control should stay responsive and should not
            // inherit the heavy anti-pop damping used near the ceiling.
            const float LinearResistance = CharacterMovementTuning::SwimmingLinearResistance;
            const float QuadraticResistance = CharacterMovementTuning::SwimmingQuadraticResistanceScale * VelocitySize * VelocitySize;
            const float Braking = FMath::Clamp(
                LinearResistance * VelocitySize + QuadraticResistance + FMath::Sqrt(FMath::Max(0.0f, QuadraticResistance)),
                CharacterMovementTuning::SwimmingMinBrakingDeceleration,
                CharacterMovementTuning::SwimmingMaxBrakingDeceleration);

            Movement->BrakingDecelerationSwimming = Braking;
            Movement->MaxAcceleration = CharacterMovementTuning::SwimmingMaxAcceleration;
            Movement->MaxSwimSpeed = bIsSprint
                ? CharacterMovementTuning::SwimmingSprintMaxSpeed
                : CharacterMovementTuning::SwimmingMaxSpeed;

            FVector SwimMoveInput = MoveInput;
            const bool bWantsUpAtSurface = SwimMoveInput.Z > CharacterMovementTuning::SwimmingInputDeadZone;
            const bool bWantsDownAtSurface = SwimMoveInput.Z < -CharacterMovementTuning::SwimmingInputDeadZone;
            const bool bLatchedAtSurfaceCeiling = UpdateSwimmingSurfaceCeilingLock(
                SurfaceLimitDepth,
                SurfaceCeilingDepth,
                SwimExitDepth,
                SwimSurfaceLockDepth,
                bWantsUpAtSurface,
                bWantsDownAtSurface,
                bSwimmingSurfaceCeilingLocked);
            if (bGroundedOnShoreExitSlope)
            {
                // While the swimmer is actually climbing a walkable shoreline slope,
                // do not treat the water surface as a ceiling. The character should
                // be allowed to keep moving up and hand off to walking naturally.
                bSwimmingSurfaceCeilingLocked = false;
            }
            const bool bAtVisibleSurfaceCeiling = !bGroundedOnShoreExitSlope
                && SurfaceLimitDepth <= SurfaceCeilingDepth + KINDA_SMALL_NUMBER;
            const bool bSurfaceAssistActive = !bGroundedOnShoreExitSlope
                && (bLatchedAtSurfaceCeiling || bAtVisibleSurfaceCeiling);
            if (!bGroundedOnShoreExitSlope)
            {
                ApplySwimmingSurfaceRiseAssist(Movement, DeltaTime, SurfaceLimitDepth, SurfaceCeilingDepth, bWantsUpAtSurface, bWantsDownAtSurface);
            }
            if (bAtVisibleSurfaceCeiling)
            {
                // The stable head reference has reached the visible ceiling: upward
                // input is ignored, while deliberate downward input still lets the player dive.
                SwimMoveInput.Z = FMath::Min(SwimMoveInput.Z, 0.0f);
                CurrentSpeed.Z = bWantsDownAtSurface ? FMath::Min(CurrentSpeed.Z, 0.0f) : 0.0f;
            }

            const float SwimAccelTime = BaseTime * CharacterMovementTuning::SwimmingAccelerationTimeScale;
            const float SwimBrakeTime = BaseTime * CharacterMovementTuning::SwimmingBrakeTimeScale;
            CurrentSpeed.X = CalculateAcceleration(CurrentSpeed.X, SwimMoveInput.X, FMath::IsNearlyZero(SwimMoveInput.X, CharacterMovementTuning::SwimmingInputDeadZone) ? SwimBrakeTime : SwimAccelTime);
            CurrentSpeed.Y = CalculateAcceleration(CurrentSpeed.Y, SwimMoveInput.Y, FMath::IsNearlyZero(SwimMoveInput.Y, CharacterMovementTuning::SwimmingInputDeadZone) ? SwimBrakeTime : SwimAccelTime);
            CurrentSpeed.Z = CalculateAcceleration(CurrentSpeed.Z, SwimMoveInput.Z, FMath::IsNearlyZero(SwimMoveInput.Z, CharacterMovementTuning::SwimmingInputDeadZone) ? SwimBrakeTime : SwimAccelTime);

            if (!bGroundedOnShoreExitSlope)
            {
                ApplySwimmingSurfaceConstraint(Movement, CurrentSpeed, DeltaTime, SurfaceLimitDepth, SurfaceCeilingDepth, SwimSurfaceLockDepth, bSurfaceAssistActive);
            }

            const float CharacterHeight = FMath::Max(1.0f, HalfHeight * 2.0f);
            const float CharacterSubmergedAlpha = FMath::Clamp(CapsuleImmersionDepth / CharacterHeight, 0.0f, 1.0f);
            const float SurfacePlaneAlpha = ComputeSwimmingSurfacePlaneAlpha(SurfaceLimitDepth, SwimSurfaceLockDepth);
            const float ForwardSurfacePlaneAlpha = bIsSprint ? SurfacePlaneAlpha : 1.0f;
            ApplySwimmingVelocityDamping(Movement, SwimMoveInput, ControlRot, DeltaTime, CharacterSubmergedAlpha, ForwardSurfacePlaneAlpha, bAtVisibleSurfaceCeiling);

            const FVector SwimForward = MakeSwimmingForwardVector(ControlRot, ForwardSurfacePlaneAlpha);
            OwnerCharacter->AddMovementInput(MakeRightVectorFromYaw(ControlRot.Yaw), CurrentSpeed.X);
            OwnerCharacter->AddMovementInput(SwimForward, CurrentSpeed.Y);

            if (CurrentSpeed.Z < 0.0f || !bAtVisibleSurfaceCeiling)
            {
                OwnerCharacter->AddMovementInput(FVector::UpVector, CurrentSpeed.Z);
            }
        }
        else
        {
            bSwimmingSurfaceCeilingLocked = false;

            if (Movement->MovementMode == MOVE_Swimming)
            {
                // Leaving the water surface should keep horizontal momentum and choose the
                // physically correct dry mode instead of forcing a slow walking state in mid-air.
                Movement->SetMovementMode(bIsGrounded ? MOVE_Walking : MOVE_Falling);
            }

            CurrentSpeed.X = CalculateAcceleration(CurrentSpeed.X, MoveInput.X, BaseTime);
            CurrentSpeed.Y = CalculateAcceleration(CurrentSpeed.Y, MoveInput.Y, BaseTime);
            CurrentSpeed.Z = 0.0f;

            if (!bIsGrounded)
            {
                Movement->MaxWalkSpeed = CharacterMovementTuning::DryAirborneWalkSpeed;
                if (!bIsJumping)
                {
                    CurrentSpeed.X = 0.0f;
                    CurrentSpeed.Y = 0.0f;
                }
            }
            else
            {
                float TargetMaxSpeed = bIsSprint
                    ? CharacterMovementTuning::GroundSprintSpeed
                    : CharacterMovementTuning::GroundWalkSpeed;
                TargetMaxSpeed *= GetGroundedWaterWalkSpeedMultiplier(EffectiveWaterLevel);
                Movement->MaxWalkSpeed = ClampGroundSpeed(TargetMaxSpeed, GroundNormal.Z, Movement->GetWalkableFloorZ());
            }
            ApplyMoveRightForward(OwnerCharacter, ControlRot, CurrentSpeed);
        }

        ApplyCrouchState(bIsCrouch && bIsOnGround && !bCheckSwimming);
    }

    UpdateRagdoll(DeltaTime, OwnerCharacter, MeshComp);
}

void UCharacterComponent::ResetMovementState()
{
    ImpactVelocity = FVector::ZeroVector;
    CurrentSpeed = FVector::ZeroVector;
    PrevVelocity = FVector::ZeroVector;
    LastPreRagdollVelocity = FVector::ZeroVector;
    LastPreRagdollVelocityAge = TNumericLimits<float>::Max();
    bSwimmingSurfaceCeilingLocked = false;

    ApplyCrouchState(false);

    if (IsValid(Movement))
    {
        Movement->ConsumeInputVector();
        Movement->StopMovementImmediately();
    }
}

void UCharacterComponent::ClearSwimmingSurfaceConstraintState()
{
    // Flying and other explicit mode changes must not inherit a previous waterline latch.
    bSwimmingSurfaceCeilingLocked = false;
    CurrentSpeed.Z = 0.0f;
}

void UCharacterComponent::ClearRagdollWaterIntent(bool bClearSwimLock)
{
    bRagdollInWater = false;
    bRagdollRecoveryWantsSwimming = false;
    if (bClearSwimLock)
    {
        RagdollRecoverySwimLockTime = 0.0f;
    }
}

void UCharacterComponent::SetMovementModeAfterRagdollRecovery(UCharacterMovementComponent* CharacterMovement, const FCharacterRagdollEnvironmentState& RecoveryEnvironmentState) const
{
    if (!IsValid(CharacterMovement))
    {
        return;
    }

    const bool bUseGroundFallback = !RecoveryEnvironmentState.bIsValid
        || RecoveryEnvironmentState.bIsOnGround
        || RecoveryEnvironmentState.bForcedLandRecovery
        || RecoveryEnvironmentState.bTreatWaterAsGround;
    const bool bHasWalkableGround = bUseGroundFallback
        || CharacterMovement->IsMovingOnGround()
        || IsRagdollTouchingWalkableGround(65.0f, false);

    CharacterMovement->SetMovementMode(bHasWalkableGround ? MOVE_Walking : MOVE_Falling);
}

void UCharacterComponent::ResetRagdollRecoveryState(bool bKeepWaterIntent)
{
    if (IsValid(OwnerCharacter))
    {
        OwnerCharacter->GetWorldTimerManager().ClearTimer(RagdollCheckTimerHandle);
    }

    bGettingUp = false;
    bCheckingRagdollStay = false;
    RagdollWeight = 0.0f;
    RagdollActiveTime = 0.0f;
    RagdollLowSpeedTime = 0.0f;
    GetUpActiveTime = 0.0f;
    RagdollRecoverySwimLockTime = 0.0f;
    WaterRagdollRecoveryElapsed = 0.0f;
    bWaterRecoveryTransformInitialized = false;
    bPendingWaterRagdollDeactivation = false;
    bRagdollReleaseGroundTraceInFlight = false;
    bRagdollReleaseGroundTraceHitWalkable = false;
    bUseAsyncRagdollReleaseGroundResult = false;
    bAsyncRagdollReleaseGroundResult = false;
    PendingRagdollReleaseGroundTraceCount = 0;
    ++RagdollReleaseGroundTraceRequestId;
    bForceLandRagdollRecoveryOnce = false;
    bLandRagdollRecoveryOverridesWater = false;
    PendingWaterRagdollDeactivationLevel = 0.0f;
    RagdollEnvironmentState = FCharacterRagdollEnvironmentState();
    RagdollEnvironmentStateFrame = 0;

    WaterRecoveryActorStartLocation = FVector::ZeroVector;
    WaterRecoveryActorTargetLocation = FVector::ZeroVector;
    WaterRecoveryActorStartRotation = FRotator::ZeroRotator;
    WaterRecoveryActorTargetRotation = FRotator::ZeroRotator;
    WaterRecoveryMeshStartRelativeLocation = FVector::ZeroVector;
    WaterRecoveryMeshStartRelativeRotation = FRotator::ZeroRotator;
    RagdollPrePhysicsActorRotation = FRotator::ZeroRotator;
    bHasRagdollPrePhysicsActorRotation = false;
    RagdollCameraStabilizeRemainingTime = 0.0f;
    bSavedRagdollCameraState = false;

    if (!bKeepWaterIntent)
    {
        ClearRagdollWaterIntent(false);
    }
}

void UCharacterComponent::ClearRagdollSwimmingRecoveryLock(bool bKeepCurrentWaterState)
{
    // Do not interrupt an active ragdoll or the actual water recovery blend.  This is only for the
    // short post-recovery animation lock that was preventing explicit player controls, especially Flying.
    if (bIsRagdoll || bGettingUp || RagdollWeight > KINDA_SMALL_NUMBER)
    {
        return;
    }

    RagdollRecoverySwimLockTime = 0.0f;
    bRagdollRecoveryWantsSwimming = false;
    WaterRagdollRecoveryElapsed = 0.0f;
    bWaterRecoveryTransformInitialized = false;

    if (!bKeepCurrentWaterState)
    {
        bRagdollInWater = false;
    }
}

bool UCharacterComponent::TryGetHeadWaterReferenceLocation(FVector& OutLocation) const
{
    return TryGetSkeletalReferenceLocation(MeshComp.Get(), FName(BONE_HEAD), OutLocation);
}

float UCharacterComponent::GetDirectWaterCapsuleImmersionDepth(float InWaterLevel) const
{
    if (!IsValid(OwnerCharacter))
    {
        return -1.0e30f;
    }

    const float CapsuleHalfHeight = HalfHeight > KINDA_SMALL_NUMBER
        ? HalfHeight
        : (OwnerCharacter->GetCapsuleComponent() ? OwnerCharacter->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 0.0f);
    return ComputeCapsuleWaterImmersionDepth(OwnerCharacter->GetActorLocation(), CapsuleHalfHeight, InWaterLevel);
}

float UCharacterComponent::GetStableHeadEmergenceHeight() const
{
    const UCapsuleComponent* Capsule = IsValid(OwnerCharacter) ? OwnerCharacter->GetCapsuleComponent() : nullptr;
    const float CapsuleRadius = Radius > KINDA_SMALL_NUMBER
        ? Radius
        : (Capsule ? Capsule->GetScaledCapsuleRadius() : 0.0f);
    const float RadiusReference = CapsuleRadius > KINDA_SMALL_NUMBER
        ? CapsuleRadius
        : FMath::Max(1.0f, HalfHeight * 0.5f);

    // The ceiling is measured from the stable BONE_HEAD reference. The target is
    // deliberately a visible-head line: the body can swim while the head remains
    // above the surface instead of forcing the reference point underwater first.
    const float RequestedClearance = RadiusReference * CharacterWaterTuning::HeadSurfaceClearanceRadiusRatio;
    const float NativeMinimumClearance = FMath::Max(
        CharacterWaterTuning::HeadSurfaceClearanceMinCm,
        RadiusReference * CharacterWaterTuning::HeadSurfaceClearanceRadiusRatio);
    const float MaxClearance = FMath::Max(0.0f, WaterReferenceOffsetFromCapsule.Z - 1.0f);
    return FMath::Clamp(FMath::Max(RequestedClearance, NativeMinimumClearance), 0.0f, MaxClearance);
}

float UCharacterComponent::GetSwimEntryReferenceDepth() const
{
    // Entry happens when the stable head reference reaches the same visible-head
    // waterline used by the swimming surface ceiling. Positive depth is no longer
    // required, because that would mean the head reference is already underwater.
    return -GetStableHeadEmergenceHeight();
}

bool UCharacterComponent::IsCapsuleAtLeastHalfSubmerged(float InWaterLevel) const
{
    if (!IsValid(OwnerCharacter))
    {
        return false;
    }

    const UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent();
    const float CapsuleHalfHeight = HalfHeight > KINDA_SMALL_NUMBER
        ? HalfHeight
        : (Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 0.0f);
    if (CapsuleHalfHeight <= KINDA_SMALL_NUMBER)
    {
        return false;
    }

    const float CapsuleHeight = FMath::Max(1.0f, CapsuleHalfHeight * 2.0f);
    const float RequiredImmersionDepth = CapsuleHeight * CharacterWaterTuning::CapsuleSwimLockSubmergedRatio;
    const float StableRequiredDepth = FMath::Max(1.0f, RequiredImmersionDepth - CharacterWaterTuning::CapsuleSwimLockHysteresisCm);
    return GetDirectWaterCapsuleImmersionDepth(InWaterLevel) >= StableRequiredDepth;
}

bool UCharacterComponent::HasStableHeadReachedShoreExitLine(float DirectImmersionDepth) const
{
    // The player may step out only after the surface servo has visibly raised the
    // head reference above water.  This prevents fully submerged floor contact
    // from turning into Walking, while still allowing a real shoreline exit.
    const float RequiredEmergence = FMath::Max(2.0f, GetStableHeadEmergenceHeight() * CharacterWaterTuning::ShoreExitHeadClearanceRatio);
    return DirectImmersionDepth <= -RequiredEmergence;
}

float UCharacterComponent::GetGroundedWaterSwimOverrideDepth() const
{
    const UCapsuleComponent* Capsule = IsValid(OwnerCharacter) ? OwnerCharacter->GetCapsuleComponent() : nullptr;
    const float CapsuleRadius = Radius > KINDA_SMALL_NUMBER
        ? Radius
        : (Capsule ? Capsule->GetScaledCapsuleRadius() : 0.0f);
    const float RadiusReference = CapsuleRadius > KINDA_SMALL_NUMBER
        ? CapsuleRadius
        : FMath::Max(1.0f, HalfHeight * 0.5f);

    float EnterDepth = 0.0f;
    float ExitDepth = 0.0f;
    float SurfaceLockDepth = 0.0f;
    GetCapsuleSwimmingDepths(EnterDepth, ExitDepth, SurfaceLockDepth);
    (void)ExitDepth;
    (void)SurfaceLockDepth;

    // Ground support wins for shallow water, so a shoreline or thin floor cannot trap
    // the character in Swimming. The half-capsule submerged rule bypasses this gate;
    // below that, require a strong direct-reference signal before ground can be ignored.
    return FMath::Max(EnterDepth, RadiusReference * CharacterWaterTuning::GroundedSwimOverrideDepthRadiusRatio);
}

float UCharacterComponent::GetGroundedWaterWalkSpeedMultiplier(float InWaterLevel) const
{
    if (!IsValid(OwnerCharacter))
    {
        return 1.0f;
    }

    const UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent();
    if (!Capsule)
    {
        return 1.0f;
    }

    float WalkWaterLevel = InWaterLevel;
    const FVector ActorLocation = OwnerCharacter->GetActorLocation();
    const float CapsuleHalfHeight = HalfHeight > KINDA_SMALL_NUMBER
        ? HalfHeight
        : Capsule->GetScaledCapsuleHalfHeight();
    const FVector BottomLocation(ActorLocation.X, ActorLocation.Y, ActorLocation.Z - CapsuleHalfHeight);

    // Grounded wading can happen while STATE_WATER is intentionally rejected, so
    // query the water actor directly instead of relying on the state bit alone.
    const bool bHasWaterLevel = AWaterActor::FindWaterLevelAtLocationStrict(this, ActorLocation, WalkWaterLevel)
        || AWaterActor::FindWaterLevelAtLocationStrict(this, BottomLocation, WalkWaterLevel);
    if (!bHasWaterLevel)
    {
        return 1.0f;
    }

    FVector ReferenceLocation = FVector::ZeroVector;
    if (!TryGetWaterReferenceOrCapsuleFallbackLocation(ReferenceLocation))
    {
        ReferenceLocation = ActorLocation + FVector::UpVector * FMath::Max(1.0f, WaterOffset);
    }

    const float ReferenceHeightFromBottom = FMath::Max(1.0f, ReferenceLocation.Z - BottomLocation.Z);
    const float SubmergedRatio = FMath::Clamp((WalkWaterLevel - BottomLocation.Z) / ReferenceHeightFromBottom, 0.0f, 1.0f);
    const float StartRatio = CharacterWaterTuning::WalkWaterSlowStartSubmergedRatio;
    const float FullRatio = FMath::Max(StartRatio + KINDA_SMALL_NUMBER, CharacterWaterTuning::WalkWaterSlowFullSubmergedRatio);
    const float LinearAlpha = FMath::Clamp((SubmergedRatio - StartRatio) / (FullRatio - StartRatio), 0.0f, 1.0f);
    const float SmoothAlpha = LinearAlpha * LinearAlpha * (3.0f - 2.0f * LinearAlpha);

    // Scale only the dry/wading walk speed. Full Swimming still uses the swim
    // movement branch, while shallow shoreline contact remains close to normal speed.
    return FMath::Lerp(1.0f, CharacterWaterTuning::WalkWaterMinSpeedMultiplier, SmoothAlpha);
}

bool UCharacterComponent::TraceCharacterWalkableGroundFromAbove(FHitResult& OutHit, float ExtraDownDistance) const
{
    OutHit = FHitResult();

    if (!IsValid(OwnerCharacter))
    {
        return false;
    }

    UWorld* World = OwnerCharacter->GetWorld();
    const UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent();
    if (!World || !Capsule)
    {
        return false;
    }

    const float CapsuleHalfHeight = HalfHeight > KINDA_SMALL_NUMBER
        ? HalfHeight
        : Capsule->GetScaledCapsuleHalfHeight();
    const float CapsuleRadius = Radius > KINDA_SMALL_NUMBER
        ? Radius
        : Capsule->GetScaledCapsuleRadius();

    const FVector ActorLocation = OwnerCharacter->GetActorLocation();
    const float StartLift = GetRaisedGroundTraceStartLift(CapsuleRadius);
    const float DefaultBelowCapsuleReach = FMath::Max(10.0f, CapsuleRadius * 0.30f);
    const float DownReach = CapsuleHalfHeight + DefaultBelowCapsuleReach + FMath::Max(0.0f, ExtraDownDistance);
    const FVector Start = ActorLocation + FVector::UpVector * StartLift;
    const FVector End = ActorLocation - FVector::UpVector * DownReach;

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(CharacterGroundCheck), true, OwnerCharacter);
    AddCharacterSelfIgnore(QueryParams, OwnerCharacter.Get(), MeshComp.Get());
    QueryParams.bTraceComplex = true;

    return TraceWalkableGroundByChannel(World, Start, End, Movement.Get(), QueryParams, &OutHit);
}

bool UCharacterComponent::IsCharacterSupportedByWalkableGround(float ExtraDownDistance) const
{
    FHitResult GroundHit;
    return TraceCharacterWalkableGroundFromAbove(GroundHit, ExtraDownDistance);
}

bool UCharacterComponent::IsGroundSupportBlockingDirectWater(float InWaterLevel, float DirectImmersionDepth, bool bCurrentlySwimming) const
{
    FHitResult GroundHit;
    const float ExtraReach = FMath::Max(18.0f, Radius * 0.45f);
    if (!TraceCharacterWalkableGroundFromAbove(GroundHit, ExtraReach))
    {
        return false;
    }

    const float SurfaceBlockMargin = FMath::Max(
        CharacterWaterTuning::ShoreExitGroundSurfaceMinMarginCm,
        Radius * CharacterWaterTuning::ShoreExitGroundSurfaceMarginRadiusRatio);
    const bool bWalkableGroundNearSurface = GroundHit.ImpactPoint.Z >= InWaterLevel - SurfaceBlockMargin;
    const bool bCapsuleAtLeastHalfSubmerged = IsCapsuleAtLeastHalfSubmerged(InWaterLevel);

    if (bWalkableGroundNearSurface)
    {
        // A walkable hit that is close to the water surface is shoreline support,
        // not a deep underwater floor. Let it win immediately so a swimming
        // character can step/walk out even if the capsule is still around half
        // submerged for a few frames. Deep floor contact is handled by the branch
        // below and still keeps MOVE_Swimming.
        return true;
    }

    if (bCapsuleAtLeastHalfSubmerged)
    {
        // Deep contact wins over ground contact. Once at least half of the capsule is
        // underwater and the walkable hit is not near the surface, stay Swimming.
        return false;
    }

    // If there is walkable support below shallow water, prefer walking/wading until
    // the stable head reference reaches the visible-head swim line.
    return DirectImmersionDepth < GetGroundedWaterSwimOverrideDepth();
}

bool UCharacterComponent::TryGetStableWaterReferenceLocation(FVector& OutLocation) const
{
    const UCapsuleComponent* Capsule = IsValid(OwnerCharacter) ? OwnerCharacter->GetCapsuleComponent() : nullptr;
    if (!Capsule || !bHasWaterReferenceOffsetFromCapsule)
    {
        return false;
    }

    OutLocation = Capsule->GetComponentTransform().TransformPositionNoScale(WaterReferenceOffsetFromCapsule);
    return !OutLocation.ContainsNaN();
}

bool UCharacterComponent::TryGetWaterReferenceOrCapsuleFallbackLocation(FVector& OutLocation) const
{
    if (TryGetStableWaterReferenceLocation(OutLocation))
    {
        return true;
    }

    const UCapsuleComponent* Capsule = IsValid(OwnerCharacter) ? OwnerCharacter->GetCapsuleComponent() : nullptr;
    if (!Capsule)
    {
        return false;
    }

    const float CapsuleHalfHeight = HalfHeight > KINDA_SMALL_NUMBER
        ? HalfHeight
        : Capsule->GetScaledCapsuleHalfHeight();
    const float CapsuleRadius = Radius > KINDA_SMALL_NUMBER
        ? Radius
        : Capsule->GetScaledCapsuleRadius();

    // Early ticks may need a water height before the async mesh has finished.
    // This fallback is deliberately temporary and never writes to member state.
    const FVector FallbackOffset = SanitizeWaterReferenceOffset(
        FVector(0.0f, 0.0f, FMath::Max(1.0f, CapsuleHalfHeight * CharacterWaterTuning::FallbackHeadOffsetHalfHeightRatio)),
        CapsuleHalfHeight,
        CapsuleRadius,
        true);

    OutLocation = Capsule->GetComponentTransform().TransformPositionNoScale(FallbackOffset);
    return !OutLocation.ContainsNaN();
}

float UCharacterComponent::GetDirectWaterImmersionDepth(float InWaterLevel) const
{
    if (!IsValid(OwnerCharacter))
    {
        return -1.0e30f;
    }

    FVector ReferenceLocation = FVector::ZeroVector;
    if (TryGetWaterReferenceOrCapsuleFallbackLocation(ReferenceLocation))
    {
        // Positive depth means the stable head/fallback point is below the water surface.
        // The live head bone is intentionally not sampled here, because animation bobbing
        // should not toggle Swimming from frame to frame.
        return InWaterLevel - ReferenceLocation.Z;
    }

    return InWaterLevel - (OwnerCharacter->GetActorLocation().Z + FMath::Max(1.0f, WaterOffset));
}

void UCharacterComponent::GetCapsuleSwimmingDepths(float& OutEnterDepth, float& OutExitDepth, float& OutSurfaceLockDepth) const
{
    const UCapsuleComponent* Capsule = IsValid(OwnerCharacter) ? OwnerCharacter->GetCapsuleComponent() : nullptr;
    const float CapsuleHalfHeight = HalfHeight > KINDA_SMALL_NUMBER
        ? HalfHeight
        : (Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 0.0f);
    const float CapsuleRadius = Radius > KINDA_SMALL_NUMBER
        ? Radius
        : (Capsule ? Capsule->GetScaledCapsuleRadius() : 0.0f);

    // The water actor keeps its original loose surface tolerance. These depths drive
    // the near-surface latch/correction band only. Actual water entry uses the stable
    // visible-head reference line from GetSwimEntryReferenceDepth().
    const float RadiusReference = CapsuleRadius > KINDA_SMALL_NUMBER
        ? CapsuleRadius
        : FMath::Max(1.0f, CapsuleHalfHeight * 0.5f);
    const float MaxCapsuleDepth = FMath::Max(RadiusReference + 1.0f, CapsuleHalfHeight * 2.0f);

    OutEnterDepth = FMath::Clamp(
        RadiusReference * CharacterWaterTuning::SwimSurfaceCaptureDepthRadiusRatio,
        1.0f,
        MaxCapsuleDepth);

    OutExitDepth = FMath::Clamp(
        RadiusReference * CharacterWaterTuning::SwimExitPaddingRadiusRatio,
        0.0f,
        OutEnterDepth);

    // The surface-lock depth is a near-surface control band measured from the
    // stable head reference. It gives existing swimmers enough hysteresis for
    // the ceiling correction to pull them back instead of popping out.
    OutSurfaceLockDepth = FMath::Min(
        MaxCapsuleDepth,
        FMath::Max(OutEnterDepth + 1.0f, RadiusReference * CharacterWaterTuning::SurfaceLockDepthRadiusRatio));
}

bool UCharacterComponent::ShouldUseDirectWaterState(float InWaterLevel, bool bCurrentlySwimming) const
{
    if (!bHasWaterReferenceOffsetFromCapsule)
    {
        // Normal water entry depends on the post-load BONE_HEAD/capsule reference.
        // Until that reference exists, keep overlap-only water hits from pushing the
        // character into Swimming with an early temporary fallback height.
        return false;
    }

    float SurfaceCaptureDepth = 0.0f;
    float SurfaceCeilingDepth = 0.0f;
    float SurfaceLockDepth = 0.0f;
    GetCapsuleSwimmingDepths(SurfaceCaptureDepth, SurfaceCeilingDepth, SurfaceLockDepth);
    (void)SurfaceCaptureDepth;
    (void)SurfaceCeilingDepth; // The movement constraint, not the water-state gate, owns the visible ceiling.

    const float SwimEntryReferenceDepth = GetSwimEntryReferenceDepth();
    const float ExistingSwimCorrectionDepth = -(SurfaceLockDepth + GetStableHeadEmergenceHeight());
    const float RequiredDepth = bCurrentlySwimming ? ExistingSwimCorrectionDepth : SwimEntryReferenceDepth;
    const float DirectImmersionDepth = GetDirectWaterImmersionDepth(InWaterLevel);
    const bool bCapsuleAtLeastHalfSubmerged = IsCapsuleAtLeastHalfSubmerged(InWaterLevel);

    if (IsGroundSupportBlockingDirectWater(InWaterLevel, DirectImmersionDepth, bCurrentlySwimming))
    {
        return false;
    }

    // Entry waits for the visible-head reference line: roughly the point where a
    // person would swim with only the head above water. Existing swimmers also stay
    // in Swimming whenever at least half the capsule is still submerged, even if a
    // walkable floor is touching the capsule.
    return DirectImmersionDepth >= RequiredDepth
        || (bCurrentlySwimming && bCapsuleAtLeastHalfSubmerged);
}

bool UCharacterComponent::RefreshRagdollWaterDetection(float* OutDetectedWaterLevel)
{
    if (!IsValid(MeshComp) || !IsRagdollLikeState())
    {
        return false;
    }

    float DetectedWaterLevel = OutDetectedWaterLevel ? *OutDetectedWaterLevel : 0.0f;
    FCharacterRagdollEnvironmentState CurrentEnvironmentState = UpdateRagdollEnvironmentStateForRelease(DetectedWaterLevel);
    DetectedWaterLevel = CurrentEnvironmentState.WaterLevel;

    if (!ShouldUseRagdollWaterRecoveryForState(CurrentEnvironmentState))
    {
        ClearRagdollWaterIntent();
        if (OutDetectedWaterLevel)
        {
            *OutDetectedWaterLevel = DetectedWaterLevel;
        }
        return false;
    }

    bRagdollInWater = true;
    if (!bIsRagdoll && (bGettingUp || RagdollWeight > 0.0f))
    {
        bRagdollRecoveryWantsSwimming = true;
    }

    if (OutDetectedWaterLevel)
    {
        *OutDetectedWaterLevel = DetectedWaterLevel;
    }

    if (IsValid(Movement))
    {
        StopMovementAndSetMode(Movement, MOVE_Swimming);
    }

    return true;
}

FCharacterRagdollEnvironmentState UCharacterComponent::UpdateRagdollEnvironmentStateForRelease(float InitialWaterLevel, bool bUseGroundOverride, bool bGroundOverride)
{
    const uint64 CurrentFrame = GFrameCounter;

    // Multiple systems ask for the same ragdoll release state in the same frame
    // (controller tick, component update, animation update). Reuse the first full probe so
    // the final release decision cannot flip just because it was sampled twice.
    if (!bUseGroundOverride
        && bIsRagdoll
        && !bPendingWaterRagdollDeactivation
        && !bForceLandRagdollRecoveryOnce
        && !bLandRagdollRecoveryOverridesWater
        && RagdollEnvironmentStateFrame == CurrentFrame
        && RagdollEnvironmentState.bIsValid)
    {
        return RagdollEnvironmentState;
    }

    FCharacterRagdollEnvironmentState State;
    State.WaterLevel = InitialWaterLevel;

    auto StoreAndReturn = [this, CurrentFrame](const FCharacterRagdollEnvironmentState& NewState)
    {
        RagdollEnvironmentState = NewState;
        RagdollEnvironmentStateFrame = CurrentFrame;
        return NewState;
    };

    if (!IsValid(OwnerCharacter) || !IsValid(MeshComp))
    {
        return StoreAndReturn(State);
    }

    State.bIsValid = true;
    State.bMovementWasSwimming = IsValid(Movement) && Movement->IsSwimming();
    State.bMovementWasFalling = IsValid(Movement) && Movement->IsFalling();
    State.bMovementWasOnGround = IsValid(Movement) && Movement->IsMovingOnGround();

    float OwnerDirectWaterLevel = State.WaterLevel;
    const bool bOwnerInsideWaterColumn = AWaterActor::FindWaterLevelAtLocationStrict(this, OwnerCharacter->GetActorLocation(), OwnerDirectWaterLevel)
        || AWaterActor::FindWaterLevelAtLocationStrict(this, OwnerCharacter->GetBottomLocation(), OwnerDirectWaterLevel);
    const bool bOwnerHasVerifiedWaterState = bOwnerInsideWaterColumn && ShouldUseDirectWaterState(OwnerDirectWaterLevel, State.bMovementWasSwimming);

    const bool bUsingStoredLandFallback = bLandRagdollRecoveryOverridesWater
        && !bIsRagdoll
        && RagdollEnvironmentState.bIsValid
        && (RagdollEnvironmentState.bForcedLandRecovery || RagdollEnvironmentState.bTreatWaterAsGround || !RagdollEnvironmentState.bIsInWater);
    if (bUsingStoredLandFallback)
    {
        State = RagdollEnvironmentState;
        State.bMovementWasSwimming = IsValid(Movement) && Movement->IsSwimming();
        State.bMovementWasFalling = IsValid(Movement) && Movement->IsFalling();
        State.bMovementWasOnGround = IsValid(Movement) && Movement->IsMovingOnGround();
        State.bIsOnGround = true;
        State.bForcedLandRecovery = true;
        State.bRagdollMeaningfullySubmerged = false;
        State.bShouldRecoverInWater = false;
        State.bShouldDelayDeactivation = false;
        return StoreAndReturn(State);
    }

    // Once underwater recovery has committed and physics has been disabled, keep the release
    // snapshot. The attached mesh is blending back to animation, so re-probing it can read a
    // temporary pose and incorrectly flip to land or swimming for one frame.
    if (!bIsRagdoll
        && bWaterRecoveryTransformInitialized
        && RagdollWeight > KINDA_SMALL_NUMBER
        && RagdollEnvironmentState.bIsValid
        && (bRagdollInWater || bRagdollRecoveryWantsSwimming)
        && RagdollEnvironmentState.bShouldRecoverInWater
        && !RagdollEnvironmentState.bForcedLandRecovery)
    {
        State = RagdollEnvironmentState;
        State.bMovementWasSwimming = IsValid(Movement) && Movement->IsSwimming();
        State.bMovementWasFalling = IsValid(Movement) && Movement->IsFalling();
        State.bMovementWasOnGround = IsValid(Movement) && Movement->IsMovingOnGround();
        State.bIsInWater = true;
        State.bTreatWaterAsGround = false;
        State.bForcedLandRecovery = false;
        State.bRagdollMeaningfullySubmerged = true;
        State.bShouldRecoverInWater = true;
        State.bShouldDelayDeactivation = false;
        return StoreAndReturn(State);
    }

    FRagdollProbeLocationArray RagdollProbeLocations;
    GatherRagdollProbeLocations(MeshComp, RagdollProbeLocations, true, false);
    UpdateRagdollProbeReferenceState(RagdollProbeLocations, State);

    FRagdollProbeLocationArray CoreRagdollProbeLocations;
    GatherRagdollProbeLocations(MeshComp, CoreRagdollProbeLocations, true, true);

    // The all-body list already contains the preferred core probes, so one filtered water scan is enough.
    State.bIsInWater = ProbeRagdollWaterLevelFromLocations(this, RagdollProbeLocations, State.WaterLevel);

    if (!State.bIsInWater && State.RagdollProbeLocationCount > 0)
    {
        // Use the ragdoll pose, not the stale capsule, to emulate the normal character water check.
        FRagdollSmallProbeLocationArray RagdollCharacterProbeLocations;
        const FVector HipsLocation = UCharacterFunctionLibrary::GetBoneLocation(*MeshComp, BONE_HIPS);
        const FVector RagdollActorLocation = GetRagdollRecoveryActorLocationFromHips(HipsLocation, false);
        RagdollCharacterProbeLocations.Add(RagdollActorLocation);
        RagdollCharacterProbeLocations.Add(RagdollActorLocation - FVector::UpVector * FMath::Max(1.0f, HalfHeight));
        RagdollCharacterProbeLocations.Add(State.RagdollReferenceLocation);
        State.bIsInWater = ProbeRagdollWaterLevelFromLocations(this, RagdollCharacterProbeLocations, State.WaterLevel);
    }

    const FRagdollSubmersionMetrics AllSubmersion = State.bIsInWater
        ? ComputeRagdollSubmersionMetrics(RagdollProbeLocations, State.WaterLevel)
        : FRagdollSubmersionMetrics();
    const FRagdollSubmersionMetrics CoreSubmersion = State.bIsInWater
        ? ComputeRagdollSubmersionMetrics(CoreRagdollProbeLocations, State.WaterLevel)
        : FRagdollSubmersionMetrics();

    State.RagdollMaxSubmersionDepth = FMath::Max(AllSubmersion.MaxDepth, CoreSubmersion.MaxDepth);
    State.RagdollAverageSubmersionDepth = CoreSubmersion.SubmergedCount > 0 ? CoreSubmersion.AverageDepth : AllSubmersion.AverageDepth;
    State.RagdollSubmergedProbeCount = AllSubmersion.SubmergedCount;

    const float CoreDepthThreshold = FMath::Max(0.0f, CharacterRagdollTuning::WaterRecoveryCoreDepth);
    const float AverageDepthThreshold = FMath::Max(0.0f, CharacterRagdollTuning::WaterRecoveryAverageDepth);
    const float ReleaseHysteresisDepth = FMath::Max(0.0f, CharacterRagdollTuning::WaterReleaseHysteresisDepth);
    const int32 MinStableCoreProbeCount = FMath::Max(1, CharacterRagdollTuning::WaterReleaseMinCoreProbes);

    const bool bCoreMeaningfullySubmerged = CoreSubmersion.SubmergedCount > 0
        && (CoreSubmersion.MaxDepth >= CoreDepthThreshold
            || CoreSubmersion.AverageDepth >= AverageDepthThreshold + ReleaseHysteresisDepth
            || (CoreSubmersion.SubmergedCount >= MinStableCoreProbeCount && CoreSubmersion.AverageDepth >= FMath::Max(ReleaseHysteresisDepth, AverageDepthThreshold)));
    const int32 MinStableBodyProbeCount = FMath::Max(3, MinStableCoreProbeCount + 1);
    const bool bBodyMeaningfullySubmerged = AllSubmersion.SubmergedCount >= MinStableBodyProbeCount
        && (AllSubmersion.MaxDepth >= FMath::Max(CoreDepthThreshold, CharacterRagdollTuning::WaterReleaseDepthBelowSurface * 0.50f)
            || AllSubmersion.AverageDepth >= AverageDepthThreshold + ReleaseHysteresisDepth);
    const bool bReferenceEvidenceAllowed = CoreSubmersion.SubmergedCount > 0 || AllSubmersion.SubmergedCount >= MinStableBodyProbeCount;
    const bool bReferenceBelowSurface = bReferenceEvidenceAllowed
        && State.RagdollProbeLocationCount > 0
        && State.RagdollReferenceLocation.Z < State.WaterLevel - (CoreDepthThreshold + ReleaseHysteresisDepth);

    State.bRagdollMeaningfullySubmerged = State.bIsInWater && (bCoreMeaningfullySubmerged || bBodyMeaningfullySubmerged || bReferenceBelowSurface);
    // The final deactivation path can pass an async ground result captured immediately
    // before disabling physics. Other refresh paths keep using the synchronous probe.
    State.bIsOnGround = bUseGroundOverride ? bGroundOverride : IsRagdollTouchingWalkableGround();

    // Dry releases may only use land recovery when the async/sync ground probe found
    // walkable support. A dry-but-airborne ragdoll must stay active instead of starting
    // a get-up blend in mid-air.
    if (!State.bIsInWater)
    {
        State.bTreatWaterAsGround = false;
        State.bForcedLandRecovery = State.bIsOnGround;
        State.bRagdollMeaningfullySubmerged = false;
        State.bShouldRecoverInWater = false;
        State.bShouldDelayDeactivation = false;
        return StoreAndReturn(State);
    }

    const bool bCommittedToWaterRecovery = !bForceLandRagdollRecoveryOnce
        && !bLandRagdollRecoveryOverridesWater
        && !bIsRagdoll
        && bOwnerHasVerifiedWaterState
        && (bRagdollRecoveryWantsSwimming || (bRagdollInWater && RagdollWeight > 0.0f) || RagdollRecoverySwimLockTime > 0.0f);

    if (State.bIsOnGround && !bOwnerHasVerifiedWaterState && !bCommittedToWaterRecovery)
    {
        // A dry, ground-supported ragdoll must recover as land even if a stale flying/water
        // sequence left the capsule in MOVE_Swimming or a broad water bounds check touches a probe.
        State.bTreatWaterAsGround = true;
        State.bForcedLandRecovery = true;
        State.bRagdollMeaningfullySubmerged = false;
        State.bShouldRecoverInWater = false;
        State.bShouldDelayDeactivation = false;
        return StoreAndReturn(State);
    }

    const bool bStableWaterRecoveryEvidence = State.bRagdollMeaningfullySubmerged || bCommittedToWaterRecovery;
    const bool bShallowEnoughForLandRecovery = !State.bRagdollMeaningfullySubmerged
        && State.RagdollMaxSubmersionDepth <= FMath::Max(0.0f, CharacterRagdollTuning::TreatWaterAsGroundMaxDepth);
    const bool bUnstableWaterContact = State.bIsInWater && !bStableWaterRecoveryEvidence;

    // This is the anti-flicker rule: a one-frame water contact is treated as land unless the
    // core ragdoll body is clearly submerged. No-ground + one wet foot is not enough anymore.
    State.bTreatWaterAsGround = State.bIsInWater
        && !bCommittedToWaterRecovery
        && (bUnstableWaterContact || (State.bIsOnGround && bShallowEnoughForLandRecovery));
    State.bForcedLandRecovery = bForceLandRagdollRecoveryOnce || State.bTreatWaterAsGround;
    State.bShouldRecoverInWater = State.bIsInWater
        && !State.bForcedLandRecovery
        && bStableWaterRecoveryEvidence;
    State.bShouldDelayDeactivation = State.bShouldRecoverInWater && ShouldDelayWaterRagdollDeactivation(State.WaterLevel, State.bIsOnGround);

    return StoreAndReturn(State);
}

bool UCharacterComponent::FindRagdollWaterLevel(float &OutWaterLevel) const
{
    if (!IsValid(MeshComp) || !IsRagdollLikeState())
    {
        return false;
    }

    if (bLandRagdollRecoveryOverridesWater && !bIsRagdoll)
    {
        return false;
    }

    // Animation code asks this every frame. Use the already-filtered release snapshot instead
    // of running another raw water probe that could see a single noisy limb contact.
    if (RagdollEnvironmentState.bIsValid && ShouldUseRagdollWaterRecoveryForState(RagdollEnvironmentState))
    {
        OutWaterLevel = RagdollEnvironmentState.WaterLevel;
        return true;
    }

    return false;
}

FVector UCharacterComponent::GetRagdollRecoveryActorLocationFromHips(const FVector& HipsLocation, bool bWaterRecovery) const
{
    float ActorZOffsetFromHips = HalfHeight - Radius * 0.33333f;
    if (bWaterRecovery)
    {
        ActorZOffsetFromHips += CharacterRagdollTuning::WaterRecoveryActorZOffset;
    }
    return HipsLocation + FVector(0.0f, 0.0f, ActorZOffsetFromHips);
}

FVector UCharacterComponent::ResolveRagdollRecoveryGroundPenetration(const FVector& DesiredActorLocation) const
{
    if (!IsValid(OwnerCharacter))
    {
        return DesiredActorLocation;
    }

    UWorld* World = OwnerCharacter->GetWorld();
    const UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent();
    if (!World || !Capsule || DesiredActorLocation.ContainsNaN())
    {
        return DesiredActorLocation;
    }

    const float CapsuleHalfHeight = FMath::Max(1.0f, Capsule->GetScaledCapsuleHalfHeight());
    const FVector Start = DesiredActorLocation + FVector::UpVector * (CapsuleHalfHeight + CharacterRagdollTuning::LandRecoveryGroundProbeUp);
    const FVector End = DesiredActorLocation - FVector::UpVector * (CapsuleHalfHeight + CharacterRagdollTuning::LandRecoveryGroundProbeDown);

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(CharacterRagdollRecoveryGroundLift), true, OwnerCharacter);
    AddCharacterSelfIgnore(QueryParams, OwnerCharacter.Get(), MeshComp.Get());
    QueryParams.bTraceComplex = true;

    FHitResult GroundHit;
    if (!TraceWalkableGroundByChannel(World, Start, End, Movement.Get(), QueryParams, &GroundHit))
    {
        return DesiredActorLocation;
    }

    const float CurrentCapsuleBottomZ = DesiredActorLocation.Z - CapsuleHalfHeight;
    const float MinimumCapsuleBottomZ = GroundHit.ImpactPoint.Z + CharacterRagdollTuning::LandRecoveryGroundClearance;
    if (CurrentCapsuleBottomZ >= MinimumCapsuleBottomZ)
    {
        return DesiredActorLocation;
    }

    // Lift the owning capsule just enough to clear the floor. Foot IK can polish
    // the visible pose later, but root/capsule penetration must be solved here.
    FVector AdjustedLocation = DesiredActorLocation;
    AdjustedLocation.Z += MinimumCapsuleBottomZ - CurrentCapsuleBottomZ;
    return AdjustedLocation;
}

bool UCharacterComponent::ShouldUseRagdollWaterRecoveryForState(const FCharacterRagdollEnvironmentState& State) const
{
    if (!State.bIsValid || !State.bIsInWater || State.bTreatWaterAsGround || State.bForcedLandRecovery)
    {
        return false;
    }

    // The final recovery path must be conservative. A single submerged non-core probe is treated
    // as noise, because that was the source of intermittent land -> swimming flips at deactivation.
    return State.bShouldRecoverInWater && State.bRagdollMeaningfullySubmerged;
}

bool UCharacterComponent::ApplyRagdollReleaseEnvironmentStateToOwner(ACharacterController *InOwner, FCharacterRagdollEnvironmentState &ReleaseEnvironmentState)
{
    if (!IsValid(InOwner))
    {
        ReleaseEnvironmentState.bShouldRecoverInWater = false;
        ReleaseEnvironmentState.bShouldDelayDeactivation = false;
        RagdollEnvironmentState = ReleaseEnvironmentState;
        return false;
    }

    if (!ReleaseEnvironmentState.bIsValid)
    {
        ReleaseEnvironmentState = UpdateRagdollEnvironmentStateForRelease(ReleaseEnvironmentState.WaterLevel);
    }

    auto ForceLandRecovery = [&]() -> bool
    {
        ReleaseEnvironmentState.bIsOnGround = true;
        ReleaseEnvironmentState.bForcedLandRecovery = true;
        ReleaseEnvironmentState.bTreatWaterAsGround = false;
        ReleaseEnvironmentState.bRagdollMeaningfullySubmerged = false;
        ReleaseEnvironmentState.bShouldRecoverInWater = false;
        ReleaseEnvironmentState.bShouldDelayDeactivation = false;
        bLandRagdollRecoveryOverridesWater = true;
        ClearRagdollWaterIntent();
        InOwner->RefreshWaterStateForRagdollRecovery(false, ReleaseEnvironmentState.WaterLevel);
        StopMovementAndDisable(Movement);
        RagdollEnvironmentState = ReleaseEnvironmentState;
        return false;
    };

    // Force the owner/controller state from the release snapshot while physics bodies are still live.
    // Never let a previous capsule overlap or stale MOVE_Swimming decide the blend path.
    const bool bForcedLandRecovery = bForceLandRagdollRecoveryOnce
        || ReleaseEnvironmentState.bForcedLandRecovery
        || ReleaseEnvironmentState.bTreatWaterAsGround;
    if (bForcedLandRecovery || !ShouldUseRagdollWaterRecoveryForState(ReleaseEnvironmentState))
    {
        return ForceLandRecovery();
    }

    if (!InOwner->RefreshWaterStateForRagdollRecovery(true, ReleaseEnvironmentState.WaterLevel))
    {
        return ForceLandRecovery();
    }

    ReleaseEnvironmentState.bShouldRecoverInWater = true;
    ReleaseEnvironmentState.bRagdollMeaningfullySubmerged = true;
    bForceLandRagdollRecoveryOnce = false;
    bLandRagdollRecoveryOverridesWater = false;
    bRagdollInWater = true;
    bRagdollRecoveryWantsSwimming = false;
    RagdollRecoverySwimLockTime = 0.0f;

    StopMovementAndSetMode(Movement, MOVE_Swimming);

    RagdollEnvironmentState = ReleaseEnvironmentState;
    return true;
}

bool UCharacterComponent::RefreshRagdollWaterStateForAnimation()
{
    if (!IsValid(OwnerCharacter) || !IsValid(MeshComp))
    {
        return false;
    }

    const bool bNeedsRefresh = IsRagdollLikeState() || ShouldKeepSwimmingAfterWaterRagdoll();
    if (!bNeedsRefresh)
    {
        return false;
    }

    if (bLandRagdollRecoveryOverridesWater && IsRagdollLikeState())
    {
        if (RagdollEnvironmentState.bIsValid)
        {
            RagdollEnvironmentState.bIsOnGround = true;
            RagdollEnvironmentState.bForcedLandRecovery = true;
            RagdollEnvironmentState.bShouldRecoverInWater = false;
            RagdollEnvironmentState.bShouldDelayDeactivation = false;
        }
        SetRagdollWaterState(false, true);
        return false;
    }

    float DetectedWaterLevel = 0.0f;
    const FCharacterRagdollEnvironmentState CurrentEnvironmentState = UpdateRagdollEnvironmentStateForRelease(DetectedWaterLevel);
    DetectedWaterLevel = CurrentEnvironmentState.WaterLevel;

    bool bActuallyInWater = false;
    if (IsRagdollLikeState())
    {
        bActuallyInWater = ShouldUseRagdollWaterRecoveryForState(CurrentEnvironmentState);
    }
    else
    {
        const bool bActorInsideWaterColumn = AWaterActor::FindWaterLevelAtLocationStrict(this, OwnerCharacter->GetActorLocation(), DetectedWaterLevel)
            || AWaterActor::FindWaterLevelAtLocationStrict(this, OwnerCharacter->GetBottomLocation(), DetectedWaterLevel);
        bActuallyInWater = bActorInsideWaterColumn && ShouldUseDirectWaterState(DetectedWaterLevel, IsValid(Movement) && Movement->MovementMode == MOVE_Swimming);
    }

    if (bActuallyInWater)
    {
        bRagdollInWater = true;
        if (!bIsRagdoll && (bGettingUp || RagdollWeight > 0.0f || RagdollRecoverySwimLockTime > 0.0f))
        {
            bRagdollRecoveryWantsSwimming = true;
        }

        // During the actual ragdoll/get-up blend, movement and animation must agree on Swimming.
        // After recovery, the short swim lock is animation-only and must not fight player input.
        if (IsValid(Movement) && IsRagdollLikeState())
        {
            StopMovementAndSetMode(Movement, MOVE_Swimming);
        }
        return true;
    }

    SetRagdollWaterState(false, true);
    return false;
}

void UCharacterComponent::SetRagdollWaterState(bool bInWater, bool bForce)
{
    if (bInWater)
    {
        bRagdollInWater = true;
        if (bIsRagdoll || bGettingUp || RagdollWeight > 0.0f)
        {
            if (!bIsRagdoll)
            {
                bRagdollRecoveryWantsSwimming = true;
            }

            StopMovementAndSetMode(Movement, MOVE_Swimming);
        }
        return;
    }

    // Overlap end can fire while detached ragdoll bodies are still submerged.  Keep the
    // water state only if a fresh physics/body probe still finds water; forced clears are
    // used right before recovery animation updates so stale Swimming cannot leak onto land.
    float CurrentWaterLevel = 0.0f;
    if (!bForce && IsRagdollLikeState() && FindRagdollWaterLevel(CurrentWaterLevel))
    {
        bRagdollInWater = true;
        return;
    }

    ClearRagdollWaterIntent();

    if (IsValid(Movement) && Movement->MovementMode == MOVE_Swimming)
    {
        Movement->StopMovementImmediately();
        if (bIsRagdoll || bGettingUp || RagdollWeight > 0.0f)
        {
            Movement->DisableMovement();
        }
        else
        {
            Movement->SetMovementMode(MOVE_Walking);
        }
    }
}


void UCharacterComponent::ClearPendingWaterRagdollDeactivation()
{
    bPendingWaterRagdollDeactivation = false;
    PendingWaterRagdollDeactivationLevel = 0.0f;
}

bool UCharacterComponent::IsRagdollTouchingWalkableGround(float TraceDistance, bool bCoreOnly) const
{
    if (!IsValid(OwnerCharacter) || !IsValid(MeshComp))
    {
        return false;
    }

    UWorld* World = OwnerCharacter->GetWorld();
    if (!World)
    {
        return false;
    }

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(CharacterRagdollGroundCheck), false, OwnerCharacter);
    AddCharacterSelfIgnore(QueryParams, OwnerCharacter.Get(), MeshComp.Get());

    FRagdollProbeLocationArray RagdollGroundProbeLocations;
    GatherRagdollProbeLocations(MeshComp, RagdollGroundProbeLocations, true, bCoreOnly);
    const float BoneTraceDistance = GetRagdollGroundTraceDistance(TraceDistance, Radius);

    for (const FVector& RagdollLocation : RagdollGroundProbeLocations)
    {
        const FVector TraceStart = GetRagdollGroundTraceStart(RagdollLocation, Radius);
        const FVector TraceEnd = GetRagdollGroundTraceEnd(RagdollLocation, BoneTraceDistance);
        if (TraceWalkableGroundByChannel(World, TraceStart, TraceEnd, Movement.Get(), QueryParams))
        {
            return true;
        }
    }

    return false;
}

void UCharacterComponent::RequestAsyncRagdollReleaseGroundTrace()
{
    if (bRagdollReleaseGroundTraceInFlight || !IsValid(OwnerCharacter) || !IsValid(MeshComp))
    {
        return;
    }

    UWorld* World = OwnerCharacter->GetWorld();
    if (!World)
    {
        FinishAsyncRagdollReleaseGroundTrace(false);
        return;
    }

    MeshComp->UpdateComponentToWorld();

    FRagdollProbeLocationArray RagdollGroundProbeLocations;
    GatherRagdollProbeLocations(MeshComp, RagdollGroundProbeLocations, true, false);
    if (RagdollGroundProbeLocations.Num() == 0)
    {
        FinishAsyncRagdollReleaseGroundTrace(false);
        return;
    }

    // Deactivation is now gated by an async raycast batch sampled while ragdoll bodies
    // are still simulating. This avoids doing a blocking trace on the exact frame where
    // physics is disabled and also prevents a later capsule/water refresh from deciding
    // the final land-vs-water path.
    const uint32 RequestId = ++RagdollReleaseGroundTraceRequestId;
    bRagdollReleaseGroundTraceInFlight = true;
    bRagdollReleaseGroundTraceHitWalkable = false;
    bUseAsyncRagdollReleaseGroundResult = false;
    bAsyncRagdollReleaseGroundResult = false;
    PendingRagdollReleaseGroundTraceCount = RagdollGroundProbeLocations.Num();

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(CharacterRagdollAsyncGroundCheck), false, OwnerCharacter);
    AddCharacterSelfIgnore(QueryParams, OwnerCharacter.Get(), MeshComp.Get());

    const float BoneTraceDistance = GetRagdollGroundTraceDistance(65.0f, Radius);
    for (const FVector& RagdollLocation : RagdollGroundProbeLocations)
    {
        FTraceDelegate TraceDelegate;
        TraceDelegate.BindUObject(this, &UCharacterComponent::OnAsyncRagdollReleaseGroundTraceCompleted);

        World->AsyncLineTraceByChannel(
            EAsyncTraceType::Multi,
            GetRagdollGroundTraceStart(RagdollLocation, Radius),
            GetRagdollGroundTraceEnd(RagdollLocation, BoneTraceDistance),
            ECC_Visibility,
            QueryParams,
            FCollisionResponseParams::DefaultResponseParam,
            &TraceDelegate,
            RequestId);
    }
}

void UCharacterComponent::OnAsyncRagdollReleaseGroundTraceCompleted(const FTraceHandle& TraceHandle, FTraceDatum& TraceDatum)
{
    (void)TraceHandle;

    if (!bRagdollReleaseGroundTraceInFlight || TraceDatum.UserData != RagdollReleaseGroundTraceRequestId)
    {
        return;
    }

    for (const FHitResult& Hit : TraceDatum.OutHits)
    {
        if (!IsIgnoredWaterTraceHit(Hit) && IsRagdollWalkableGroundHit(Hit, Movement))
        {
            bRagdollReleaseGroundTraceHitWalkable = true;
            break;
        }
    }

    --PendingRagdollReleaseGroundTraceCount;
    if (bRagdollReleaseGroundTraceHitWalkable || PendingRagdollReleaseGroundTraceCount <= 0)
    {
        FinishAsyncRagdollReleaseGroundTrace(bRagdollReleaseGroundTraceHitWalkable);
    }
}

void UCharacterComponent::FinishAsyncRagdollReleaseGroundTrace(bool bWalkableGround)
{
    bRagdollReleaseGroundTraceInFlight = false;
    bRagdollReleaseGroundTraceHitWalkable = false;
    PendingRagdollReleaseGroundTraceCount = 0;

    if (!bIsRagdoll || !IsValid(OwnerCharacter) || !IsValid(MeshComp))
    {
        return;
    }

    // Store the async result for the immediately following SetRagdollActive(false) call.
    // The result is consumed once so later animation/water refreshes cannot reuse stale ground data.
    bUseAsyncRagdollReleaseGroundResult = true;
    bAsyncRagdollReleaseGroundResult = bWalkableGround;
    SetRagdollActive(false);
}

bool UCharacterComponent::ShouldDelayWaterRagdollDeactivation(float WaterLevel, bool bKnownWalkableGround) const
{
    if (!IsValid(OwnerCharacter) || !IsValid(MeshComp))
    {
        return false;
    }

    if (bKnownWalkableGround)
    {
        return false;
    }

    const FVector HipsLocation = UCharacterFunctionLibrary::GetBoneLocation(*MeshComp, BONE_HIPS);
    const FVector NeckLocation = UCharacterFunctionLibrary::GetBoneLocation(*MeshComp, BONE_NECK);
    const FVector HeadLocation = UCharacterFunctionLibrary::GetBoneLocation(*MeshComp, BONE_HEAD);
    const FVector RecoveryActorLocation = GetRagdollRecoveryActorLocationFromHips(HipsLocation, true);
    const float DesiredActorZ = WaterLevel - FMath::Max(0.0f, CharacterRagdollTuning::WaterReleaseDepthBelowSurface);
    const float DesiredUpperBodyZ = WaterLevel - FMath::Max(8.0f, CharacterRagdollTuning::WaterReleaseDepthBelowSurface * 0.35f);

    return RecoveryActorLocation.Z > DesiredActorZ
        || NeckLocation.Z > DesiredUpperBodyZ
        || HeadLocation.Z > WaterLevel - 6.0f;
}

void UCharacterComponent::BeginPendingWaterRagdollDeactivation(float WaterLevel)
{
    bPendingWaterRagdollDeactivation = true;
    PendingWaterRagdollDeactivationLevel = WaterLevel;
    bRagdollInWater = true;
    bRagdollRecoveryWantsSwimming = false;
    RagdollWeight = CharacterConstants::MaxRagdollWeight;
    RagdollActiveTime = 0.0f;
    RagdollLowSpeedTime = 0.0f;
    GetUpActiveTime = 0.0f;
    WaterRagdollRecoveryElapsed = 0.0f;
    bWaterRecoveryTransformInitialized = false;

    if (IsValid(OwnerCharacter))
    {
        OwnerCharacter->GetWorldTimerManager().ClearTimer(RagdollCheckTimerHandle);
    }
    bCheckingRagdollStay = false;

    if (IsValid(Movement))
    {
        StopMovementAndSetMode(Movement, MOVE_Swimming);
    }
}

bool UCharacterComponent::UpdatePendingWaterRagdollDeactivation(float DeltaTime, ACharacterController *InOwner, USkeletalMeshComponent *SkeletalMesh)
{
    if (!bPendingWaterRagdollDeactivation || !InOwner || !SkeletalMesh)
    {
        return false;
    }

    const FCharacterRagdollEnvironmentState PendingEnvironmentState = UpdateRagdollEnvironmentStateForRelease(PendingWaterRagdollDeactivationLevel);
    const float DetectedWaterLevel = PendingEnvironmentState.WaterLevel;
    PendingWaterRagdollDeactivationLevel = DetectedWaterLevel;

    if (!PendingEnvironmentState.bShouldRecoverInWater || PendingEnvironmentState.bTreatWaterAsGround)
    {
        ClearPendingWaterRagdollDeactivation();
        return true;
    }

    if (!PendingEnvironmentState.bShouldDelayDeactivation)
    {
        ClearPendingWaterRagdollDeactivation();
        return true;
    }

    const FVector HipsLocation = UCharacterFunctionLibrary::GetBoneLocation(*SkeletalMesh, BONE_HIPS);
    const FVector RecoveryActorLocation = GetRagdollRecoveryActorLocationFromHips(HipsLocation, true);
    const float DesiredActorZ = DetectedWaterLevel - FMath::Max(0.0f, CharacterRagdollTuning::WaterReleaseDepthBelowSurface);
    const float ErrorZ = FMath::Max(0.0f, RecoveryActorLocation.Z - DesiredActorZ);
    if (ErrorZ <= 0.0f)
    {
        return false;
    }

    const float SafeDeltaTime = FMath::Max(DeltaTime, 0.001f);
    const float DesiredDownSpeed = FMath::Clamp(ErrorZ / SafeDeltaTime, 20.0f, FMath::Max(20.0f, CharacterRagdollTuning::WaterReleaseSinkSpeed));
    const float ForceMagnitude = FMath::Clamp(ErrorZ * CharacterRagdollTuning::WaterReleaseSinkForce, 0.0f, CharacterRagdollTuning::WaterReleaseSinkForce * 1.35f);
    const FVector DownForce = -FVector::UpVector * ForceMagnitude;

    const FName SinkBones[] = {
        FName(BONE_HIPS),
        FName(BONE_NECK),
        FName(BONE_HEAD)
    };

    int32 SimulatingSinkBodies = 0;
    for (const FName BoneName : SinkBones)
    {
        FBodyInstance* BodyInstance = SkeletalMesh->GetBodyInstance(BoneName);
        if (BodyInstance && BodyInstance->IsInstanceSimulatingPhysics())
        {
            ++SimulatingSinkBodies;
        }
    }

    if (SimulatingSinkBodies <= 0)
    {
        SkeletalMesh->AddForce(DownForce, NAME_None, false);
        return false;
    }

    const FVector ForcePerBody = DownForce / static_cast<float>(SimulatingSinkBodies);
    for (const FName BoneName : SinkBones)
    {
        FBodyInstance* BodyInstance = SkeletalMesh->GetBodyInstance(BoneName);
        if (!BodyInstance || !BodyInstance->IsInstanceSimulatingPhysics())
        {
            continue;
        }

        SkeletalMesh->AddForce(ForcePerBody, BoneName, false);
        FVector BoneVelocity = SkeletalMesh->GetPhysicsLinearVelocity(BoneName);
        if (BoneVelocity.Z > -DesiredDownSpeed)
        {
            BoneVelocity.Z = FMath::FInterpTo(BoneVelocity.Z, -DesiredDownSpeed, SafeDeltaTime, 2.5f);
            SkeletalMesh->SetPhysicsLinearVelocity(BoneVelocity, false, BoneName);
        }
    }

    return false;
}

inline float UCharacterComponent::CalculateAcceleration(const float A, const float B, const float T)
{
    return (A > B) ? FMath::Max(A - T, B) : FMath::Min(A + T, B);
}

void UCharacterComponent::ApplyMoveRightForward(ACharacterController *InOwner, const FRotator &ControlRotation, const FVector &Speed)
{
    if (!InOwner)
        return;
    InOwner->AddMovementInput(MakeRightVectorFromYaw(ControlRotation.Yaw), Speed.X);
    InOwner->AddMovementInput(MakeForwardVectorFromYaw(ControlRotation.Yaw), Speed.Y);
}

inline float UCharacterComponent::ClampGroundSpeed(const float MaxSpeed, const float NormalZ, const float WalkableZ)
{
    if (NormalZ >= 1.0f)
        return MaxSpeed;
    return FMath::Clamp(MaxSpeed * NormalZ * NormalZ, MaxSpeed * WalkableZ * WalkableZ, MaxSpeed);
}

inline FVector UCharacterComponent::CalculateImpactVelocity(const FVector &CurrentVelocity)
{
    FVector Result = (CurrentVelocity - PrevVelocity);
    PrevVelocity = CurrentVelocity;
    return Result;
}

void UCharacterComponent::UpdateRagdollVelocityHistory(float DeltaTime, const FVector& CurrentVelocity)
{
    const float SafeDeltaTime = FMath::Max(0.0f, DeltaTime);

    if (IsRagdollLikeState())
    {
        return;
    }

    FVector BestVelocity = FVector::ZeroVector;
    ConsiderInitialRagdollVelocity(BestVelocity, CurrentVelocity);
    if (IsValid(Movement))
    {
        ConsiderInitialRagdollVelocity(BestVelocity, Movement->Velocity);
        ConsiderInitialRagdollVelocity(BestVelocity, Movement->GetLastUpdateVelocity());
    }

    if (ShouldPreserveCachedPreRagdollVelocity(LastPreRagdollVelocity, LastPreRagdollVelocityAge, BestVelocity))
    {
        LastPreRagdollVelocityAge += SafeDeltaTime;
        if (LastPreRagdollVelocityAge > CharacterRagdollTuning::PreRagdollVelocityGraceSeconds)
        {
            LastPreRagdollVelocity = FVector::ZeroVector;
        }
        return;
    }

    if (IsUsefulInitialRagdollVelocity(BestVelocity))
    {
        LastPreRagdollVelocity = ClampInitialRagdollVelocityForActivation(BestVelocity);
        LastPreRagdollVelocityAge = 0.0f;
        return;
    }

    LastPreRagdollVelocityAge += SafeDeltaTime;
    if (LastPreRagdollVelocityAge > CharacterRagdollTuning::PreRagdollVelocityGraceSeconds)
    {
        LastPreRagdollVelocity = FVector::ZeroVector;
    }
}

bool UCharacterComponent::IsRagdollDamage()
{
    return (ImpactVelocity.Size() > RagdollResistance) && !bInvincible;
}

FVector UCharacterComponent::CapturePreRagdollVelocity(ACharacterController *InOwner, UCharacterMovementComponent *CharacterMovement) const
{
    FVector BestVelocity = FVector::ZeroVector;

    // Pick the strongest valid gameplay velocity instead of the first non-zero one.
    // During the impact frame CharacterMovement can already be partially damped, while
    // LastPreRagdollVelocity still contains the real velocity from the previous game tick.
    if (CharacterMovement)
    {
        ConsiderInitialRagdollVelocity(BestVelocity, CharacterMovement->Velocity);
        ConsiderInitialRagdollVelocity(BestVelocity, CharacterMovement->GetLastUpdateVelocity());
    }

    if (InOwner)
    {
        ConsiderInitialRagdollVelocity(BestVelocity, InOwner->GetVelocity());
    }

    if (LastPreRagdollVelocityAge <= CharacterRagdollTuning::PreRagdollVelocityGraceSeconds)
    {
        ConsiderInitialRagdollVelocity(BestVelocity, LastPreRagdollVelocity);
    }

    // PrevVelocity is the last value used by impact detection. It is a fallback only,
    // but considering the largest valid candidate prevents a low same-frame value from
    // making the ragdoll appear to stop at activation.
    ConsiderInitialRagdollVelocity(BestVelocity, PrevVelocity);

    return ClampInitialRagdollVelocityForActivation(BestVelocity);
}

FVector UCharacterComponent::GetInitialRagdollActivationVelocity(ACharacterController *InOwner, UCharacterMovementComponent *CharacterMovement, USkeletalMeshComponent *SkeletalMesh) const
{
    (void)SkeletalMesh;
    return CapturePreRagdollVelocity(InOwner, CharacterMovement);
}

void UCharacterComponent::ApplyInitialRagdollVelocity(USkeletalMeshComponent *SkeletalMesh, const FVector &InitialVelocity) const
{
    if (!IsValid(SkeletalMesh))
    {
        return;
    }

    const FVector SafeInitialVelocity = ClampInitialRagdollVelocityForActivation(InitialVelocity);
    if (!IsUsefulInitialRagdollVelocity(SafeInitialVelocity))
    {
        return;
    }

    struct FRagdollVelocityBody
    {
        FName BoneName = NAME_None;
        FBodyInstance* BodyInstance = nullptr;
        float Mass = 1.0f;
    };

    TArray<FRagdollVelocityBody, TInlineAllocator<24>> VelocityBodies;
    float TotalMass = 0.0f;
    FVector MassWeightedVelocity = FVector::ZeroVector;

    const int32 BoneCount = SkeletalMesh->GetNumBones();
    for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
    {
        const FName BoneName = SkeletalMesh->GetBoneName(BoneIndex);
        FBodyInstance* BodyInstance = SkeletalMesh->GetBodyInstance(BoneName);
        if (!BodyInstance || !BodyInstance->IsInstanceSimulatingPhysics())
        {
            continue;
        }

        if (!ShouldApplyInitialRagdollVelocityToBone(BoneName))
        {
            continue;
        }

        // Some skeletons can resolve several bones to the same physical body.
        // Each BodyInstance participates once in the mass-weighted velocity solve.
        const bool bAlreadyAdded = VelocityBodies.ContainsByPredicate(
            [BodyInstance](const FRagdollVelocityBody& ExistingBody)
            {
                return ExistingBody.BodyInstance == BodyInstance;
            });

        if (bAlreadyAdded)
        {
            continue;
        }

        const float BodyMass = GetSafeRagdollBodyMass(BodyInstance);
        FRagdollVelocityBody VelocityBody;
        VelocityBody.BoneName = BoneName;
        VelocityBody.BodyInstance = BodyInstance;
        VelocityBody.Mass = BodyMass;
        VelocityBodies.Add(VelocityBody);
        TotalMass += BodyMass;
        MassWeightedVelocity += GetSafeRagdollBodyVelocity(BodyInstance) * BodyMass;
    }

    if (VelocityBodies.Num() == 0 || TotalMass <= UE_SMALL_NUMBER)
    {
        // Fallback for unusual physics assets that expose only the component body.
        SkeletalMesh->WakeAllRigidBodies();
        SkeletalMesh->SetPhysicsLinearVelocity(SafeInitialVelocity, false, NAME_None);
        if (FBodyInstance* RootBodyInstance = SkeletalMesh->GetBodyInstance(NAME_None))
        {
            RootBodyInstance->SetLinearVelocity(SafeInitialVelocity, false, true);
        }
        SkeletalMesh->WakeAllRigidBodies();
        return;
    }

    // Apply one common delta velocity so the mass-weighted center-of-mass velocity
    // of the driven ragdoll bodies becomes exactly the character velocity captured
    // immediately before activation. This preserves momentum without dividing the
    // velocity by body count and without injecting an impulse or angular velocity.
    const FVector CurrentCenterOfMassVelocity = MassWeightedVelocity / TotalMass;
    const FVector CommonVelocityDelta = SafeInitialVelocity - CurrentCenterOfMassVelocity;
    if (!IsUsefulInitialRagdollVelocity(CommonVelocityDelta))
    {
        return;
    }

    SkeletalMesh->WakeAllRigidBodies();

    for (const FRagdollVelocityBody& VelocityBody : VelocityBodies)
    {
        FBodyInstance* BodyInstance = VelocityBody.BodyInstance;
        if (!BodyInstance)
        {
            continue;
        }

        const FVector BodyVelocity = GetSafeRagdollBodyVelocity(BodyInstance);
        BodyInstance->SetLinearVelocity(BodyVelocity + CommonVelocityDelta, false, true);
    }

    SkeletalMesh->WakeAllRigidBodies();
}

void UCharacterComponent::BeginRagdollCameraStabilization()
{
    RagdollCameraStabilizeRemainingTime = CharacterRagdollTuning::RagdollCameraStabilizeDuration;
    // Keep the actor as a raw location-only ragdoll anchor. Camera smoothing is
    // handled by SpringArm lag, not by interpolating the actor/capsule transform.

    if (IsValid(SpringArm))
    {
        if (!bSavedRagdollCameraState)
        {
            bSavedSpringArmCameraLag = SpringArm->bEnableCameraLag;
            bSavedSpringArmCollisionTest = SpringArm->bDoCollisionTest;
            SavedSpringArmCameraLagSpeed = SpringArm->CameraLagSpeed;
            bSavedSpringArmUseCameraLagSubstepping = SpringArm->bUseCameraLagSubstepping;
            SavedSpringArmCameraLagMaxTimeStep = SpringArm->CameraLagMaxTimeStep;
            SavedSpringArmCameraLagMaxDistance = SpringArm->CameraLagMaxDistance;
            bSavedRagdollCameraState = true;
        }

        // Ragdoll does not move the camera component directly. The capsule is snapped to
        // the ragdoll location as a location-only anchor; the spring arm handles visual
        // smoothing through camera lag while keeping collision tests enabled.
        SpringArm->bEnableCameraLag = true;
        SpringArm->bDoCollisionTest = true;
        SpringArm->CameraLagSpeed = CharacterRagdollTuning::RagdollSpringArmLagSpeed;
        SpringArm->bUseCameraLagSubstepping = true;
        SpringArm->CameraLagMaxTimeStep = CharacterRagdollTuning::RagdollSpringArmLagMaxTimeStep;
        SpringArm->CameraLagMaxDistance = CharacterRagdollTuning::RagdollSpringArmMaxLagDistance;
    }
}

void UCharacterComponent::UpdateRagdollCameraStabilization(float DeltaTime)
{
    if (bIsRagdoll)
    {
        if (IsValid(SpringArm))
        {
            SpringArm->bEnableCameraLag = true;
            SpringArm->bDoCollisionTest = true;
            SpringArm->CameraLagSpeed = CharacterRagdollTuning::RagdollSpringArmLagSpeed;
            SpringArm->bUseCameraLagSubstepping = true;
            SpringArm->CameraLagMaxTimeStep = CharacterRagdollTuning::RagdollSpringArmLagMaxTimeStep;
            SpringArm->CameraLagMaxDistance = CharacterRagdollTuning::RagdollSpringArmMaxLagDistance;
        }
        return;
    }

    if (RagdollCameraStabilizeRemainingTime <= 0.0f)
    {
        return;
    }

    RagdollCameraStabilizeRemainingTime = FMath::Max(0.0f, RagdollCameraStabilizeRemainingTime - FMath::Max(0.0f, DeltaTime));
    if (RagdollCameraStabilizeRemainingTime <= 0.0f)
    {
        RestoreRagdollCameraState();
    }
}

void UCharacterComponent::RestoreRagdollCameraState()
{
    RagdollCameraStabilizeRemainingTime = 0.0f;

    if (IsValid(SpringArm) && bSavedRagdollCameraState)
    {
        SpringArm->bEnableCameraLag = bSavedSpringArmCameraLag;
        SpringArm->bDoCollisionTest = bSavedSpringArmCollisionTest;
        SpringArm->CameraLagSpeed = SavedSpringArmCameraLagSpeed;
        SpringArm->bUseCameraLagSubstepping = bSavedSpringArmUseCameraLagSubstepping;
        SpringArm->CameraLagMaxTimeStep = SavedSpringArmCameraLagMaxTimeStep;
        SpringArm->CameraLagMaxDistance = SavedSpringArmCameraLagMaxDistance;
    }

    bSavedRagdollCameraState = false;
}

void UCharacterComponent::BeginRagdollCapsuleCollisionIsolation()
{
    if (!IsValid(OwnerCharacter))
    {
        return;
    }

    UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent();
    if (!IsValid(Capsule))
    {
        return;
    }

    if (!bSavedRagdollCapsuleCollisionState)
    {
        SavedRagdollCapsuleCollisionEnabled = Capsule->GetCollisionEnabled();
        bSavedRagdollCapsuleGenerateOverlapEvents = Capsule->GetGenerateOverlapEvents();
        bSavedRagdollCapsuleCollisionState = true;
    }

    // During active ragdoll the skeletal mesh is the authoritative physics body.
    // Leaving the character capsule colliding while it is moved as a camera anchor can
    // push the ragdoll bodies, causing over-speed, hitching, and spring-arm collision pops.
    Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Capsule->SetGenerateOverlapEvents(false);
}

void UCharacterComponent::RestoreRagdollCapsuleCollision()
{
    if (!bSavedRagdollCapsuleCollisionState)
    {
        return;
    }

    if (IsValid(OwnerCharacter))
    {
        if (UCapsuleComponent* Capsule = OwnerCharacter->GetCapsuleComponent())
        {
            Capsule->SetCollisionEnabled(SavedRagdollCapsuleCollisionEnabled);
            Capsule->SetGenerateOverlapEvents(bSavedRagdollCapsuleGenerateOverlapEvents);
        }
    }

    bSavedRagdollCapsuleCollisionState = false;
}

void UCharacterComponent::ActiveRagdoll(ACharacterController *InOwner, USkeletalMeshComponent *SkeletalMesh)
{
    if (!InOwner || !SkeletalMesh)
        return;

    UCharacterMovementComponent* CharacterMovement = InOwner->GetCharacterMovement();
    const FVector InitialRagdollVelocity = GetInitialRagdollActivationVelocity(InOwner, CharacterMovement, SkeletalMesh);
    LastPreRagdollVelocity = InitialRagdollVelocity;
    LastPreRagdollVelocityAge = 0.0f;
    const bool bKeepWaterIntent = bRagdollInWater || bRagdollRecoveryWantsSwimming;
    const FRotator ActorRotationBeforeRagdoll = MakeFlatYawRotation(InOwner->GetActorRotation().Yaw);

    // If a new ragdoll starts during a land/water recovery blend, discard that recovery state.
    // The new ragdoll must start with a fresh physics pose and full ragdoll weight.
    ResetRagdollRecoveryState(bKeepWaterIntent);
    RagdollPrePhysicsActorRotation = ActorRotationBeforeRagdoll;
    bHasRagdollPrePhysicsActorRotation = true;

    bGettingUp = false;
    RagdollWeight = CharacterConstants::MaxRagdollWeight;
    bRagdollRecoveryWantsSwimming = false;
    bRagdollInWater = bKeepWaterIntent;

    if (CharacterMovement)
    {
        if (bRagdollInWater)
        {
            StopMovementAndSetMode(CharacterMovement, MOVE_Swimming);
        }
        else
        {
            CharacterMovement->DisableMovement();
        }
    }

    BeginRagdollCapsuleCollisionIsolation();

    if (UCharacterAnimInstance* CharacterAnimInst = Cast<UCharacterAnimInstance>(SkeletalMesh->GetAnimInstance()))
    {
        CharacterAnimInst->RefreshCharacterAnimationState(0.0f);
    }

    FActorHelper::DetachParent(SkeletalMesh, FDetachmentTransformRules::KeepWorldTransform);
    SkeletalMesh->SetAllBodiesSimulatePhysics(true);
    UCharacterFunctionLibrary::BlendRagdoll(*SkeletalMesh, CharacterRagdollTuning::MaxBlendWeight);
    ApplyInitialRagdollVelocity(SkeletalMesh, InitialRagdollVelocity);
    BeginRagdollCameraStabilization();
}

void UCharacterComponent::DeactiveRagdoll(ACharacterController *InOwner, USkeletalMeshComponent *SkeletalMesh, const FCharacterRagdollEnvironmentState &InReleaseEnvironmentState)
{
    if (!InOwner || !SkeletalMesh)
        return;

    UAnimInstance *AnimInst = SkeletalMesh->GetAnimInstance();
    if (!IsValid(AnimInst))
        return;

    ClearPendingWaterRagdollDeactivation();
    RestoreRagdollCameraState();

    CapturedMeshLocation = UCharacterFunctionLibrary::GetBoneLocation(*SkeletalMesh, BONE_HIPS);
    CapturedMeshRotation = UCharacterFunctionLibrary::GetBoneRotation(*SkeletalMesh, BONE_HIPS);

    AnimInst->SavePoseSnapshot(CharacterRagdollTuning::PoseSnapshotName);

    // This state was captured and applied to the controller while the ragdoll bodies were still
    // simulating. Do not re-probe after bIsRagdoll is already false; that path can read sticky
    // recovery flags and make a dry release look like a swimming release.
    FCharacterRagdollEnvironmentState ReleaseEnvironmentState = InReleaseEnvironmentState.bIsValid
        ? InReleaseEnvironmentState
        : UpdateRagdollEnvironmentStateForRelease(0.0f);

    const bool bForceLandRecovery = bForceLandRagdollRecoveryOnce || ReleaseEnvironmentState.bForcedLandRecovery || ReleaseEnvironmentState.bTreatWaterAsGround;
    if (bForceLandRecovery)
    {
        ReleaseEnvironmentState.bForcedLandRecovery = true;
        ReleaseEnvironmentState.bShouldRecoverInWater = false;
        ReleaseEnvironmentState.bShouldDelayDeactivation = false;
    }
    RagdollEnvironmentState = ReleaseEnvironmentState;

    const bool bAppliedRecoverInWater = ApplyRagdollReleaseEnvironmentStateToOwner(InOwner, ReleaseEnvironmentState);
    const bool bShouldRecoverInWater = !bForceLandRecovery && bAppliedRecoverInWater;
    bLandRagdollRecoveryOverridesWater = bForceLandRecovery;
    bForceLandRagdollRecoveryOnce = false;

    if (bShouldRecoverInWater)
    {
        bGettingUp = true;
        bLandRagdollRecoveryOverridesWater = false;
        bRagdollRecoveryWantsSwimming = true;
        bRagdollInWater = true;
        RagdollWeight = CharacterConstants::MaxRagdollWeight;
        GetUpActiveTime = 0.0f;
        WaterRagdollRecoveryElapsed = 0.0f;

        bIsLieOnBack = CheckIfLieOnBack(SkeletalMesh);
        WaterRecoveryActorStartLocation = InOwner->GetActorLocation();
        WaterRecoveryActorStartRotation = MakeFlatYawRotation(InOwner->GetActorRotation().Yaw);
        WaterRecoveryActorTargetLocation = GetRagdollRecoveryActorLocationFromHips(CapturedMeshLocation, true);
        // Underwater recovery has no grounded "get-up" facing direction.  Using the floating body yaw here
        // can flip 180 degrees when the hips/head vector rolls in water, so keep the pre-ragdoll actor yaw
        // and let the mesh pose/relative rotation blend back to the normal swimming basis.
        WaterRecoveryActorTargetRotation = bHasRagdollPrePhysicsActorRotation
            ? MakeFlatYawRotationNear(RagdollPrePhysicsActorRotation.Yaw, WaterRecoveryActorStartRotation)
            : WaterRecoveryActorStartRotation;

        // Water recovery should start from the exact current actor/mesh transform and then blend,
        // matching the land recovery feel.  Snapping the actor to the hips target here is what made
        // underwater deactivation feel like it popped before interpolation even began.

        UCharacterFunctionLibrary::DisableRagdollPhysicsButKeepSecondary(*SkeletalMesh);
        RestoreRagdollCapsuleCollision();
        if (SkeletalMesh->GetAttachParent() != InOwner->GetCapsuleComponent())
        {
            FActorHelper::AttachParent(SkeletalMesh, InOwner->GetCapsuleComponent(), FAttachmentTransformRules::KeepWorldTransform);
        }
        WaterRecoveryMeshStartRelativeLocation = SkeletalMesh->GetRelativeLocation();
        WaterRecoveryMeshStartRelativeRotation = SkeletalMesh->GetRelativeRotation();
        bWaterRecoveryTransformInitialized = true;
        SkeletalMesh->SetVisibility(true, true);

        if (UCharacterMovementComponent* CharacterMovement = InOwner->GetCharacterMovement())
        {
            StopMovementAndSetMode(CharacterMovement, MOVE_Swimming);
        }
    }
    else
    {
        ClearRagdollWaterIntent();
        WaterRagdollRecoveryElapsed = 0.0f;
        bWaterRecoveryTransformInitialized = false;

        const FVector DesiredRecoveryLocation = GetRagdollRecoveryActorLocationFromHips(CapturedMeshLocation, false);
        const FVector SafeRecoveryLocation = ResolveRagdollRecoveryGroundPenetration(DesiredRecoveryLocation);
        if (!SafeRecoveryLocation.Equals(InOwner->GetActorLocation(), KINDA_SMALL_NUMBER))
        {
            InOwner->SetActorLocation(SafeRecoveryLocation, false, nullptr, ETeleportType::TeleportPhysics);
        }

        UCharacterFunctionLibrary::DisableRagdollPhysicsButKeepSecondary(*SkeletalMesh);
        RestoreRagdollCapsuleCollision();
        FActorHelper::AttachParent(SkeletalMesh, InOwner->GetCapsuleComponent(), FAttachmentTransformRules::KeepWorldTransform);
        SetSkeletalMeshLocationAndRotation(SkeletalMesh, CharacterRagdollTuning::MeshRecoveryRelativeLocation, CharacterRagdollTuning::MeshRecoveryRelativeRotation);
        SkeletalMesh->SetVisibility(true, true);

        bGettingUp = true;

        if (UCharacterMovementComponent* CharacterMovement = InOwner->GetCharacterMovement())
        {
            StopMovementAndDisable(CharacterMovement);
        }
    }

    // Push the just-mutated component state into the AnimInstance before the graph evaluates.
    // This avoids a one-frame stale bIsRagdoll/bIsGettingUp/bIsSwimming state when ragdoll
    // deactivation happens from the game-update callback instead of the skeletal mesh tick.
    if (UCharacterAnimInstance* CharacterAnimInst = Cast<UCharacterAnimInstance>(AnimInst))
    {
        CharacterAnimInst->RefreshCharacterAnimationState(0.0f);
    }

    // UpdateAnimation will run NativeUpdateAnimation before the Blueprint graph reads the
    // variables. Calling BlueprintUpdateAnimation first can expose the previous frame's
    // stale bIsSwimming value.
    AnimInst->UpdateAnimation(0.0f, false);
    SkeletalMesh->RefreshBoneTransforms();
    SkeletalMesh->UpdateComponentToWorld();
}

void UCharacterComponent::FinalizeRagdollRecovery(ACharacterController *InOwner, USkeletalMeshComponent *SkeletalMesh)
{
    if (!InOwner || !SkeletalMesh)
        return;

    ClearPendingWaterRagdollDeactivation();

    FCharacterRagdollEnvironmentState RecoveryEnvironmentState = UpdateRagdollEnvironmentStateForRelease(RagdollEnvironmentState.WaterLevel);
    float ResumeWaterLevel = RecoveryEnvironmentState.WaterLevel;
    const bool bActorStillInsideWaterColumn = AWaterActor::FindWaterLevelAtLocationStrict(this, InOwner->GetActorLocation(), ResumeWaterLevel)
        || AWaterActor::FindWaterLevelAtLocationStrict(this, InOwner->GetBottomLocation(), ResumeWaterLevel);
    const bool bActorStillInWater = bActorStillInsideWaterColumn && ShouldUseDirectWaterState(ResumeWaterLevel, IsValid(Movement) && Movement->MovementMode == MOVE_Swimming);
    const bool bRecoveryPoseInWater = !RecoveryEnvironmentState.bForcedLandRecovery
        && !RecoveryEnvironmentState.bTreatWaterAsGround
        && (RecoveryEnvironmentState.bShouldRecoverInWater || RecoveryEnvironmentState.bRagdollMeaningfullySubmerged || bActorStillInWater);
    const bool bShouldResumeSwimming = InOwner->RefreshWaterStateForRagdollRecovery(bRecoveryPoseInWater, ResumeWaterLevel);

    if (!bShouldResumeSwimming)
    {
        ClearRagdollWaterIntent();
    }

    UCharacterFunctionLibrary::DisableRagdollPhysicsButKeepSecondary(*SkeletalMesh);
    FActorHelper::AttachParent(SkeletalMesh, InOwner->GetCapsuleComponent(), FAttachmentTransformRules::KeepWorldTransform);
    SetSkeletalMeshLocationAndRotation(SkeletalMesh, CharacterRagdollTuning::MeshRecoveryRelativeLocation, CharacterRagdollTuning::MeshRecoveryRelativeRotation);
    SkeletalMesh->SetVisibility(true, true);

    RagdollWeight = 0.0f;
    bGettingUp = false;
    bLandRagdollRecoveryOverridesWater = false;
    GetUpActiveTime = 0.0f;
    RagdollActiveTime = 0.0f;
    RagdollLowSpeedTime = 0.0f;
    WaterRagdollRecoveryElapsed = 0.0f;
    bWaterRecoveryTransformInitialized = false;
    RagdollRecoverySwimLockTime = bShouldResumeSwimming ? FMath::Max(0.0f, CharacterRagdollTuning::WaterSwimLockAfterRecovery) : 0.0f;
    bRagdollRecoveryWantsSwimming = false;
    bRagdollInWater = bShouldResumeSwimming;

    RestoreRagdollCameraState();
    RestoreRagdollCapsuleCollision();

    if (!bShouldResumeSwimming)
    {
        const FVector SafeFinalLocation = ResolveRagdollRecoveryGroundPenetration(InOwner->GetActorLocation());
        if (!SafeFinalLocation.Equals(InOwner->GetActorLocation(), KINDA_SMALL_NUMBER))
        {
            InOwner->SetActorLocation(SafeFinalLocation, false, nullptr, ETeleportType::TeleportPhysics);
        }
    }

    if (UCharacterMovementComponent* CharacterMovement = InOwner->GetCharacterMovement())
    {
        CharacterMovement->StopMovementImmediately();
        CharacterMovement->ConsumeInputVector();
        if (bShouldResumeSwimming)
        {
            CharacterMovement->SetMovementMode(MOVE_Swimming);
        }
        else
        {
            SetMovementModeAfterRagdollRecovery(CharacterMovement, RecoveryEnvironmentState);
        }
    }

    RagdollPrePhysicsActorRotation = FRotator::ZeroRotator;
    bHasRagdollPrePhysicsActorRotation = false;

    ResetMovementState();

    if (UCharacterAnimInstance* CharacterAnimInst = Cast<UCharacterAnimInstance>(SkeletalMesh->GetAnimInstance()))
    {
        CharacterAnimInst->RefreshCharacterAnimationState(0.0f);
    }
    SkeletalMesh->UpdateComponentToWorld();

    InOwner->RestoreControlAfterRagdollRecovery();
}

void UCharacterComponent::SetRagdollActive(bool bActive)
{
    if (!IsValid(OwnerCharacter) || !IsValid(MeshComp))
        return;

    if (bActive)
    {
        // Cache the last gameplay velocity before bIsRagdoll changes branch state.
        LastPreRagdollVelocity = CapturePreRagdollVelocity(OwnerCharacter, OwnerCharacter->GetCharacterMovement());
        LastPreRagdollVelocityAge = LastPreRagdollVelocity.IsNearlyZero() ? TNumericLimits<float>::Max() : 0.0f;
        bIsRagdoll = true;
        float DetectedActivationWaterLevel = 0.0f;
        const bool bActivationPoseInWater = RefreshRagdollWaterDetection(&DetectedActivationWaterLevel);
        if (!bActivationPoseInWater)
        {
            ClearRagdollWaterIntent();
        }
        ActiveRagdoll(OwnerCharacter, MeshComp);
        StartRagdollStayChecking();
    }
    else
    {
        if (!bIsRagdoll)
        {
            // A repeated deactivate request during get-up/recovery should not restart the recovery path.
            return;
        }

        // Capture the final ground state through an async raycast batch before disabling physics.
        // The callback re-enters this branch with a one-shot cached result, so the expensive
        // ground decision is never made from the already-attached capsule or stale movement mode.
        if (!bUseAsyncRagdollReleaseGroundResult)
        {
            RequestAsyncRagdollReleaseGroundTrace();
            return;
        }

        const bool bAsyncGroundResult = bAsyncRagdollReleaseGroundResult;
        bUseAsyncRagdollReleaseGroundResult = false;
        bAsyncRagdollReleaseGroundResult = false;

        // Capture and apply the final release state while physics bodies are still simulating.
        // This is the important ordering: ragdoll-position state probe -> async ground result
        // -> controller state update -> disable physics / start blend-out.
        MeshComp->UpdateComponentToWorld();
        FCharacterRagdollEnvironmentState ReleaseEnvironmentState = UpdateRagdollEnvironmentStateForRelease(0.0f, true, bAsyncGroundResult);
        const bool bHasStableReleaseSupport = ReleaseEnvironmentState.bIsOnGround || ShouldUseRagdollWaterRecoveryForState(ReleaseEnvironmentState);
        if (!bHasStableReleaseSupport)
        {
            // Neither walkable ground nor a stable water-recovery pose was confirmed.
            // Keep physics active and retry the low-speed confirmation instead of
            // starting a land get-up blend from an airborne/noisy release frame.
            ClearPendingWaterRagdollDeactivation();
            bCheckingRagdollStay = false;
            StartRagdollStayChecking();
            return;
        }

        const bool bForceLandRecovery = ReleaseEnvironmentState.bTreatWaterAsGround || ReleaseEnvironmentState.bForcedLandRecovery;
        if (bForceLandRecovery)
        {
            ReleaseEnvironmentState.bForcedLandRecovery = true;
            ReleaseEnvironmentState.bShouldRecoverInWater = false;
            ReleaseEnvironmentState.bShouldDelayDeactivation = false;
            bPendingWaterRagdollDeactivation = false;
            bForceLandRagdollRecoveryOnce = true;
        }

        const bool bReleasePoseInWater = ApplyRagdollReleaseEnvironmentStateToOwner(OwnerCharacter, ReleaseEnvironmentState);
        if (bReleasePoseInWater && ReleaseEnvironmentState.bShouldDelayDeactivation)
        {
            BeginPendingWaterRagdollDeactivation(ReleaseEnvironmentState.WaterLevel);
            return;
        }

        bIsRagdoll = false;
        DeactiveRagdoll(OwnerCharacter, MeshComp, ReleaseEnvironmentState);
    }
}

void UCharacterComponent::UpdateRagdoll(const float DeltaTime, ACharacterController *InOwner, USkeletalMeshComponent *SkeletalMesh)
{
    if (!InOwner || !SkeletalMesh)
        return;

    if (bIsRagdoll)
    {
        if (UCharacterMovementComponent* CharacterMovement = InOwner->GetCharacterMovement())
        {
            if (bRagdollInWater || bRagdollRecoveryWantsSwimming)
            {
                StopMovementAndSetMode(CharacterMovement, MOVE_Swimming);
            }
            else
            {
                CharacterMovement->DisableMovement();
            }
        }

        bIsLieOnBack = CheckIfLieOnBack(SkeletalMesh);
        const bool bUseStableWaterYaw = bHasRagdollPrePhysicsActorRotation
            && (bRagdollInWater || bRagdollRecoveryWantsSwimming || RagdollEnvironmentState.bShouldRecoverInWater || RagdollEnvironmentState.bIsInWater);
        if (bUseStableWaterYaw)
        {
            const FRotator CurrentActorYaw = MakeFlatYawRotation(InOwner->GetActorRotation().Yaw);
            const FRotator DesiredStableYaw = MakeFlatYawRotationNear(RagdollPrePhysicsActorRotation.Yaw, CurrentActorYaw);
            const float YawBlendSpeed = FMath::Max(0.0f, CharacterRagdollTuning::WaterStableYawBlendSpeed);
            ActorTargetRotation = YawBlendSpeed > KINDA_SMALL_NUMBER
                ? FMath::RInterpTo(CurrentActorYaw, DesiredStableYaw, DeltaTime, YawBlendSpeed)
                : DesiredStableYaw;
            ActorTargetRotation.Pitch = 0.0f;
            ActorTargetRotation.Roll = 0.0f;
        }
        else
        {
            ActorTargetRotation = FRotator(0.0f, GetMeshForwardYaw(bIsLieOnBack, SkeletalMesh), 0.0f);
        }
        const bool bUseWaterActorOrigin = bRagdollInWater || bRagdollRecoveryWantsSwimming || RagdollEnvironmentState.bShouldRecoverInWater;
        const FVector Location = GetRagdollRecoveryActorLocationFromHips(UCharacterFunctionLibrary::GetBoneLocation(*SkeletalMesh, BONE_HIPS), bUseWaterActorOrigin);
        // The actor is only a camera/control anchor during active ragdoll. Keep it location-only
        // and let SpringArm camera lag smooth the view; do not interpolate the actor itself and
        // do not rotate it from the simulated mesh yaw.
        if (!Location.ContainsNaN())
        {
            InOwner->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
        }
        UpdateRagdollCameraStabilization(DeltaTime);

        if (bPendingWaterRagdollDeactivation && UpdatePendingWaterRagdollDeactivation(DeltaTime, InOwner, SkeletalMesh))
        {
            SetRagdollActive(false);
            return;
        }
    }
    else if (RagdollWeight > 0.0f)
    {
        if (bRagdollRecoveryWantsSwimming || bRagdollInWater)
        {
            bGettingUp = true;
            GetUpActiveTime += DeltaTime;
            WaterRagdollRecoveryElapsed += DeltaTime;

            const float SafeBlendDuration = FMath::Max(0.1f, CharacterRagdollTuning::WaterTransformBlendDuration);
            const float RawAlpha = FMath::Clamp(WaterRagdollRecoveryElapsed / SafeBlendDuration, 0.0f, 1.0f);
            const float SmoothAlpha = RawAlpha * RawAlpha * (3.0f - 2.0f * RawAlpha);
            RagdollWeight = CharacterConstants::MaxRagdollWeight * (1.0f - SmoothAlpha);

            if (!bWaterRecoveryTransformInitialized)
            {
                bIsLieOnBack = CheckIfLieOnBack(SkeletalMesh);
                WaterRecoveryActorStartLocation = InOwner->GetActorLocation();
                WaterRecoveryActorStartRotation = MakeFlatYawRotation(InOwner->GetActorRotation().Yaw);
                WaterRecoveryActorTargetLocation = GetRagdollRecoveryActorLocationFromHips(UCharacterFunctionLibrary::GetBoneLocation(*SkeletalMesh, BONE_HIPS), true);
                WaterRecoveryActorTargetRotation = bHasRagdollPrePhysicsActorRotation
                    ? MakeFlatYawRotationNear(RagdollPrePhysicsActorRotation.Yaw, WaterRecoveryActorStartRotation)
                    : WaterRecoveryActorStartRotation;
                if (SkeletalMesh->GetAttachParent() != InOwner->GetCapsuleComponent())
                {
                    FActorHelper::AttachParent(SkeletalMesh, InOwner->GetCapsuleComponent(), FAttachmentTransformRules::KeepWorldTransform);
                }
                WaterRecoveryMeshStartRelativeLocation = SkeletalMesh->GetRelativeLocation();
                WaterRecoveryMeshStartRelativeRotation = SkeletalMesh->GetRelativeRotation();
                bWaterRecoveryTransformInitialized = true;
            }

            const FVector BlendedActorLocation = FMath::Lerp(WaterRecoveryActorStartLocation, WaterRecoveryActorTargetLocation, SmoothAlpha);
            const FQuat BlendedActorQuat = FQuat::Slerp(WaterRecoveryActorStartRotation.Quaternion(), WaterRecoveryActorTargetRotation.Quaternion(), SmoothAlpha).GetNormalized();
            InOwner->SetActorLocationAndRotation(BlendedActorLocation, BlendedActorQuat.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);

            const FVector BlendedMeshLocation = FMath::Lerp(WaterRecoveryMeshStartRelativeLocation, CharacterRagdollTuning::MeshRecoveryRelativeLocation, SmoothAlpha);
            const FQuat BlendedMeshQuat = FQuat::Slerp(WaterRecoveryMeshStartRelativeRotation.Quaternion(), CharacterRagdollTuning::MeshRecoveryRelativeRotation.Quaternion(), SmoothAlpha).GetNormalized();
            SkeletalMesh->SetRelativeLocationAndRotation(BlendedMeshLocation, BlendedMeshQuat.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);

            SkeletalMesh->SetAllBodiesPhysicsBlendWeight(FMath::Clamp(RagdollWeight / CharacterConstants::MaxRagdollWeight, 0.0f, 1.0f));
            UCharacterFunctionLibrary::KeepSecondaryPhysicsBodies(*SkeletalMesh);

            if (UCharacterMovementComponent* CharacterMovement = InOwner->GetCharacterMovement())
            {
                StopMovementAndSetMode(CharacterMovement, MOVE_Swimming);
            }

            if (RawAlpha >= 1.0f)
            {
                FinalizeRagdollRecovery(InOwner, SkeletalMesh);
            }
        }
        else
        {
            RagdollWeight = FMath::Max(0.0f, RagdollWeight - DeltaTime);
            InOwner->GetCharacterMovement()->DisableMovement();
            if (RagdollWeight == 0.0f)
            {
                bGettingUp = false;
                bLandRagdollRecoveryOverridesWater = false;
                RagdollPrePhysicsActorRotation = FRotator::ZeroRotator;
                bHasRagdollPrePhysicsActorRotation = false;
                RestoreRagdollCameraState();
                RestoreRagdollCapsuleCollision();
                const FVector SafeFinalLocation = ResolveRagdollRecoveryGroundPenetration(InOwner->GetActorLocation());
                if (!SafeFinalLocation.Equals(InOwner->GetActorLocation(), KINDA_SMALL_NUMBER))
                {
                    InOwner->SetActorLocation(SafeFinalLocation, false, nullptr, ETeleportType::TeleportPhysics);
                }
                if (UCharacterMovementComponent* CharacterMovement = InOwner->GetCharacterMovement())
                {
                    SetMovementModeAfterRagdollRecovery(CharacterMovement, RagdollEnvironmentState);
                }
                ResetMovementState();
                if (UCharacterAnimInstance* CharacterAnimInst = Cast<UCharacterAnimInstance>(SkeletalMesh->GetAnimInstance()))
                {
                    CharacterAnimInst->RefreshCharacterAnimationState(0.0f);
                }
                InOwner->RestoreControlAfterRagdollRecovery();
            }
        }
    }
}

float UCharacterComponent::GetRagdollReleaseSpeedSquared(USkeletalMeshComponent *SkeletalMesh) const
{
    if (!IsValid(SkeletalMesh))
    {
        return TNumericLimits<float>::Max();
    }

    float PrimaryMaxSpeedSquared = 0.0f;
    int32 PrimaryBodyCount = 0;
    float CoreSumSpeedSquared = 0.0f;
    int32 CoreBodyCount = 0;

    const int32 BoneCount = SkeletalMesh->GetNumBones();
    for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
    {
        const FName BoneName = SkeletalMesh->GetBoneName(BoneIndex);
        if (!IsRagdollBodySimulatingForRelease(SkeletalMesh, BoneName) || !IsRagdollCoreReleaseBone(BoneName))
        {
            continue;
        }

        const FVector BoneVelocity = SkeletalMesh->GetPhysicsLinearVelocity(BoneName);
        const float SpeedSquared = BoneVelocity.SizeSquared();
        if (!FMath::IsFinite(SpeedSquared))
        {
            continue;
        }

        if (IsRagdollPrimaryReleaseBone(BoneName))
        {
            PrimaryMaxSpeedSquared = FMath::Max(PrimaryMaxSpeedSquared, SpeedSquared);
            ++PrimaryBodyCount;
        }

        CoreSumSpeedSquared += SpeedSquared;
        ++CoreBodyCount;
    }

    // Use root/hips/pelvis first. Arms, hands, legs and feet can keep oscillating in water, but they should not block get-up.
    if (PrimaryBodyCount > 0)
    {
        return PrimaryMaxSpeedSquared;
    }

    if (CoreBodyCount > 0)
    {
        return CoreSumSpeedSquared / static_cast<float>(CoreBodyCount);
    }

    return SkeletalMesh->GetComponentVelocity().SizeSquared();
}

bool UCharacterComponent::CheckRagdollStay()
{
    if (!IsValid(MeshComp))
        return false;

    float DetectedWaterLevel = 0.0f;
    const bool bCurrentlyInWater = RefreshRagdollWaterDetection(&DetectedWaterLevel);
    if (!bCurrentlyInWater)
    {
        ClearRagdollWaterIntent(false);
        return GetRagdollReleaseSpeedSquared(MeshComp) < FMath::Square(FMath::Max(1.0f, CharacterRagdollTuning::GetUpSpeedThreshold));
    }

    bRagdollInWater = true;
    bRagdollRecoveryWantsSwimming = !bIsRagdoll && (bGettingUp || RagdollWeight > 0.0f);
    if (IsValid(Movement) && Movement->MovementMode != MOVE_Swimming)
    {
        Movement->StopMovementImmediately();
        Movement->SetMovementMode(MOVE_Swimming);
    }

    return GetRagdollReleaseSpeedSquared(MeshComp) < FMath::Square(FMath::Max(1.0f, CharacterRagdollTuning::WaterGetUpSpeedThreshold));
}

void UCharacterComponent::StartRagdollStayChecking()
{
    if (!IsValid(OwnerCharacter) || bCheckingRagdollStay)
        return;

    bCheckingRagdollStay = true;
    OwnerCharacter->GetWorldTimerManager().SetTimer(
        RagdollCheckTimerHandle,
        this,
        &UCharacterComponent::ProcessRagdollCheck,
        FMath::Max(0.05f, CharacterRagdollTuning::MinimumActiveTime),
        false);
}

void UCharacterComponent::ProcessRagdollCheck()
{
    if (!IsValid(OwnerCharacter))
    {
        bCheckingRagdollStay = false;
        return;
    }

    if (CheckRagdollStay())
    {
        const FCharacterRagdollEnvironmentState FirstReleaseState = RagdollEnvironmentState;
        // The component can be destroyed while the confirmation delay is pending (for example
        // during world travel), so bind the timer through a weak UObject-aware delegate.
        FTimerDelegate NextCheckDelegate = FTimerDelegate::CreateWeakLambda(
            this,
            [this, FirstReleaseState]()
        {
            if (CheckRagdollStay())
            {
                const bool bFirstWantedWater = ShouldUseRagdollWaterRecoveryForState(FirstReleaseState);
                const bool bSecondWantsWater = ShouldUseRagdollWaterRecoveryForState(RagdollEnvironmentState);
                if (bFirstWantedWater == bSecondWantsWater)
                {
                    bCheckingRagdollStay = false;
                    SetRagdollActive(false);
                    return;
                }
            }

            // If the release mode changed between the two low-speed checks, wait for another
            // confirmation window instead of committing a one-frame swimming/land decision.
            bCheckingRagdollStay = false;
            StartRagdollStayChecking();
        });

        const bool bUseWaterRecoveryThreshold = bRagdollInWater || bRagdollRecoveryWantsSwimming;
        const float ConfirmDelay = FMath::Max(0.05f, bUseWaterRecoveryThreshold ? CharacterRagdollTuning::WaterLowSpeedConfirmTime : CharacterRagdollTuning::LowSpeedConfirmTime);
        OwnerCharacter->GetWorldTimerManager().SetTimer(RagdollCheckTimerHandle, NextCheckDelegate, ConfirmDelay, false);
    }
    else
    {
        bCheckingRagdollStay = false;
        StartRagdollStayChecking();
    }
}

bool UCharacterComponent::CheckIfLieOnBack(const USkeletalMeshComponent *SkeletalMesh)
{
    if (!SkeletalMesh)
        return false;
    FVector LeftPos = UCharacterFunctionLibrary::GetBoneLocation(*SkeletalMesh, BONE_LEFT_UPPER_LEG);
    FVector RightPos = UCharacterFunctionLibrary::GetBoneLocation(*SkeletalMesh, BONE_RIGHT_UPPER_LEG);
    FVector HipsPos = UCharacterFunctionLibrary::GetBoneLocation(*SkeletalMesh, BONE_HIPS);

    FVector LeftRelative = (LeftPos - HipsPos).GetSafeNormal2D();
    FVector RightRelative = (RightPos - HipsPos).GetSafeNormal2D();

    FQuat Q = FQuat::FindBetweenVectors(LeftRelative, FVector(0.f, 1.f, 0.f));
    FVector T = Q.RotateVector(RightRelative);
    return T.X < 0.0f;
}

float UCharacterComponent::GetMeshForwardYaw(const bool Back, const USkeletalMeshComponent *SkeletalMesh)
{
    if (!SkeletalMesh)
        return 0.0f;
    const FVector Head = UCharacterFunctionLibrary::GetBoneLocation(*SkeletalMesh, BONE_HEAD);
    const FVector Hips = UCharacterFunctionLibrary::GetBoneLocation(*SkeletalMesh, BONE_HIPS);
    const FVector Direction = (Head - Hips).GetSafeNormal2D();

    float Result = FMath::Atan2(Direction.Y, Direction.X) * (180.0f / PI);
    return Back ? Result + 180.0f : Result;
}

void UCharacterComponent::SetSkeletalMeshLocationAndRotation(USkeletalMeshComponent *SkeletalMesh, const FVector &Location, const FRotator &Rotation, const float InvTime)
{
    if (!SkeletalMesh)
        return;
    SkeletalMesh->SetRelativeLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
}

void UCharacterComponent::SetCharacterLocationAndRotation(ACharacterController *InOwner, const FVector &Location, const FRotator &Rotation, const float InvTime)
{
    if (!InOwner)
        return;
    InOwner->SetActorLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
}
