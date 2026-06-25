// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Character/CharacterComponent.h"
#include "Character/CharacterController.h"
#include "Character/CharacterFunctionLibrary.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "System/PhysicsHelper.h"
#include "System/ActorHelper.h"
#include "System/MacroLibrary.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "TimerManager.h"
#include "PhysicsEngine/BodyInstance.h"
#include "World/WaterActor.h"
#include "CollisionQueryParams.h"
#include "CoreGlobals.h"

#define MAX_BLEND_WEIGHT 1.0f
#define RAGDOLL_CAMERA_LAG_SPEED 6.0f
#define RAGDOLL_POSE_NAME TEXT("RagdollPose")
#define MESH_POSITION FVector(0.0f, 0.0f, -90.0f)
#define MESH_ROTATION FRotator(0.0f, 270.0f, 0.0f)

using FRuntimeRagdollProbeLocationArray = TArray<FVector, TInlineAllocator<16>>;
using FRuntimeRagdollSmallProbeLocationArray = TArray<FVector, TInlineAllocator<4>>;

static FORCEINLINE FVector MakeRuntimeForwardVectorFromYaw(const float YawDegrees)
{
    float SinYaw = 0.0f;
    float CosYaw = 1.0f;
    FMath::SinCos(&SinYaw, &CosYaw, FMath::DegreesToRadians(YawDegrees));
    return FVector(CosYaw, SinYaw, 0.0f);
}

static FORCEINLINE FVector MakeRuntimeRightVectorFromYaw(const float YawDegrees)
{
    float SinYaw = 0.0f;
    float CosYaw = 1.0f;
    FMath::SinCos(&SinYaw, &CosYaw, FMath::DegreesToRadians(YawDegrees));
    return FVector(-SinYaw, CosYaw, 0.0f);
}

static FORCEINLINE void StopRuntimeMovementAndSetMode(UCharacterMovementComponent* Movement, const EMovementMode NewMode)
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

static FORCEINLINE void StopRuntimeMovementAndDisable(UCharacterMovementComponent* Movement)
{
    if (!IsValid(Movement))
    {
        return;
    }

    Movement->StopMovementImmediately();
    Movement->DisableMovement();
}

static FRotator MakeRuntimeFlatYawRotation(const float Yaw)
{
    return FRotator(0.0f, FRotator::NormalizeAxis(Yaw), 0.0f);
}

static FRotator MakeRuntimeFlatYawRotationNear(const float DesiredYaw, const FRotator& ReferenceRotation)
{
    const float ReferenceYaw = FRotator::NormalizeAxis(ReferenceRotation.Yaw);
    const float DeltaYaw = FMath::FindDeltaAngleDegrees(ReferenceYaw, DesiredYaw);
    return FRotator(0.0f, ReferenceYaw + DeltaYaw, 0.0f);
}

static float ComputeRuntimeExponentialDampingFactor(const float DampingRate, const float DeltaTime)
{
    return FMath::Exp(-FMath::Max(0.0f, DampingRate) * FMath::Max(0.0f, DeltaTime));
}

static FVector ComputeRuntimeSwimmingInputDirection(
    const FVector& MoveInput,
    const FRotator& ControlRotation,
    const bool bIsOnSurface)
{
    const FVector FlatRight = MakeRuntimeRightVectorFromYaw(ControlRotation.Yaw);
    const FVector Forward = bIsOnSurface ? MakeRuntimeForwardVectorFromYaw(ControlRotation.Yaw) : ControlRotation.Vector();
    const FVector DesiredDirection = FlatRight * MoveInput.X + Forward * MoveInput.Y + FVector::UpVector * MoveInput.Z;

    return DesiredDirection.GetSafeNormal();
}

static void ApplyRuntimeSwimmingVelocityDamping(
    UCharacterMovementComponent* Movement,
    const FVector& MoveInput,
    const FRotator& ControlRotation,
    const float DeltaTime,
    const float SubmergedAlpha,
    const bool bIsOnSurface)
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

    // Always remove some velocity while swimming so water does not feel like ice.
    const float BaseDampingRate = FMath::Lerp(1.2f, 2.5f, SafeSubmergedAlpha);
    Velocity *= ComputeRuntimeExponentialDampingFactor(BaseDampingRate, DeltaTime);

    const FVector InputDirection = ComputeRuntimeSwimmingInputDirection(MoveInput, ControlRotation, bIsOnSurface);
    if (InputAmount > 0.05f && !InputDirection.IsNearlyZero())
    {
        const float AlongSpeed = FVector::DotProduct(Velocity, InputDirection);
        const FVector AlongInputVelocity = InputDirection * FMath::Max(0.0f, AlongSpeed);
        const FVector DriftVelocity = Velocity - AlongInputVelocity;

        // Sideways/backward carry-over should disappear much faster than intentional swimming velocity.
        const float DriftDampingRate = FMath::Lerp(8.0f, 18.0f, SafeSubmergedAlpha);
        Velocity = AlongInputVelocity + DriftVelocity * ComputeRuntimeExponentialDampingFactor(DriftDampingRate, DeltaTime);
    }
    else
    {
        // Releasing input should stop the character quickly instead of letting old momentum coast.
        const float NoInputDampingRate = FMath::Lerp(1.0f, 2.2f, SafeSubmergedAlpha);
        Velocity *= ComputeRuntimeExponentialDampingFactor(NoInputDampingRate, DeltaTime);
    }

    if (bIsOnSurface && Velocity.Z > 0.0f)
    {
        const float SurfaceUpDampingRate = FMath::Lerp(1.2f, 2.8f, SafeSubmergedAlpha);
        Velocity.Z *= ComputeRuntimeExponentialDampingFactor(SurfaceUpDampingRate, DeltaTime);
    }

    if (Velocity.SizeSquared() < FMath::Square(8.0f))
    {
        Velocity = FVector::ZeroVector;
    }

    Movement->Velocity = Velocity;
}


static FString GetRuntimeCharacterNormalizedBoneName(const FName BoneName)
{
    FString BoneString = BoneName.ToString().ToLower();
    BoneString.ReplaceInline(TEXT("_"), TEXT(""));
    BoneString.ReplaceInline(TEXT("-"), TEXT(""));
    BoneString.ReplaceInline(TEXT("."), TEXT(""));
    BoneString.ReplaceInline(TEXT(" "), TEXT(""));
    BoneString.ReplaceInline(TEXT(":"), TEXT(""));
    return BoneString;
}

static bool IsRuntimeRagdollReleaseIgnoredBone(const FName BoneName)
{
    const FString BoneString = GetRuntimeCharacterNormalizedBoneName(BoneName);
    return BoneString.Contains(TEXT("hair"))
        || BoneString.Contains(TEXT("cloth"))
        || BoneString.Contains(TEXT("skirt"))
        || BoneString.Contains(TEXT("cape"))
        || BoneString.Contains(TEXT("ponytail"))
        || BoneString.Contains(TEXT("accessory"))
        || BoneString.Contains(TEXT("jiggle"))
        || BoneString.Contains(TEXT("breast"))
        || BoneString.Contains(TEXT("clavicle"))
        || BoneString.Contains(TEXT("shoulder"))
        || BoneString.Contains(TEXT("upperarm"))
        || BoneString.Contains(TEXT("lowerarm"))
        || BoneString.Contains(TEXT("forearm"))
        || BoneString.Contains(TEXT("arm"))
        || BoneString.Contains(TEXT("elbow"))
        || BoneString.Contains(TEXT("hand"))
        || BoneString.Contains(TEXT("wrist"))
        || BoneString.Contains(TEXT("palm"))
        || BoneString.Contains(TEXT("finger"))
        || BoneString.Contains(TEXT("thumb"))
        || BoneString.Contains(TEXT("index"))
        || BoneString.Contains(TEXT("middle"))
        || BoneString.Contains(TEXT("ring"))
        || BoneString.Contains(TEXT("pinky"))
        || BoneString.Contains(TEXT("upperleg"))
        || BoneString.Contains(TEXT("lowerleg"))
        || BoneString.Contains(TEXT("thigh"))
        || BoneString.Contains(TEXT("calf"))
        || BoneString.Contains(TEXT("shin"))
        || BoneString.Contains(TEXT("knee"))
        || BoneString.Contains(TEXT("ankle"))
        || BoneString.Contains(TEXT("leg"))
        || BoneString.Contains(TEXT("foot"))
        || BoneString.Contains(TEXT("toe"))
        || BoneString.Contains(TEXT("ball"))
        || BoneString.Contains(TEXT("twist"))
        || BoneString.Contains(TEXT("ik"))
        || BoneString.Contains(TEXT("weapon"));
}

static bool IsRuntimeRagdollWaterProbeBone(const FName BoneName)
{
    const FString BoneString = GetRuntimeCharacterNormalizedBoneName(BoneName);
    return !BoneString.Contains(TEXT("hair"))
        && !BoneString.Contains(TEXT("cloth"))
        && !BoneString.Contains(TEXT("skirt"))
        && !BoneString.Contains(TEXT("cape"))
        && !BoneString.Contains(TEXT("ponytail"))
        && !BoneString.Contains(TEXT("accessory"))
        && !BoneString.Contains(TEXT("jiggle"))
        && !BoneString.Contains(TEXT("breast"))
        && !BoneString.Contains(TEXT("twist"))
        && !BoneString.Contains(TEXT("ik"))
        && !BoneString.Contains(TEXT("weapon"));
}

static bool IsRuntimeRagdollPrimaryReleaseBone(const FName BoneName)
{
    const FString BoneString = GetRuntimeCharacterNormalizedBoneName(BoneName);
    return BoneString == TEXT("root")
        || BoneString.Contains(TEXT("hips"))
        || BoneString.Contains(TEXT("pelvis"));
}

static bool IsRuntimeRagdollCoreReleaseBone(const FName BoneName)
{
    if (IsRuntimeRagdollReleaseIgnoredBone(BoneName))
    {
        return false;
    }

    const FString BoneString = GetRuntimeCharacterNormalizedBoneName(BoneName);
    return IsRuntimeRagdollPrimaryReleaseBone(BoneName)
        || BoneString.Contains(TEXT("spine"))
        || BoneString.Contains(TEXT("chest"))
        || BoneString.Contains(TEXT("torso"))
        || BoneString.Contains(TEXT("abdomen"))
        || BoneString.Contains(TEXT("neck"))
        || BoneString.Contains(TEXT("head"));
}

static FORCEINLINE bool IsRuntimeRagdollPreferredProbeBone(const FName BoneName)
{
    return BoneName == FName(BONE_HIPS)
        || BoneName == FName(BONE_NECK)
        || BoneName == FName(BONE_HEAD)
        || BoneName == FName(BONE_LEFT_UPPER_LEG)
        || BoneName == FName(BONE_RIGHT_UPPER_LEG)
        || BoneName == FName(BONE_LEFT_FOOT)
        || BoneName == FName(BONE_RIGHT_FOOT);
}

static FORCEINLINE bool IsRuntimeRagdollPreferredCoreProbeBone(const FName BoneName)
{
    return BoneName == FName(BONE_HIPS)
        || BoneName == FName(BONE_NECK)
        || BoneName == FName(BONE_HEAD);
}

static bool IsRuntimeRagdollBodySimulatingForRelease(USkeletalMeshComponent* SkeletalMesh, const FName BoneName)
{
    if (!IsValid(SkeletalMesh) || BoneName == NAME_None)
    {
        return false;
    }

    const FBodyInstance* BodyInstance = SkeletalMesh->GetBodyInstance(BoneName);
    return BodyInstance != nullptr && BodyInstance->IsInstanceSimulatingPhysics();
}

static bool AddRuntimeRagdollProbeLocation(
    const USkeletalMeshComponent* SkeletalMesh,
    const FName BoneName,
    FRuntimeRagdollProbeLocationArray& OutLocations,
    const bool bRequireSimulatingBody)
{
    if (!IsValid(SkeletalMesh) || BoneName == NAME_None || SkeletalMesh->GetBoneIndex(BoneName) == INDEX_NONE)
    {
        return false;
    }

    if (bRequireSimulatingBody && !IsRuntimeRagdollBodySimulatingForRelease(const_cast<USkeletalMeshComponent*>(SkeletalMesh), BoneName))
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

static void GatherRuntimeRagdollProbeLocations(
    const USkeletalMeshComponent* SkeletalMesh,
    FRuntimeRagdollProbeLocationArray& OutLocations,
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
            if (!bCoreOnly || IsRuntimeRagdollPreferredCoreProbeBone(BoneName))
            {
                AddRuntimeRagdollProbeLocation(SkeletalMesh, BoneName, OutLocations, bRequireSimulatingBody);
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
            if (IsRuntimeRagdollPreferredProbeBone(BoneName))
            {
                continue;
            }

            if (bCoreOnly)
            {
                if (!IsRuntimeRagdollCoreReleaseBone(BoneName))
                {
                    continue;
                }
            }
            else if (!IsRuntimeRagdollWaterProbeBone(BoneName))
            {
                continue;
            }

            AddRuntimeRagdollProbeLocation(SkeletalMesh, BoneName, OutLocations, bRequireSimulatingBody);
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
static bool ProbeRuntimeRagdollWaterLevelFromLocations(const UObject* WorldContext, const TArray<FVector, AllocatorType>& ProbeLocations, float& InOutWaterLevel)
{
    bool bFoundWater = false;
    float DetectedLevel = InOutWaterLevel;

    for (const FVector& WorldLocation : ProbeLocations)
    {
        float ProbeLevel = DetectedLevel;
        if (AWaterActor::FindWaterLevelAtLocation(WorldContext, WorldLocation, ProbeLevel))
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
static void UpdateRuntimeRagdollProbeReferenceState(const TArray<FVector, AllocatorType>& ProbeLocations, FCharacterRagdollEnvironmentState& State)
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

struct FRuntimeRagdollSubmersionMetrics
{
    float MaxDepth = 0.0f;
    float AverageDepth = 0.0f;
    int32 SubmergedCount = 0;
};

template <typename AllocatorType>
static FRuntimeRagdollSubmersionMetrics ComputeRuntimeRagdollSubmersionMetrics(const TArray<FVector, AllocatorType>& ProbeLocations, const float WaterLevel)
{
    FRuntimeRagdollSubmersionMetrics Metrics;
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
        if (UCapsuleComponent *Capsule = OwnerCharacter->GetCapsuleComponent())
        {
            HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
            Radius = Capsule->GetScaledCapsuleRadius();
        }
        MeshComp = OwnerCharacter->GetMesh();
        SpringArm = OwnerCharacter->GetSpringArm();

        if (MeshComp)
        {
            WaterOffset = UCharacterFunctionLibrary::GetBoneLocation(*MeshComp, BONE_NECK).Z;
        }
    }
}

void UCharacterComponent::UpdateComponent(float DeltaTime, const FVector &MoveInput, const int32 CharacterState, const float WaterLevel)
{
    if (!IsValid(OwnerCharacter) || !IsValid(Movement) || !IsValid(MeshComp))
        return;

    const FVector CurrentLocation = OwnerCharacter->GetActorLocation();
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

    ImpactVelocity = CalculateImpactVelocity(CurrentVelocity);

    float EffectiveWaterLevel = WaterLevel;
    const bool bRagdollLikeState = IsRagdollLikeState();
    const bool bRagdollBodyDetectedInWater = bRagdollLikeState && RefreshRagdollWaterDetection(&EffectiveWaterLevel);
    const bool bEffectiveInWaterState = bRagdollLikeState ? bRagdollBodyDetectedInWater : (bInWaterState || bRagdollBodyDetectedInWater);
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

    if (bRagdollLikeState && (bEffectiveInWaterState || bActiveRagdollOrRecoverySwimLock))
    {
        bRagdollInWater = true;
        if (!bIsRagdoll && (bGettingUp || RagdollWeight > 0.0f))
        {
            bRagdollRecoveryWantsSwimming = true;
        }

        StopRuntimeMovementAndSetMode(Movement, MOVE_Swimming);

        UpdateRagdoll(DeltaTime, OwnerCharacter, MeshComp);
        return;
    }
    else if (bRagdollLikeState)
    {
        // Current frame probes say the ragdoll/recovery is dry. Clear the sticky water flags
        // before the animation instance reads them so a character that came out of water does
        // not stay in the swimming state during land get-up.
        bRagdollInWater = false;
        bRagdollRecoveryWantsSwimming = false;
        RagdollRecoverySwimLockTime = 0.0f;
        if (Movement->MovementMode == MOVE_Swimming)
        {
            StopRuntimeMovementAndDisable(Movement);
        }
    }
    else
    {
        bRagdollInWater = bEffectiveInWaterState;
        if (!bRagdollInWater)
        {
            bRagdollRecoveryWantsSwimming = false;
            RagdollRecoverySwimLockTime = 0.0f;
        }
    }

    // 1. Cache ground and base movement states once for this frame.
    FHitResult HitResult;
    const bool bIsOnGround = Movement->IsMovingOnGround();
    const bool bIsContactGround = FPhysicsHelper::Raycast(OwnerCharacter, CurrentLocation, FVector::UpVector, -100.0f, HitResult);
    const bool bIsGrounded = bIsOnGround || bIsContactGround;
    const bool bIsFalling = Movement->IsFalling();
    const bool bIsCrouch = UCharacterFunctionLibrary::IsStateActive(CharacterState, STATE_CROUCH);
    const FVector GroundNormal = HitResult.Normal;

    Movement->bOrientRotationToMovement = (MoveInput.X != 0.0f || MoveInput.Y != 0.0f) && !bIsFalling;

    const float BaseTime = DeltaTime * 5.0f;

    // 2. Flying mode.
    if (Movement->IsFlying())
    {
        RagdollResistance = 1000000.0f;
        CurrentSpeed.X = CalculateAcceleration(CurrentSpeed.X, MoveInput.X, BaseTime);
        CurrentSpeed.Y = CalculateAcceleration(CurrentSpeed.Y, MoveInput.Y, BaseTime);
        CurrentSpeed.Z = CalculateAcceleration(CurrentSpeed.Z, MoveInput.Z, BaseTime);

        Movement->MaxAcceleration = 15000.0f;
        Movement->MaxFlySpeed = UCharacterFunctionLibrary::IsStateActive(CharacterState, STATE_SPRINT) ? 15000.0f : 3000.0f;

        ApplyMoveRightForward(OwnerCharacter, ControlRot, CurrentSpeed);
        OwnerCharacter->AddMovementInput(FVector::UpVector, CurrentSpeed.Z);
    }
    // 3. Swimming and ground movement modes.
    else
    {
        RagdollResistance = 1200.0f;
        const bool bUnderSurface = (OwnerCharacter->GetBottomLocation().Z + HalfHeight + WaterOffset) < EffectiveWaterLevel;
        const bool bIsJumping = UCharacterFunctionLibrary::IsStateActive(CharacterState, STATE_JUMPING);
        const bool bNotLandBehaviour = !(bIsGrounded || bIsJumping || bIsFalling);
        const bool bCheckSwimming = bEffectiveInWaterState && (bUnderSurface || bNotLandBehaviour);
        const bool bIsSprint = UCharacterFunctionLibrary::IsStateActive(CharacterState, STATE_SPRINT);

        if (bCheckSwimming)
        {
            if (Movement->MovementMode != MOVE_Swimming)
            {
                Movement->SetMovementMode(MOVE_Swimming);
            }

            const float VelocitySize = Movement->Velocity.Size();
            // Stronger water resistance: actively kill carried-over velocity instead of only relying on CharacterMovement braking.
            const float LinearResistance = 9.25f;
            const float QuadraticResistance = 0.028f * VelocitySize * VelocitySize;
            const float Braking = FMath::Clamp(LinearResistance * VelocitySize + QuadraticResistance + FMath::Sqrt(QuadraticResistance), 180.0f, 26000.0f);

            Movement->BrakingDecelerationSwimming = Braking;
            Movement->MaxAcceleration = 1050.0f;
            Movement->MaxSwimSpeed = bIsSprint ? 260.0f : 125.0f;

            const float SwimAccelTime = BaseTime * 0.58f;
            const float SwimBrakeTime = BaseTime * 1.35f;
            CurrentSpeed.X = CalculateAcceleration(CurrentSpeed.X, MoveInput.X, FMath::IsNearlyZero(MoveInput.X, 0.05f) ? SwimBrakeTime : SwimAccelTime);
            CurrentSpeed.Y = CalculateAcceleration(CurrentSpeed.Y, MoveInput.Y, FMath::IsNearlyZero(MoveInput.Y, 0.05f) ? SwimBrakeTime : SwimAccelTime);
            CurrentSpeed.Z = CalculateAcceleration(CurrentSpeed.Z, MoveInput.Z, FMath::IsNearlyZero(MoveInput.Z, 0.05f) ? SwimBrakeTime : SwimAccelTime);

            const float DistanceToSurface = EffectiveWaterLevel - CurrentLocation.Z + WaterOffset;
            const float BrakeStartDepth = WaterOffset;
            const bool bIsOnSurface = DistanceToSurface < BrakeStartDepth && bNotLandBehaviour;

            if (bIsOnSurface && CurrentVelocity.Z > 0.0f)
            {
                const float SpeedMultiplier = FMath::Clamp(DistanceToSurface / BrakeStartDepth, 0.0f, 1.0f);
                CurrentSpeed.Z = 0.0f;
                Movement->Velocity.Z = FMath::Lerp(0.0f, CurrentVelocity.Z, SpeedMultiplier);
            }

            const float CharacterHeight = FMath::Max(1.0f, HalfHeight * 2.0f);
            const float CharacterSubmergedAlpha = FMath::Clamp(((EffectiveWaterLevel + WaterOffset) - OwnerCharacter->GetBottomLocation().Z) / CharacterHeight, 0.0f, 1.0f);
            ApplyRuntimeSwimmingVelocityDamping(Movement, MoveInput, ControlRot, DeltaTime, CharacterSubmergedAlpha, bIsOnSurface);

            if (CurrentSpeed.Y != 0.0f && bIsSprint)
            {
                const FVector Forward = bIsOnSurface ? MakeRuntimeForwardVectorFromYaw(ControlRot.Yaw) : ControlRot.Vector();
                OwnerCharacter->AddMovementInput(Forward, CurrentSpeed.Y);
                OwnerCharacter->AddMovementInput(MakeRuntimeRightVectorFromYaw(ControlRot.Yaw), CurrentSpeed.X);

                if (CurrentSpeed.Z < 0.0f || !bIsOnSurface)
                {
                    OwnerCharacter->AddMovementInput(FVector::UpVector, CurrentSpeed.Z);
                }
            }
            else
            {
                ApplyMoveRightForward(OwnerCharacter, ControlRot, CurrentSpeed);
                OwnerCharacter->AddMovementInput(FVector::UpVector, CurrentSpeed.Z);
            }
        }
        else
        {
            if (Movement->MovementMode == MOVE_Swimming)
            {
                Movement->SetMovementMode(MOVE_Walking);
            }

            CurrentSpeed.X = CalculateAcceleration(CurrentSpeed.X, MoveInput.X, BaseTime);
            CurrentSpeed.Y = CalculateAcceleration(CurrentSpeed.Y, MoveInput.Y, BaseTime);
            CurrentSpeed.Z = 0.0f;

            if (!bIsGrounded)
            {
                Movement->MaxWalkSpeed = 100.0f;
                if (!bIsJumping)
                {
                    CurrentSpeed.X = 0.0f;
                    CurrentSpeed.Y = 0.0f;
                }
            }
            else
            {
                float TargetMaxSpeed = bIsSprint ? 533.3f : 200.0f;
                Movement->MaxWalkSpeed = ClampGroundSpeed(TargetMaxSpeed, GroundNormal.Z, Movement->GetWalkableFloorZ());
            }
            ApplyMoveRightForward(OwnerCharacter, ControlRot, CurrentSpeed);
        }

        if (bIsCrouch && bIsOnGround && !bCheckSwimming)
        {
            OwnerCharacter->Crouch();
        }
        else
        {
            OwnerCharacter->UnCrouch();
        }
    }

    UpdateRagdoll(DeltaTime, OwnerCharacter, MeshComp);
}

void UCharacterComponent::ResetMovementState()
{
    ImpactVelocity = FVector::ZeroVector;
    CurrentSpeed = FVector::ZeroVector;
    PrevVelocity = FVector::ZeroVector;

    if (IsValid(Movement))
    {
        Movement->ConsumeInputVector();
        Movement->StopMovementImmediately();
    }
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

    if (!bKeepWaterIntent)
    {
        bRagdollInWater = false;
        bRagdollRecoveryWantsSwimming = false;
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
        bRagdollInWater = false;
        bRagdollRecoveryWantsSwimming = false;
        RagdollRecoverySwimLockTime = 0.0f;
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
        StopRuntimeMovementAndSetMode(Movement, MOVE_Swimming);
    }

    return true;
}

FCharacterRagdollEnvironmentState UCharacterComponent::UpdateRagdollEnvironmentStateForRelease(float InitialWaterLevel)
{
    const uint64 CurrentFrame = GFrameCounter;

    // Multiple systems ask for the same ragdoll release state in the same frame
    // (controller tick, component update, animation update). Reuse the first full probe so
    // the final release decision cannot flip just because it was sampled twice.
    if (bIsRagdoll
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

    FRuntimeRagdollProbeLocationArray RagdollProbeLocations;
    GatherRuntimeRagdollProbeLocations(MeshComp, RagdollProbeLocations, true, false);
    UpdateRuntimeRagdollProbeReferenceState(RagdollProbeLocations, State);

    FRuntimeRagdollProbeLocationArray CoreRagdollProbeLocations;
    GatherRuntimeRagdollProbeLocations(MeshComp, CoreRagdollProbeLocations, true, true);

    // The all-body list already contains the preferred core probes, so one filtered water scan is enough.
    State.bIsInWater = ProbeRuntimeRagdollWaterLevelFromLocations(this, RagdollProbeLocations, State.WaterLevel);

    if (!State.bIsInWater && State.RagdollProbeLocationCount > 0)
    {
        // Use the ragdoll pose, not the stale capsule, to emulate the normal character water check.
        FRuntimeRagdollSmallProbeLocationArray RagdollCharacterProbeLocations;
        const FVector HipsLocation = UCharacterFunctionLibrary::GetBoneLocation(*MeshComp, BONE_HIPS);
        const FVector RagdollActorLocation = GetRagdollRecoveryActorLocationFromHips(HipsLocation, false);
        RagdollCharacterProbeLocations.Add(RagdollActorLocation);
        RagdollCharacterProbeLocations.Add(RagdollActorLocation - FVector::UpVector * FMath::Max(1.0f, HalfHeight));
        RagdollCharacterProbeLocations.Add(State.RagdollReferenceLocation);
        State.bIsInWater = ProbeRuntimeRagdollWaterLevelFromLocations(this, RagdollCharacterProbeLocations, State.WaterLevel);
    }

    const FRuntimeRagdollSubmersionMetrics AllSubmersion = State.bIsInWater
        ? ComputeRuntimeRagdollSubmersionMetrics(RagdollProbeLocations, State.WaterLevel)
        : FRuntimeRagdollSubmersionMetrics();
    const FRuntimeRagdollSubmersionMetrics CoreSubmersion = State.bIsInWater
        ? ComputeRuntimeRagdollSubmersionMetrics(CoreRagdollProbeLocations, State.WaterLevel)
        : FRuntimeRagdollSubmersionMetrics();

    State.RagdollMaxSubmersionDepth = FMath::Max(AllSubmersion.MaxDepth, CoreSubmersion.MaxDepth);
    State.RagdollAverageSubmersionDepth = CoreSubmersion.SubmergedCount > 0 ? CoreSubmersion.AverageDepth : AllSubmersion.AverageDepth;
    State.RagdollSubmergedProbeCount = AllSubmersion.SubmergedCount;

    const float CoreDepthThreshold = FMath::Max(0.0f, RagdollWaterRecoveryCoreDepth);
    const float AverageDepthThreshold = FMath::Max(0.0f, RagdollWaterRecoveryAverageDepth);
    const float ReleaseHysteresisDepth = FMath::Max(0.0f, RagdollWaterReleaseHysteresisDepth);
    const int32 MinStableCoreProbeCount = FMath::Max(1, RagdollWaterReleaseMinCoreProbes);

    const bool bCoreMeaningfullySubmerged = CoreSubmersion.SubmergedCount > 0
        && (CoreSubmersion.MaxDepth >= CoreDepthThreshold
            || CoreSubmersion.AverageDepth >= AverageDepthThreshold + ReleaseHysteresisDepth
            || (CoreSubmersion.SubmergedCount >= MinStableCoreProbeCount && CoreSubmersion.AverageDepth >= FMath::Max(ReleaseHysteresisDepth, AverageDepthThreshold)));
    const int32 MinStableBodyProbeCount = FMath::Max(3, MinStableCoreProbeCount + 1);
    const bool bBodyMeaningfullySubmerged = AllSubmersion.SubmergedCount >= MinStableBodyProbeCount
        && (AllSubmersion.MaxDepth >= FMath::Max(CoreDepthThreshold, WaterRagdollReleaseDepthBelowSurface * 0.50f)
            || AllSubmersion.AverageDepth >= AverageDepthThreshold + ReleaseHysteresisDepth);
    const bool bReferenceEvidenceAllowed = CoreSubmersion.SubmergedCount > 0 || AllSubmersion.SubmergedCount >= MinStableBodyProbeCount;
    const bool bReferenceBelowSurface = bReferenceEvidenceAllowed
        && State.RagdollProbeLocationCount > 0
        && State.RagdollReferenceLocation.Z < State.WaterLevel - (CoreDepthThreshold + ReleaseHysteresisDepth);

    State.bRagdollMeaningfullySubmerged = State.bIsInWater && (bCoreMeaningfullySubmerged || bBodyMeaningfullySubmerged || bReferenceBelowSurface);
    State.bIsOnGround = IsRagdollTouchingWalkableGround();

    // Dry releases always use land recovery. This clears stale swimming/falling state that may
    // remain on the capsule from before ragdoll physics took over.
    if (!State.bIsInWater)
    {
        State.bIsOnGround = true;
        State.bTreatWaterAsGround = false;
        State.bForcedLandRecovery = true;
        State.bRagdollMeaningfullySubmerged = false;
        State.bShouldRecoverInWater = false;
        State.bShouldDelayDeactivation = false;
        return StoreAndReturn(State);
    }

    const bool bCommittedToWaterRecovery = !bForceLandRagdollRecoveryOnce
        && !bLandRagdollRecoveryOverridesWater
        && !bIsRagdoll
        && (bRagdollRecoveryWantsSwimming || (bRagdollInWater && RagdollWeight > 0.0f) || RagdollRecoverySwimLockTime > 0.0f);
    const bool bStableWaterRecoveryEvidence = State.bRagdollMeaningfullySubmerged || bCommittedToWaterRecovery;
    const bool bShallowEnoughForLandRecovery = !State.bRagdollMeaningfullySubmerged
        && State.RagdollMaxSubmersionDepth <= FMath::Max(0.0f, RagdollTreatWaterAsGroundMaxDepth);
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
    State.bShouldDelayDeactivation = State.bShouldRecoverInWater && ShouldDelayWaterRagdollDeactivation(State.WaterLevel);

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
        ActorZOffsetFromHips += WaterRagdollRecoveryActorZOffset;
    }
    return HipsLocation + FVector(0.0f, 0.0f, ActorZOffsetFromHips);
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
        bRagdollInWater = false;
        bRagdollRecoveryWantsSwimming = false;
        RagdollRecoverySwimLockTime = 0.0f;
        InOwner->RefreshWaterStateForRagdollRecovery(false, ReleaseEnvironmentState.WaterLevel);
        StopRuntimeMovementAndDisable(Movement);
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

    StopRuntimeMovementAndSetMode(Movement, MOVE_Swimming);

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
        bActuallyInWater = AWaterActor::FindWaterLevelAtLocation(this, OwnerCharacter->GetActorLocation(), DetectedWaterLevel)
            || AWaterActor::FindWaterLevelAtLocation(this, OwnerCharacter->GetBottomLocation(), DetectedWaterLevel);
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
            StopRuntimeMovementAndSetMode(Movement, MOVE_Swimming);
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

            StopRuntimeMovementAndSetMode(Movement, MOVE_Swimming);
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

    bRagdollInWater = false;
    bRagdollRecoveryWantsSwimming = false;
    RagdollRecoverySwimLockTime = 0.0f;

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

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(RuntimeCharacterRagdollGroundCheck), false, OwnerCharacter);
    QueryParams.AddIgnoredActor(OwnerCharacter);

    auto IsWalkableBelow = [&](const FVector& WorldLocation, const float ExtraTraceDistance) -> bool
    {
        const FVector Start = WorldLocation + FVector::UpVector * 12.0f;
        const FVector End = WorldLocation - FVector::UpVector * FMath::Max(1.0f, ExtraTraceDistance);
        FHitResult Hit;
        if (!World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, QueryParams))
        {
            return false;
        }

        const float WalkableZ = IsValid(Movement) ? Movement->GetWalkableFloorZ() : 0.55f;
        return Hit.bBlockingHit && Hit.ImpactNormal.Z >= FMath::Max(0.35f, WalkableZ - 0.05f);
    };

    FRuntimeRagdollProbeLocationArray RagdollGroundProbeLocations;
    GatherRuntimeRagdollProbeLocations(MeshComp, RagdollGroundProbeLocations, true, bCoreOnly);
    const float BoneTraceDistance = FMath::Max(TraceDistance, FMath::Max(45.0f, Radius * 0.75f + 24.0f));

    for (const FVector& RagdollLocation : RagdollGroundProbeLocations)
    {
        if (IsWalkableBelow(RagdollLocation, BoneTraceDistance))
        {
            return true;
        }
    }

    return false;
}

bool UCharacterComponent::ShouldDelayWaterRagdollDeactivation(float WaterLevel) const
{
    if (!IsValid(OwnerCharacter) || !IsValid(MeshComp))
    {
        return false;
    }

    if (IsRagdollTouchingWalkableGround())
    {
        return false;
    }

    const FVector HipsLocation = UCharacterFunctionLibrary::GetBoneLocation(*MeshComp, BONE_HIPS);
    const FVector NeckLocation = UCharacterFunctionLibrary::GetBoneLocation(*MeshComp, BONE_NECK);
    const FVector HeadLocation = UCharacterFunctionLibrary::GetBoneLocation(*MeshComp, BONE_HEAD);
    const FVector RecoveryActorLocation = GetRagdollRecoveryActorLocationFromHips(HipsLocation, true);
    const float DesiredActorZ = WaterLevel - FMath::Max(0.0f, WaterRagdollReleaseDepthBelowSurface);
    const float DesiredUpperBodyZ = WaterLevel - FMath::Max(8.0f, WaterRagdollReleaseDepthBelowSurface * 0.35f);

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
    RagdollWeight = MAX_RAGDOLL_WEIGHT;
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
        StopRuntimeMovementAndSetMode(Movement, MOVE_Swimming);
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
    const float DesiredActorZ = DetectedWaterLevel - FMath::Max(0.0f, WaterRagdollReleaseDepthBelowSurface);
    const float ErrorZ = FMath::Max(0.0f, RecoveryActorLocation.Z - DesiredActorZ);
    if (ErrorZ <= 0.0f)
    {
        return false;
    }

    const float SafeDeltaTime = FMath::Max(DeltaTime, 0.001f);
    const float DesiredDownSpeed = FMath::Clamp(ErrorZ / SafeDeltaTime, 20.0f, FMath::Max(20.0f, WaterRagdollReleaseSinkSpeed));
    const float ForceMagnitude = FMath::Clamp(ErrorZ * WaterRagdollReleaseSinkForce, 0.0f, WaterRagdollReleaseSinkForce * 1.35f);
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
    InOwner->AddMovementInput(MakeRuntimeRightVectorFromYaw(ControlRotation.Yaw), Speed.X);
    InOwner->AddMovementInput(MakeRuntimeForwardVectorFromYaw(ControlRotation.Yaw), Speed.Y);
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

bool UCharacterComponent::IsRagdollDamage()
{
    return (ImpactVelocity.Size() > RagdollResistance) && !bInvincible;
}

void UCharacterComponent::ActiveRagdoll(ACharacterController *InOwner, USkeletalMeshComponent *SkeletalMesh)
{
    if (!InOwner || !SkeletalMesh)
        return;

    UCharacterMovementComponent* CharacterMovement = InOwner->GetCharacterMovement();
    const bool bKeepWaterIntent = bRagdollInWater || bRagdollRecoveryWantsSwimming;
    const FRotator ActorRotationBeforeRagdoll = MakeRuntimeFlatYawRotation(InOwner->GetActorRotation().Yaw);

    // If a new ragdoll starts during a land/water recovery blend, discard that recovery state.
    // The new ragdoll must start with a fresh physics pose and full ragdoll weight.
    ResetRagdollRecoveryState(bKeepWaterIntent);
    RagdollPrePhysicsActorRotation = ActorRotationBeforeRagdoll;
    bHasRagdollPrePhysicsActorRotation = true;

    bGettingUp = false;
    RagdollWeight = MAX_RAGDOLL_WEIGHT;
    bRagdollRecoveryWantsSwimming = false;
    bRagdollInWater = bKeepWaterIntent;

    if (CharacterMovement)
    {
        if (bRagdollInWater)
        {
            StopRuntimeMovementAndSetMode(CharacterMovement, MOVE_Swimming);
        }
        else
        {
            CharacterMovement->DisableMovement();
        }
    }

    FActorHelper::DetachParent(SkeletalMesh, FDetachmentTransformRules::KeepWorldTransform);
    SkeletalMesh->SetAllBodiesSimulatePhysics(true);
    UCharacterFunctionLibrary::BlendRagdoll(*SkeletalMesh, MAX_BLEND_WEIGHT);

    if (IsValid(SpringArm))
    {
        SpringArm->bEnableCameraLag = true;
        SpringArm->CameraLagSpeed = RAGDOLL_CAMERA_LAG_SPEED;
    }
}

void UCharacterComponent::DeactiveRagdoll(ACharacterController *InOwner, USkeletalMeshComponent *SkeletalMesh, const FCharacterRagdollEnvironmentState &InReleaseEnvironmentState)
{
    if (!InOwner || !SkeletalMesh)
        return;

    UAnimInstance *AnimInst = SkeletalMesh->GetAnimInstance();
    if (!IsValid(AnimInst))
        return;

    ClearPendingWaterRagdollDeactivation();

    CapturedMeshLocation = UCharacterFunctionLibrary::GetBoneLocation(*SkeletalMesh, BONE_HIPS);
    CapturedMeshRotation = UCharacterFunctionLibrary::GetBoneRotation(*SkeletalMesh, BONE_HIPS);

    AnimInst->SavePoseSnapshot(FName(RAGDOLL_POSE_NAME));

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
        RagdollWeight = MAX_RAGDOLL_WEIGHT;
        GetUpActiveTime = 0.0f;
        WaterRagdollRecoveryElapsed = 0.0f;

        bIsLieOnBack = CheckIfLieOnBack(SkeletalMesh);
        WaterRecoveryActorStartLocation = InOwner->GetActorLocation();
        WaterRecoveryActorStartRotation = MakeRuntimeFlatYawRotation(InOwner->GetActorRotation().Yaw);
        WaterRecoveryActorTargetLocation = GetRagdollRecoveryActorLocationFromHips(CapturedMeshLocation, true);
        // Underwater recovery has no grounded "get-up" facing direction.  Using the floating body yaw here
        // can flip 180 degrees when the hips/head vector rolls in water, so keep the pre-ragdoll actor yaw
        // and let the mesh pose/relative rotation blend back to the normal swimming basis.
        WaterRecoveryActorTargetRotation = bHasRagdollPrePhysicsActorRotation
            ? MakeRuntimeFlatYawRotationNear(RagdollPrePhysicsActorRotation.Yaw, WaterRecoveryActorStartRotation)
            : WaterRecoveryActorStartRotation;

        // Water recovery should start from the exact current actor/mesh transform and then blend,
        // matching the land recovery feel.  Snapping the actor to the hips target here is what made
        // underwater deactivation feel like it popped before interpolation even began.

        UCharacterFunctionLibrary::DisableRagdollPhysicsButKeepSecondary(*SkeletalMesh);
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
            StopRuntimeMovementAndSetMode(CharacterMovement, MOVE_Swimming);
        }
    }
    else
    {
        bRagdollInWater = false;
        bRagdollRecoveryWantsSwimming = false;
        RagdollRecoverySwimLockTime = 0.0f;
        WaterRagdollRecoveryElapsed = 0.0f;
        bWaterRecoveryTransformInitialized = false;
        UCharacterFunctionLibrary::DisableRagdollPhysicsButKeepSecondary(*SkeletalMesh);
        FActorHelper::AttachParent(SkeletalMesh, InOwner->GetCapsuleComponent(), FAttachmentTransformRules::KeepWorldTransform);
        SetSkeletalMeshLocationAndRotation(SkeletalMesh, MESH_POSITION, MESH_ROTATION);
        SkeletalMesh->SetVisibility(true, true);

        bGettingUp = true;

        if (UCharacterMovementComponent* CharacterMovement = InOwner->GetCharacterMovement())
        {
            StopRuntimeMovementAndDisable(CharacterMovement);
        }
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
    const bool bActorStillInWater = AWaterActor::FindWaterLevelAtLocation(this, InOwner->GetActorLocation(), ResumeWaterLevel)
        || AWaterActor::FindWaterLevelAtLocation(this, InOwner->GetBottomLocation(), ResumeWaterLevel);
    const bool bRecoveryPoseInWater = !RecoveryEnvironmentState.bForcedLandRecovery
        && !RecoveryEnvironmentState.bTreatWaterAsGround
        && (RecoveryEnvironmentState.bShouldRecoverInWater || RecoveryEnvironmentState.bRagdollMeaningfullySubmerged || bActorStillInWater);
    const bool bShouldResumeSwimming = InOwner->RefreshWaterStateForRagdollRecovery(bRecoveryPoseInWater, ResumeWaterLevel);

    if (!bShouldResumeSwimming)
    {
        bRagdollRecoveryWantsSwimming = false;
        bRagdollInWater = false;
        RagdollRecoverySwimLockTime = 0.0f;
    }

    UCharacterFunctionLibrary::DisableRagdollPhysicsButKeepSecondary(*SkeletalMesh);
    FActorHelper::AttachParent(SkeletalMesh, InOwner->GetCapsuleComponent(), FAttachmentTransformRules::KeepWorldTransform);
    SetSkeletalMeshLocationAndRotation(SkeletalMesh, MESH_POSITION, MESH_ROTATION);
    SkeletalMesh->SetVisibility(true, true);

    RagdollWeight = 0.0f;
    bGettingUp = false;
    bLandRagdollRecoveryOverridesWater = false;
    GetUpActiveTime = 0.0f;
    RagdollActiveTime = 0.0f;
    RagdollLowSpeedTime = 0.0f;
    WaterRagdollRecoveryElapsed = 0.0f;
    bWaterRecoveryTransformInitialized = false;
    RagdollRecoverySwimLockTime = bShouldResumeSwimming ? FMath::Max(0.0f, WaterRagdollSwimLockAfterRecovery) : 0.0f;
    bRagdollRecoveryWantsSwimming = false;
    bRagdollInWater = bShouldResumeSwimming;

    if (IsValid(SpringArm))
    {
        SpringArm->bEnableCameraLag = false;
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
            const bool bUseGroundFallback = !RecoveryEnvironmentState.bIsValid
                || !RecoveryEnvironmentState.bIsInWater
                || RecoveryEnvironmentState.bForcedLandRecovery
                || RecoveryEnvironmentState.bTreatWaterAsGround;
            const bool bHasWalkableGround = bUseGroundFallback
                || CharacterMovement->IsMovingOnGround()
                || IsRagdollTouchingWalkableGround(65.0f, false);
            CharacterMovement->SetMovementMode(bHasWalkableGround ? MOVE_Walking : MOVE_Falling);
        }
    }

    RagdollPrePhysicsActorRotation = FRotator::ZeroRotator;
    bHasRagdollPrePhysicsActorRotation = false;

    ResetMovementState();
}

void UCharacterComponent::SetRagdollActive(bool bActive)
{
    if (!IsValid(OwnerCharacter) || !IsValid(MeshComp))
        return;

    if (bActive)
    {
        bIsRagdoll = true;
        float DetectedActivationWaterLevel = 0.0f;
        const bool bActivationPoseInWater = RefreshRagdollWaterDetection(&DetectedActivationWaterLevel);
        if (!bActivationPoseInWater)
        {
            bRagdollInWater = false;
            bRagdollRecoveryWantsSwimming = false;
            RagdollRecoverySwimLockTime = 0.0f;
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

        // Capture and apply the final release state while physics bodies are still simulating.
        // This is the important ordering: ragdoll-position state probe -> controller state update
        // -> disable physics / start blend-out.  Do not let the actor/capsule state decide this path.
        MeshComp->UpdateComponentToWorld();
        FCharacterRagdollEnvironmentState ReleaseEnvironmentState = UpdateRagdollEnvironmentStateForRelease(0.0f);
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
                CharacterMovement->StopMovementImmediately();
                if (CharacterMovement->MovementMode != MOVE_Swimming)
                {
                    CharacterMovement->SetMovementMode(MOVE_Swimming);
                }
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
            const FRotator CurrentActorYaw = MakeRuntimeFlatYawRotation(InOwner->GetActorRotation().Yaw);
            const FRotator DesiredStableYaw = MakeRuntimeFlatYawRotationNear(RagdollPrePhysicsActorRotation.Yaw, CurrentActorYaw);
            const float YawBlendSpeed = FMath::Max(0.0f, WaterRagdollStableYawBlendSpeed);
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
        InOwner->SetActorLocationAndRotation(Location, ActorTargetRotation, false, nullptr, ETeleportType::TeleportPhysics);

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

            const float SafeBlendDuration = FMath::Max(0.1f, WaterRagdollTransformBlendDuration);
            const float RawAlpha = FMath::Clamp(WaterRagdollRecoveryElapsed / SafeBlendDuration, 0.0f, 1.0f);
            const float SmoothAlpha = RawAlpha * RawAlpha * (3.0f - 2.0f * RawAlpha);
            RagdollWeight = MAX_RAGDOLL_WEIGHT * (1.0f - SmoothAlpha);

            if (!bWaterRecoveryTransformInitialized)
            {
                bIsLieOnBack = CheckIfLieOnBack(SkeletalMesh);
                WaterRecoveryActorStartLocation = InOwner->GetActorLocation();
                WaterRecoveryActorStartRotation = MakeRuntimeFlatYawRotation(InOwner->GetActorRotation().Yaw);
                WaterRecoveryActorTargetLocation = GetRagdollRecoveryActorLocationFromHips(UCharacterFunctionLibrary::GetBoneLocation(*SkeletalMesh, BONE_HIPS), true);
                WaterRecoveryActorTargetRotation = bHasRagdollPrePhysicsActorRotation
                    ? MakeRuntimeFlatYawRotationNear(RagdollPrePhysicsActorRotation.Yaw, WaterRecoveryActorStartRotation)
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

            const FVector BlendedMeshLocation = FMath::Lerp(WaterRecoveryMeshStartRelativeLocation, MESH_POSITION, SmoothAlpha);
            const FQuat BlendedMeshQuat = FQuat::Slerp(WaterRecoveryMeshStartRelativeRotation.Quaternion(), MESH_ROTATION.Quaternion(), SmoothAlpha).GetNormalized();
            SkeletalMesh->SetRelativeLocationAndRotation(BlendedMeshLocation, BlendedMeshQuat.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);

            SkeletalMesh->SetAllBodiesPhysicsBlendWeight(FMath::Clamp(RagdollWeight / MAX_RAGDOLL_WEIGHT, 0.0f, 1.0f));
            UCharacterFunctionLibrary::KeepSecondaryPhysicsBodies(*SkeletalMesh);

            if (UCharacterMovementComponent* CharacterMovement = InOwner->GetCharacterMovement())
            {
                CharacterMovement->StopMovementImmediately();
                if (CharacterMovement->MovementMode != MOVE_Swimming)
                {
                    CharacterMovement->SetMovementMode(MOVE_Swimming);
                }
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
                if (IsValid(SpringArm))
                {
                    SpringArm->bEnableCameraLag = false;
                }
                if (UCharacterMovementComponent* CharacterMovement = InOwner->GetCharacterMovement())
                {
                    const bool bUseGroundFallback = !RagdollEnvironmentState.bIsValid
                        || !RagdollEnvironmentState.bIsInWater
                        || RagdollEnvironmentState.bForcedLandRecovery
                        || RagdollEnvironmentState.bTreatWaterAsGround;
                    const bool bHasWalkableGround = bUseGroundFallback
                        || CharacterMovement->IsMovingOnGround()
                        || IsRagdollTouchingWalkableGround(65.0f, false);
                    CharacterMovement->SetMovementMode(bHasWalkableGround ? MOVE_Walking : MOVE_Falling);
                }
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
        if (!IsRuntimeRagdollBodySimulatingForRelease(SkeletalMesh, BoneName) || !IsRuntimeRagdollCoreReleaseBone(BoneName))
        {
            continue;
        }

        const FVector BoneVelocity = SkeletalMesh->GetPhysicsLinearVelocity(BoneName);
        const float SpeedSquared = BoneVelocity.SizeSquared();
        if (!FMath::IsFinite(SpeedSquared))
        {
            continue;
        }

        if (IsRuntimeRagdollPrimaryReleaseBone(BoneName))
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
        bRagdollInWater = false;
        bRagdollRecoveryWantsSwimming = false;
        return GetRagdollReleaseSpeedSquared(MeshComp) < FMath::Square(FMath::Max(1.0f, RagdollGetUpSpeedThreshold));
    }

    bRagdollInWater = true;
    bRagdollRecoveryWantsSwimming = !bIsRagdoll && (bGettingUp || RagdollWeight > 0.0f);
    if (IsValid(Movement) && Movement->MovementMode != MOVE_Swimming)
    {
        Movement->StopMovementImmediately();
        Movement->SetMovementMode(MOVE_Swimming);
    }

    return GetRagdollReleaseSpeedSquared(MeshComp) < FMath::Square(FMath::Max(1.0f, RagdollWaterGetUpSpeedThreshold));
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
        FMath::Max(0.05f, RagdollMinimumActiveTime),
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
        FTimerDelegate NextCheckDelegate;
        NextCheckDelegate.BindLambda([this, FirstReleaseState]()
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
        const float ConfirmDelay = FMath::Max(0.05f, bUseWaterRecoveryThreshold ? RagdollWaterLowSpeedConfirmTime : RagdollLowSpeedConfirmTime);
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
