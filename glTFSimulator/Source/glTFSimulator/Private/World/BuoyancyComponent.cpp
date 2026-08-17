// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "World/BuoyancyComponent.h"

#include "Async/ParallelFor.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PhysicsEngine/BodyInstance.h"
#include "System/GameUpdateSubSystem.h"
#include "System/MacroLibrary.h"

struct FBuoyancySampleResult
{
    FVector Force = FVector::ZeroVector;
    FVector WorldLocation = FVector::ZeroVector;
    FName BoneName = NAME_None;
    FBuoyancyPhysicsSettings PhysicsSettings;
    float SubmergedAlpha = 0.0f;
    bool bActive = false;
};

struct FSkeletalBuoyancyWorkItem
{
    FName BoneName = NAME_None;
    FBuoyancyPhysicsSettings PhysicsSettings;
};

static bool IsSkeletalBodySimulatingPhysics(const USkeletalMeshComponent* SkeletalMesh, const FName BoneName)
{
    if (!IsValid(SkeletalMesh) || BoneName == NAME_None)
    {
        return false;
    }

    // UE 5.7 no longer exposes USkeletalMeshComponent::IsBodySimulating.
    // Query the per-bone body instance directly so runtime skeletal physics still works.
    const FBodyInstance* BodyInstance = const_cast<USkeletalMeshComponent*>(SkeletalMesh)->GetBodyInstance(BoneName);
    return BodyInstance != nullptr && BodyInstance->IsInstanceSimulatingPhysics();
}

static FString GetNormalizedSkeletalBoneName(const FName BoneName)
{
    FString BoneString = BoneName.ToString().ToLower();
    BoneString.ReplaceInline(TEXT("_"), TEXT(""));
    BoneString.ReplaceInline(TEXT("-"), TEXT(""));
    BoneString.ReplaceInline(TEXT("."), TEXT(""));
    BoneString.ReplaceInline(TEXT(" "), TEXT(""));
    BoneString.ReplaceInline(TEXT(":"), TEXT(""));
    return BoneString;
}

static FString GetNormalizedSkeletalBonePattern(const FString& Pattern)
{
    FString PatternString = Pattern.ToLower();
    PatternString.ReplaceInline(TEXT("_"), TEXT(""));
    PatternString.ReplaceInline(TEXT("-"), TEXT(""));
    PatternString.ReplaceInline(TEXT("."), TEXT(""));
    PatternString.ReplaceInline(TEXT(" "), TEXT(""));
    PatternString.ReplaceInline(TEXT(":"), TEXT(""));
    return PatternString;
}

static bool IsSecondarySkeletalPhysicsBone(const USkeletalMeshComponent* SkeletalMesh, const FName BoneName)
{
    if (!IsValid(SkeletalMesh) || BoneName == NAME_None)
    {
        return false;
    }

    FName CurrentBone = BoneName;
    while (CurrentBone != NAME_None)
    {
        if (CurrentBone == FName(BONE_HAIR_ROOT) || CurrentBone == FName(BONE_DYN_ROOT))
        {
            return true;
        }
        CurrentBone = SkeletalMesh->GetParentBone(CurrentBone);
    }
    return false;
}

static const FSkeletalBuoyancyBoneRule* FindSkeletalBuoyancyBoneRule(const FSkeletalBuoyancySettings& Settings, const FName BoneName)
{
    if (BoneName == NAME_None)
    {
        return nullptr;
    }

    const FString NormalizedBoneName = GetNormalizedSkeletalBoneName(BoneName);
    for (const FSkeletalBuoyancyBoneRule& Rule : Settings.BoneRules)
    {
        if (Rule.BoneNames.Contains(BoneName))
        {
            return &Rule;
        }

        for (const FString& Pattern : Rule.BoneNameContainsPatterns)
        {
            const FString NormalizedPattern = GetNormalizedSkeletalBonePattern(Pattern);
            if (!NormalizedPattern.IsEmpty() && NormalizedBoneName.Contains(NormalizedPattern))
            {
                return &Rule;
            }
        }
    }

    return nullptr;
}

static FBuoyancyPhysicsSettings ResolveBuoyancyPhysicsSettings(
    const FBuoyancyPhysicsSettings& CommonSettings,
    const FSkeletalBuoyancyBoneRule* Rule)
{
    return (Rule && Rule->bOverridePhysicsSettings) ? Rule->PhysicsSettings : CommonSettings;
}

static bool DoesSkeletalMeshHaveSimulatingBody(const USkeletalMeshComponent* SkeletalMesh)
{
    if (!IsValid(SkeletalMesh))
    {
        return false;
    }

    const int32 BoneCount = SkeletalMesh->GetNumBones();
    for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
    {
        const FName BoneName = SkeletalMesh->GetBoneName(BoneIndex);
        if (IsSkeletalBodySimulatingPhysics(SkeletalMesh, BoneName))
        {
            return true;
        }
    }

    return false;
}

static bool IsPrimitiveReadyForForces(const UPrimitiveComponent* Primitive)
{
    if (!IsValid(Primitive))
    {
        return false;
    }

    const USkeletalMeshComponent* SkeletalMesh = Cast<USkeletalMeshComponent>(Primitive);
    if (IsValid(SkeletalMesh))
    {
        return DoesSkeletalMeshHaveSimulatingBody(SkeletalMesh);
    }

    return Primitive->IsSimulatingPhysics();
}

static float GetGravityMagnitude(const UObject* WorldContext)
{
    const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
    const float GravityZ = World ? World->GetGravityZ() : -980.0f;
    return FMath::Max(1.0f, FMath::Abs(GravityZ));
}

static float GetPrimitiveMass(const UPrimitiveComponent* Primitive)
{
    if (!IsValid(Primitive))
    {
        return 1.0f;
    }

    return FMath::Max(1.0f, Primitive->GetMass());
}

static float ComputeForceLimitPerSample(
    const float TotalMass,
    const float GravityMagnitude,
    const float BuoyancyScale,
    const float LegacyPointLimit,
    const float MaxPointLimit,
    const int32 SampleCount)
{
    const int32 SafeSampleCount = FMath::Max(1, SampleCount);
    const float NeutralForcePerSample = (FMath::Max(1.0f, TotalMass) * GravityMagnitude * FMath::Max(0.0f, BuoyancyScale)) / static_cast<float>(SafeSampleCount);
    const float SafeLegacyLimit = FMath::Max(1.0f, LegacyPointLimit);
    const float SafeMaxLimit = FMath::Max(1.0f, MaxPointLimit);
    return FMath::Max(1.0f, FMath::Min(NeutralForcePerSample, FMath::Min(SafeLegacyLimit, SafeMaxLimit)));
}

static FVector ComputeWaterDragForce(
    const FVector& Velocity,
    const float MassPerSample,
    const float SubmergedAlpha,
    const FBuoyancyPhysicsSettings& PhysicsSettings)
{
    if (SubmergedAlpha <= 0.0f || Velocity.IsNearlyZero())
    {
        return FVector::ZeroVector;
    }

    const float SafeMass = FMath::Max(1.0f, MassPerSample);
    const float Speed = Velocity.Size();
    const FVector Direction = Velocity / FMath::Max(Speed, 1.0f);

    // Do not treat the first surface-contact frame as fully submerged. The alpha power keeps
    // high-speed water entry noticeably damped, but avoids an abrupt hard stop at the surface.
    const float SafeAlpha = FMath::Clamp(SubmergedAlpha, 0.0f, 1.0f);
    const float DragSubmergedAlpha = FMath::Pow(SafeAlpha, FMath::Max(0.1f, PhysicsSettings.SurfaceEntryDragAlphaPower));
    const float ExtraDragRampStart = FMath::Clamp(PhysicsSettings.WaterDragMultiplierMinSubmergedAlpha, 0.0f, 0.95f);
    const float ExtraDragAlpha = FMath::Clamp((SafeAlpha - ExtraDragRampStart) / FMath::Max(0.05f, 1.0f - ExtraDragRampStart), 0.0f, 1.0f);
    const float SmoothExtraDragAlpha = ExtraDragAlpha * ExtraDragAlpha * (3.0f - 2.0f * ExtraDragAlpha);
    const float AllDirectionDragMultiplier = FMath::Lerp(1.0f, FMath::Max(0.0f, PhysicsSettings.WaterDragMultiplier), SmoothExtraDragAlpha);
    const float HighSpeedStart = FMath::Max(0.0f, PhysicsSettings.HighSpeedDragStartSpeed);
    const float HighSpeedFull = FMath::Max(HighSpeedStart + 1.0f, PhysicsSettings.HighSpeedDragFullSpeed);
    const float HighSpeedAlpha = FMath::Clamp((Speed - HighSpeedStart) / (HighSpeedFull - HighSpeedStart), 0.0f, 1.0f);
    const float SpeedDragMultiplier = FMath::Lerp(1.0f, FMath::Max(1.0f, PhysicsSettings.HighSpeedDragMultiplier), HighSpeedAlpha) * AllDirectionDragMultiplier;

    const float LinearScale = (FMath::Max(0.0f, PhysicsSettings.WaterLinearDragCoefficient) + FMath::Max(0.0f, PhysicsSettings.LinearWaterDamping) * 0.35f) * SpeedDragMultiplier;
    const float QuadraticScale = FMath::Max(0.0f, PhysicsSettings.WaterQuadraticDragCoefficient) * SpeedDragMultiplier;
    FVector DragForce = (-Velocity * SafeMass * LinearScale - Direction * SafeMass * Speed * Speed * QuadraticScale) * DragSubmergedAlpha;

    if (Velocity.Z < -KINDA_SMALL_NUMBER)
    {
        const float DownSpeed = -Velocity.Z;
        const float DownwardExtraScale = FMath::Max(0.0f, PhysicsSettings.DownwardWaterDragMultiplier - 1.0f);
        const float DownwardAlpha = SafeAlpha * SafeAlpha;
        const FVector DownwardDrag = FVector::UpVector
            * SafeMass
            * (DownSpeed * LinearScale + DownSpeed * DownSpeed * QuadraticScale)
            * DownwardExtraScale
            * DownwardAlpha;
        DragForce += DownwardDrag;
    }

    return DragForce.GetClampedToMaxSize(FMath::Max(1.0f, PhysicsSettings.MaxDragForcePerPoint));
}

static int32 GetBuoyancySubstepCount(const float DeltaTime, const float MaxStepSeconds)
{
    const float SafeMaxStep = FMath::Clamp(MaxStepSeconds, 0.001f, 0.1f);
    return FMath::Clamp(FMath::CeilToInt(FMath::Max(DeltaTime, SafeMaxStep) / SafeMaxStep), 1, 8);
}

static FVector ClampImpulseByVelocityChange(const FVector& Impulse, const float MassPerSample, const float MaxVelocityChange)
{
    const float MaxImpulseSize = FMath::Max(1.0f, MassPerSample) * FMath::Max(1.0f, MaxVelocityChange);
    return Impulse.GetClampedToMaxSize(MaxImpulseSize);
}

static void ApplySkeletalBoneWaterVelocityLimits(
    USkeletalMeshComponent* SkeletalMesh,
    const FName BoneName,
    const float WaterLevel,
    const float DeltaTime,
    const FBuoyancyPhysicsSettings& PhysicsSettings)
{
    if (!IsValid(SkeletalMesh) || BoneName == NAME_None || DeltaTime <= SMALL_NUMBER)
    {
        return;
    }

    const float Depth = WaterLevel - SkeletalMesh->GetBoneLocation(BoneName).Z;
    if (Depth <= 0.0f)
    {
        return;
    }

    const float SubmergedAlpha = FMath::Clamp(Depth / FMath::Max(1.0f, PhysicsSettings.SubmersionDepth), 0.0f, 1.0f);
    if (SubmergedAlpha <= 0.0f)
    {
        return;
    }

    FVector LinearVelocity = SkeletalMesh->GetPhysicsLinearVelocity(BoneName);
    bool bChanged = false;

    const float PassiveDampingRate = FMath::Max(0.0f, PhysicsSettings.LinearWaterDamping) * FMath::Lerp(0.45f, 0.95f, SubmergedAlpha);
    if (PassiveDampingRate > KINDA_SMALL_NUMBER && !LinearVelocity.IsNearlyZero())
    {
        LinearVelocity *= FMath::Exp(-PassiveDampingRate * DeltaTime);
        bChanged = true;
    }

    if (PhysicsSettings.bClampLinearVelocity)
    {
        const float MaxLinearSpeed = FMath::Max(1.0f, PhysicsSettings.MaxLinearSpeed);
        if (LinearVelocity.SizeSquared() > FMath::Square(MaxLinearSpeed))
        {
            const float ClampAlpha = FMath::Clamp((SubmergedAlpha - 0.25f) / 0.75f, 0.0f, 1.0f);
            if (ClampAlpha > KINDA_SMALL_NUMBER)
            {
                const FVector TargetVelocity = LinearVelocity.GetClampedToMaxSize(MaxLinearSpeed);
                LinearVelocity = FMath::VInterpTo(LinearVelocity, TargetVelocity, DeltaTime, 5.5f * ClampAlpha);
                bChanged = true;
            }
        }
    }

    if (PhysicsSettings.bLimitDownwardSinkSpeed && LinearVelocity.Z < -FMath::Max(1.0f, PhysicsSettings.MaxDownwardSinkSpeed))
    {
        const float MinAlpha = FMath::Clamp(PhysicsSettings.SinkSpeedClampMinSubmergedAlpha, 0.0f, 0.95f);
        const float SinkClampAlpha = FMath::Clamp((SubmergedAlpha - MinAlpha) / FMath::Max(0.05f, 1.0f - MinAlpha), 0.0f, 1.0f);
        const float SinkInterpSpeed = FMath::Max(0.0f, PhysicsSettings.SinkSpeedSoftClampInterpSpeed) * SinkClampAlpha;
        if (SinkInterpSpeed > KINDA_SMALL_NUMBER)
        {
            const float TargetZ = -FMath::Max(1.0f, PhysicsSettings.MaxDownwardSinkSpeed);
            LinearVelocity.Z = FMath::FInterpTo(LinearVelocity.Z, TargetZ, DeltaTime, SinkInterpSpeed);
            bChanged = true;
        }
    }

    if (bChanged)
    {
        SkeletalMesh->SetPhysicsLinearVelocity(LinearVelocity, false, BoneName);
    }
}

static void ApplySkeletalBoneWaterAngularVelocityLimits(
    USkeletalMeshComponent* SkeletalMesh,
    const FName BoneName,
    const float WaterLevel,
    const float DeltaTime,
    const FBuoyancyPhysicsSettings& PhysicsSettings)
{
    if (!IsValid(SkeletalMesh) || BoneName == NAME_None || DeltaTime <= SMALL_NUMBER)
    {
        return;
    }

    const float Depth = WaterLevel - SkeletalMesh->GetBoneLocation(BoneName).Z;
    if (Depth <= 0.0f)
    {
        return;
    }

    const float SubmergedAlpha = FMath::Clamp(Depth / FMath::Max(1.0f, PhysicsSettings.SubmersionDepth), 0.0f, 1.0f);
    if (SubmergedAlpha <= 0.0f)
    {
        return;
    }

    FVector AngularVelocity = SkeletalMesh->GetPhysicsAngularVelocityInRadians(BoneName);
    bool bChanged = false;

    const float AngularDampingRate = FMath::Max(0.0f, PhysicsSettings.AngularWaterDamping) * FMath::Lerp(0.55f, 1.15f, SubmergedAlpha);
    if (AngularDampingRate > KINDA_SMALL_NUMBER && !AngularVelocity.IsNearlyZero())
    {
        AngularVelocity *= FMath::Exp(-AngularDampingRate * DeltaTime);
        bChanged = true;
    }

    if (PhysicsSettings.bClampAngularVelocity)
    {
        const float MaxAngularSpeed = FMath::Max(0.1f, PhysicsSettings.MaxAngularSpeed);
        if (AngularVelocity.SizeSquared() > FMath::Square(MaxAngularSpeed))
        {
            AngularVelocity = AngularVelocity.GetClampedToMaxSize(MaxAngularSpeed);
            bChanged = true;
        }
    }

    if (bChanged)
    {
        SkeletalMesh->SetPhysicsAngularVelocityInRadians(AngularVelocity, false, BoneName);
    }
}

static void ClampPrimitiveVelocities(
    UPrimitiveComponent* Primitive,
    const FBuoyancyPhysicsSettings& PhysicsSettings,
    const float DeltaTime,
    const float SubmergedAlpha)
{
    if (!IsValid(Primitive) || !Primitive->IsSimulatingPhysics() || DeltaTime <= SMALL_NUMBER)
    {
        return;
    }

    const float SafeSubmergedAlpha = FMath::Clamp(SubmergedAlpha, 0.0f, 1.0f);
    if (SafeSubmergedAlpha <= 0.0f)
    {
        return;
    }

    FVector LinearVelocity = Primitive->GetPhysicsLinearVelocity();
    bool bChangedLinearVelocity = false;

    const float PassiveDampingRate = FMath::Max(0.0f, PhysicsSettings.LinearWaterDamping) * FMath::Lerp(0.35f, 0.85f, SafeSubmergedAlpha);
    if (PassiveDampingRate > KINDA_SMALL_NUMBER && !LinearVelocity.IsNearlyZero())
    {
        LinearVelocity *= FMath::Exp(-PassiveDampingRate * DeltaTime);
        bChangedLinearVelocity = true;
    }

    if (PhysicsSettings.bClampLinearVelocity)
    {
        const float MaxLinearSpeed = FMath::Max(1.0f, PhysicsSettings.MaxLinearSpeed);
        if (LinearVelocity.SizeSquared() > FMath::Square(MaxLinearSpeed))
        {
            LinearVelocity = FMath::VInterpTo(LinearVelocity, LinearVelocity.GetClampedToMaxSize(MaxLinearSpeed), DeltaTime, 5.5f);
            bChangedLinearVelocity = true;
        }
    }

    if (PhysicsSettings.bLimitDownwardSinkSpeed && LinearVelocity.Z < -FMath::Max(1.0f, PhysicsSettings.MaxDownwardSinkSpeed))
    {
        const float SinkInterpSpeed = FMath::Max(0.0f, PhysicsSettings.SinkSpeedSoftClampInterpSpeed);
        if (SinkInterpSpeed > KINDA_SMALL_NUMBER)
        {
            LinearVelocity.Z = FMath::FInterpTo(
                LinearVelocity.Z,
                -FMath::Max(1.0f, PhysicsSettings.MaxDownwardSinkSpeed),
                DeltaTime,
                SinkInterpSpeed);
            bChangedLinearVelocity = true;
        }
    }

    if (bChangedLinearVelocity)
    {
        Primitive->SetPhysicsLinearVelocity(LinearVelocity);
    }

    FVector AngularVelocity = Primitive->GetPhysicsAngularVelocityInRadians();
    bool bChangedAngularVelocity = false;

    const float AngularDampingRate = FMath::Max(0.0f, PhysicsSettings.AngularWaterDamping) * FMath::Lerp(0.45f, 1.05f, SafeSubmergedAlpha);
    if (AngularDampingRate > KINDA_SMALL_NUMBER && !AngularVelocity.IsNearlyZero())
    {
        AngularVelocity *= FMath::Exp(-AngularDampingRate * DeltaTime);
        bChangedAngularVelocity = true;
    }

    if (PhysicsSettings.bClampAngularVelocity)
    {
        const float MaxAngularSpeed = FMath::Max(0.1f, PhysicsSettings.MaxAngularSpeed);
        if (AngularVelocity.SizeSquared() > FMath::Square(MaxAngularSpeed))
        {
            AngularVelocity = AngularVelocity.GetClampedToMaxSize(MaxAngularSpeed);
            bChangedAngularVelocity = true;
        }
    }

    if (bChangedAngularVelocity)
    {
        Primitive->SetPhysicsAngularVelocityInRadians(AngularVelocity, false, NAME_None);
    }
}

static FSkeletalBuoyancyBoneRule MakeMinorSkeletalBodyRule()
{
    FSkeletalBuoyancyBoneRule Rule;
    Rule.RuleName = FName(TEXT("MinorSecondaryBodies"));
    Rule.BoneNameContainsPatterns = {
        TEXT("hair"),
        TEXT("cloth"),
        TEXT("skirt"),
        TEXT("cape"),
        TEXT("ponytail"),
        TEXT("accessory"),
        TEXT("jiggle"),
        TEXT("breast")
    };
    Rule.bExcludeFromPrimaryBuoyancy = true;
    Rule.bCountTowardsSimulationThreshold = false;
    Rule.bApplyPhysicsWhenExcluded = false;
    Rule.bOverridePhysicsSettings = false;
    return Rule;
}

static FSkeletalBuoyancyBoneRule MakeDistalSkeletalBodyRule()
{
    FSkeletalBuoyancyBoneRule Rule;
    Rule.RuleName = FName(TEXT("DistalLimbs"));
    Rule.BoneNameContainsPatterns = {
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
        TEXT("metacarpal"),
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
        TEXT("ball"),
        TEXT("twist"),
        TEXT("ik"),
        TEXT("weapon")
    };
    Rule.bExcludeFromPrimaryBuoyancy = true;
    Rule.bCountTowardsSimulationThreshold = true;
    Rule.bApplyPhysicsWhenExcluded = true;
    Rule.bOverridePhysicsSettings = true;
    Rule.PhysicsSettings.BuoyancyAccelerationScale = 0.990f * 0.78f;
    Rule.PhysicsSettings.WaterLinearDragCoefficient = 1.25f * 0.95f;
    Rule.PhysicsSettings.WaterQuadraticDragCoefficient = 0.00105f * 0.95f;
    Rule.PhysicsSettings.LinearWaterDamping = 2.50f;
    Rule.PhysicsSettings.AngularWaterDamping = 3.25f;
    Rule.PhysicsSettings.WaterDragMultiplier = 2.60f;
    Rule.PhysicsSettings.WaterDragMultiplierMinSubmergedAlpha = 0.03f;
    Rule.PhysicsSettings.DownwardWaterDragMultiplier = 3.10f;
    Rule.PhysicsSettings.MaxDownwardSinkSpeed = 125.0f;
    Rule.PhysicsSettings.SinkSpeedSoftClampInterpSpeed = 4.4f;
    Rule.PhysicsSettings.MaxImpulseVelocityChangePerStep = 160.0f;
    Rule.PhysicsSettings.bClampLinearVelocity = true;
    Rule.PhysicsSettings.MaxLinearSpeed = 1100.0f;
    Rule.PhysicsSettings.bClampAngularVelocity = true;
    Rule.PhysicsSettings.MaxAngularSpeed = 6.0f;
    return Rule;
}

UBuoyancyComponent::UBuoyancyComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    PrimaryComponentTick.bStartWithTickEnabled = false;

    SkeletalMeshSettings.BoneRules.Add(MakeMinorSkeletalBodyRule());
    SkeletalMeshSettings.BoneRules.Add(MakeDistalSkeletalBodyRule());
}

void UBuoyancyComponent::BeginPlay()
{
    Super::BeginPlay();
    TargetPrimitive = ResolveTargetPrimitive();
    RebuildSamplePoints();
    SetComponentTickEnabled(false);
}

void UBuoyancyComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UnregisterGameUpdate();
    Super::EndPlay(EndPlayReason);
}

void UBuoyancyComponent::RegisterGameUpdate()
{
    if (GameUpdateTickHandle != INDEX_NONE)
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [WeakThis = TWeakObjectPtr<UBuoyancyComponent>(this)](const float DeltaSeconds)
            {
                if (UBuoyancyComponent* StrongThis = WeakThis.Get())
                {
                    StrongThis->UpdateBuoyancyFromGameUpdate(DeltaSeconds);
                }
            },
            10);
    }
}

void UBuoyancyComponent::UnregisterGameUpdate()
{
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
    }
    GameUpdateTickHandle = INDEX_NONE;
}

void UBuoyancyComponent::SetTargetComponentName(FName InTargetComponentName)
{
    if (TargetComponentName == InTargetComponentName)
    {
        return;
    }

    TargetComponentName = InTargetComponentName;
    TargetPrimitive = nullptr;
    ClearAutoGeneratedSamplePoints();
}

void UBuoyancyComponent::SetCommonPhysicsSettings(const FBuoyancyPhysicsSettings& InSettings)
{
    CommonPhysicsSettings = InSettings;
}

void UBuoyancyComponent::SetSkeletalMeshSettings(const FSkeletalBuoyancySettings& InSettings)
{
    SkeletalMeshSettings = InSettings;
}

UPrimitiveComponent* UBuoyancyComponent::ResolveTargetPrimitive()
{
    AActor* Owner = GetOwner();
    if (!IsValid(Owner))
    {
        return nullptr;
    }

    TArray<UPrimitiveComponent*> Primitives;
    Owner->GetComponents<UPrimitiveComponent>(Primitives);

    if (!TargetComponentName.IsNone())
    {
        for (UPrimitiveComponent* Primitive : Primitives)
        {
            if (IsValid(Primitive) && Primitive->GetFName() == TargetComponentName)
            {
                return Primitive;
            }
        }
    }

    if (bAutoFindSimulatingPrimitive)
    {
        for (UPrimitiveComponent* Primitive : Primitives)
        {
            if (IsPrimitiveReadyForForces(Primitive))
            {
                return Primitive;
            }
        }
    }

    return Cast<UPrimitiveComponent>(Owner->GetRootComponent());
}

void UBuoyancyComponent::ClearAutoGeneratedSamplePoints()
{
    if (bAutoGeneratedSamplePoints)
    {
        LocalSamplePoints.Empty();
        bAutoGeneratedSamplePoints = false;
    }
}

void UBuoyancyComponent::RebuildSamplePoints()
{
    if (LocalSamplePoints.Num() > 0)
    {
        return;
    }

    UPrimitiveComponent* Primitive = ResolveTargetPrimitive();
    if (!IsValid(Primitive))
    {
        return;
    }

    TargetPrimitive = Primitive;

    const FBoxSphereBounds Bounds = Primitive->CalcBounds(Primitive->GetComponentTransform());
    const FVector Extent = Bounds.BoxExtent.ComponentMax(FVector(25.0f));
    const FTransform ComponentTransform = Primitive->GetComponentTransform();
    const int32 CountX = FMath::Clamp(AutoSampleCountX, 1, 4);
    const int32 CountY = FMath::Clamp(AutoSampleCountY, 1, 4);
    const int32 CountZ = FMath::Clamp(AutoSampleCountZ, 1, 2);

    for (int32 X = 0; X < CountX; ++X)
    {
        const float AlphaX = CountX == 1 ? 0.5f : static_cast<float>(X) / static_cast<float>(CountX - 1);
        for (int32 Y = 0; Y < CountY; ++Y)
        {
            const float AlphaY = CountY == 1 ? 0.5f : static_cast<float>(Y) / static_cast<float>(CountY - 1);
            for (int32 Z = 0; Z < CountZ; ++Z)
            {
                const float AlphaZ = CountZ == 1 ? 0.5f : static_cast<float>(Z) / static_cast<float>(CountZ - 1);
                const FVector WorldPoint(
                    Bounds.Origin.X + FMath::Lerp(-Extent.X, Extent.X, AlphaX),
                    Bounds.Origin.Y + FMath::Lerp(-Extent.Y, Extent.Y, AlphaY),
                    Bounds.Origin.Z + FMath::Lerp(-Extent.Z, Extent.Z, AlphaZ));
                LocalSamplePoints.Add(ComponentTransform.InverseTransformPosition(WorldPoint));
            }
        }
    }

    bAutoGeneratedSamplePoints = LocalSamplePoints.Num() > 0;
}

void UBuoyancyComponent::EnterWater(const float Level)
{
    bInWater = true;
    WaterLevel = Level;

    UPrimitiveComponent* NewPrimitive = ResolveTargetPrimitive();
    if (NewPrimitive != TargetPrimitive.Get())
    {
        ClearAutoGeneratedSamplePoints();
    }
    TargetPrimitive = NewPrimitive;
    RebuildSamplePoints();
    RegisterGameUpdate();
}

void UBuoyancyComponent::ExitWater(const float Level)
{
    WaterLevel = Level;
    bInWater = false;
    UnregisterGameUpdate();
}

bool UBuoyancyComponent::ApplySkeletalMeshBuoyancy(USkeletalMeshComponent* SkeletalMesh, float DeltaTime)
{
    if (!SkeletalMeshSettings.bApplyToSimulatingBodies || !IsValid(SkeletalMesh) || DeltaTime <= SMALL_NUMBER)
    {
        return false;
    }

    TArray<FName> SimulatedBones;
    TArray<FName> ThresholdBones;
    TArray<FSkeletalBuoyancyWorkItem> PrimaryCandidateItems;
    TArray<FSkeletalBuoyancyWorkItem> ExcludedItems;

    const int32 BoneCount = SkeletalMesh->GetNumBones();
    for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
    {
        const FName BoneName = SkeletalMesh->GetBoneName(BoneIndex);
        if (!IsSkeletalBodySimulatingPhysics(SkeletalMesh, BoneName))
        {
            continue;
        }

        if (SkeletalMeshSettings.bIgnoreSecondaryPhysicsBodies && IsSecondarySkeletalPhysicsBone(SkeletalMesh, BoneName))
        {
            continue;
        }

        const FSkeletalBuoyancyBoneRule* Rule = FindSkeletalBuoyancyBoneRule(SkeletalMeshSettings, BoneName);
        const FBuoyancyPhysicsSettings BonePhysicsSettings = ResolveBuoyancyPhysicsSettings(CommonPhysicsSettings, Rule);
        const bool bExcludedFromPrimary = Rule && Rule->bExcludeFromPrimaryBuoyancy;
        const bool bCountsForThreshold = !Rule || Rule->bCountTowardsSimulationThreshold;

        SimulatedBones.Add(BoneName);
        if (bCountsForThreshold)
        {
            ThresholdBones.Add(BoneName);
        }

        if (bExcludedFromPrimary)
        {
            if (Rule->bApplyPhysicsWhenExcluded)
            {
                FSkeletalBuoyancyWorkItem ExcludedItem;
                ExcludedItem.BoneName = BoneName;
                ExcludedItem.PhysicsSettings = BonePhysicsSettings;
                ExcludedItems.Add(ExcludedItem);
            }
            continue;
        }

        FSkeletalBuoyancyWorkItem CandidateItem;
        CandidateItem.BoneName = BoneName;
        CandidateItem.PhysicsSettings = BonePhysicsSettings;
        PrimaryCandidateItems.Add(CandidateItem);
    }

    const int32 MinBodies = FMath::Clamp(SkeletalMeshSettings.MinSimulatedBodiesForBuoyancy, 1, 16);
    if (ThresholdBones.Num() < MinBodies)
    {
        return false;
    }

    const int32 MaxSamples = FMath::Clamp(SkeletalMeshSettings.MaxBodySamples, 1, 32);
    TArray<FSkeletalBuoyancyWorkItem> SampleItems;
    if (PrimaryCandidateItems.Num() <= MaxSamples)
    {
        SampleItems = PrimaryCandidateItems;
    }
    else
    {
        SampleItems.Reserve(MaxSamples);
        for (int32 SampleIndex = 0; SampleIndex < MaxSamples; ++SampleIndex)
        {
            const float Alpha = MaxSamples <= 1 ? 0.0f : static_cast<float>(SampleIndex) / static_cast<float>(MaxSamples - 1);
            const int32 CandidateIndex = FMath::Clamp(FMath::RoundToInt(Alpha * static_cast<float>(PrimaryCandidateItems.Num() - 1)), 0, PrimaryCandidateItems.Num() - 1);
            SampleItems.Add(PrimaryCandidateItems[CandidateIndex]);
        }
    }

    if (SampleItems.Num() == 0 && ExcludedItems.Num() == 0)
    {
        return false;
    }

    const float CurrentWaterLevel = WaterLevel;
    const float GravityMagnitude = GetGravityMagnitude(this);
    const float TotalMass = FMath::Max(GetPrimitiveMass(SkeletalMesh), FMath::Max(1.0f, SkeletalMeshSettings.TotalMassFloor));
    const int32 PrimarySampleCount = FMath::Max(1, SampleItems.Num());
    const float MassPerPrimarySample = TotalMass / static_cast<float>(PrimarySampleCount);
    const FVector UpVector = FVector::UpVector;
    const int32 SubstepCount = GetBuoyancySubstepCount(DeltaTime, CommonPhysicsSettings.MaxBuoyancyStepSeconds);
    const float StepDeltaTime = DeltaTime / static_cast<float>(SubstepCount);
    bool bAppliedAnyImpulse = false;

    if (SampleItems.Num() > 0)
    {
        TArray<FVector> WorldPoints;
        TArray<FVector> PointVelocities;
        TArray<FBuoyancyPhysicsSettings> PointPhysicsSettings;
        WorldPoints.SetNum(SampleItems.Num());
        PointVelocities.SetNum(SampleItems.Num());
        PointPhysicsSettings.SetNum(SampleItems.Num());

        for (int32 Index = 0; Index < SampleItems.Num(); ++Index)
        {
            const FName BoneName = SampleItems[Index].BoneName;
            WorldPoints[Index] = SkeletalMesh->GetBoneLocation(BoneName);
            PointVelocities[Index] = SkeletalMesh->GetPhysicsLinearVelocity(BoneName);
            PointPhysicsSettings[Index] = SampleItems[Index].PhysicsSettings;
        }

        TArray<FBuoyancySampleResult> Results;
        Results.SetNum(SampleItems.Num());

        const auto CalculateSkeletalSample = [CurrentWaterLevel, UpVector, GravityMagnitude, TotalMass, PrimarySampleCount, MassPerPrimarySample, &SampleItems, &WorldPoints, &PointVelocities, &PointPhysicsSettings, &Results](int32 Index)
        {
            const FVector& WorldPoint = WorldPoints[Index];
            const FBuoyancyPhysicsSettings& PhysicsSettings = PointPhysicsSettings[Index];
            const float SafeSubmersionDepth = FMath::Max(1.0f, PhysicsSettings.SubmersionDepth);
            const float Depth = CurrentWaterLevel - WorldPoint.Z;
            if (Depth <= 0.0f)
            {
                return;
            }

            const float SubmergedAlpha = FMath::Clamp(Depth / SafeSubmersionDepth, 0.0f, 1.0f);
            const float ForceLimitPerSample = ComputeForceLimitPerSample(
                TotalMass,
                GravityMagnitude,
                PhysicsSettings.BuoyancyAccelerationScale,
                PhysicsSettings.BuoyancyForcePerPoint,
                PhysicsSettings.MaxForcePerPoint,
                PrimarySampleCount);
            const float DragLimitPerSample = FMath::Max(1.0f, PhysicsSettings.MaxDragForcePerPoint);
            const FVector BuoyancyForce = UpVector * ForceLimitPerSample * SubmergedAlpha;
            const FVector DragForce = ComputeWaterDragForce(
                PointVelocities[Index],
                MassPerPrimarySample,
                SubmergedAlpha,
                PhysicsSettings);
            const FVector LinearDampingForce = -PointVelocities[Index]
                * FMath::Max(0.0f, PhysicsSettings.LinearWaterDamping)
                * MassPerPrimarySample
                * SubmergedAlpha;

            Results[Index].BoneName = SampleItems[Index].BoneName;
            Results[Index].WorldLocation = WorldPoint;
            Results[Index].PhysicsSettings = PhysicsSettings;
            Results[Index].SubmergedAlpha = SubmergedAlpha;
            Results[Index].Force = (BuoyancyForce + DragForce + LinearDampingForce).GetClampedToMaxSize(ForceLimitPerSample + DragLimitPerSample);
            Results[Index].bActive = true;
        };

        // Skeletal sampling is capped at 32 entries, below the point where task dispatch wins.
        for (int32 Index = 0; Index < SampleItems.Num(); ++Index)
        {
            CalculateSkeletalSample(Index);
        }

        for (int32 SubstepIndex = 0; SubstepIndex < SubstepCount; ++SubstepIndex)
        {
            for (const FBuoyancySampleResult& Result : Results)
            {
                if (!Result.bActive || !IsSkeletalBodySimulatingPhysics(SkeletalMesh, Result.BoneName))
                {
                    continue;
                }

                const FVector Impulse = ClampImpulseByVelocityChange(Result.Force * StepDeltaTime, MassPerPrimarySample, Result.PhysicsSettings.MaxImpulseVelocityChangePerStep);
                if (!Impulse.IsNearlyZero())
                {
                    // Apply on the body center of mass. AddImpulseAtLocation can introduce unwanted water torque.
                    SkeletalMesh->AddImpulse(Impulse, Result.BoneName, false);
                    bAppliedAnyImpulse = true;
                }
            }
        }

        for (int32 SubstepIndex = 0; SubstepIndex < SubstepCount; ++SubstepIndex)
        {
            for (const FSkeletalBuoyancyWorkItem& Item : SampleItems)
            {
                if (!IsSkeletalBodySimulatingPhysics(SkeletalMesh, Item.BoneName) || Item.PhysicsSettings.AngularWaterDamping <= 0.0f)
                {
                    continue;
                }

                const FVector AngularVelocity = SkeletalMesh->GetPhysicsAngularVelocityInRadians(Item.BoneName);
                const float AngularDampingScale = FMath::Max(0.0f, Item.PhysicsSettings.AngularWaterDamping) * MassPerPrimarySample;
                const FVector AngularImpulse = (-AngularVelocity * AngularDampingScale * StepDeltaTime).GetClampedToMaxSize(MassPerPrimarySample * FMath::Max(1.0f, Item.PhysicsSettings.MaxImpulseVelocityChangePerStep));
                if (!AngularImpulse.IsNearlyZero())
                {
                    SkeletalMesh->AddAngularImpulseInRadians(AngularImpulse, Item.BoneName, false);
                }
            }
        }
    }

    if (ExcludedItems.Num() > 0)
    {
        const int32 ExcludedSampleCount = FMath::Max(1, SimulatedBones.Num());
        const float MassPerExcludedSample = TotalMass / static_cast<float>(ExcludedSampleCount);

        for (int32 SubstepIndex = 0; SubstepIndex < SubstepCount; ++SubstepIndex)
        {
            for (const FSkeletalBuoyancyWorkItem& Item : ExcludedItems)
            {
                if (!IsSkeletalBodySimulatingPhysics(SkeletalMesh, Item.BoneName))
                {
                    continue;
                }

                const FBuoyancyPhysicsSettings& PhysicsSettings = Item.PhysicsSettings;
                const float SafeSubmersionDepth = FMath::Max(1.0f, PhysicsSettings.SubmersionDepth);
                const float Depth = CurrentWaterLevel - SkeletalMesh->GetBoneLocation(Item.BoneName).Z;
                if (Depth <= 0.0f)
                {
                    continue;
                }

                const float SubmergedAlpha = FMath::Clamp(Depth / SafeSubmersionDepth, 0.0f, 1.0f);
                const float ForceLimitPerSample = ComputeForceLimitPerSample(
                    TotalMass,
                    GravityMagnitude,
                    PhysicsSettings.BuoyancyAccelerationScale,
                    PhysicsSettings.BuoyancyForcePerPoint,
                    PhysicsSettings.MaxForcePerPoint,
                    ExcludedSampleCount);
                const float DragLimitPerSample = FMath::Max(1.0f, PhysicsSettings.MaxDragForcePerPoint);
                const FVector LinearVelocity = SkeletalMesh->GetPhysicsLinearVelocity(Item.BoneName);
                const FVector BuoyancyForce = FVector::UpVector * ForceLimitPerSample * SubmergedAlpha;
                const FVector DragForce = ComputeWaterDragForce(
                    LinearVelocity,
                    MassPerExcludedSample,
                    SubmergedAlpha,
                    PhysicsSettings);
                const FVector LinearDampingForce = -LinearVelocity * FMath::Max(0.0f, PhysicsSettings.LinearWaterDamping) * MassPerExcludedSample * SubmergedAlpha;
                const FVector LinearImpulse = ClampImpulseByVelocityChange(
                    (BuoyancyForce + DragForce + LinearDampingForce) * StepDeltaTime,
                    MassPerExcludedSample,
                    PhysicsSettings.MaxImpulseVelocityChangePerStep);
                if (!LinearImpulse.IsNearlyZero())
                {
                    SkeletalMesh->AddImpulse(LinearImpulse, Item.BoneName, false);
                    bAppliedAnyImpulse = true;
                }

                const FVector AngularVelocity = SkeletalMesh->GetPhysicsAngularVelocityInRadians(Item.BoneName);
                const FVector AngularImpulse = (-AngularVelocity * FMath::Max(0.0f, PhysicsSettings.AngularWaterDamping) * MassPerExcludedSample * SubmergedAlpha * StepDeltaTime)
                    .GetClampedToMaxSize(MassPerExcludedSample * FMath::Max(1.0f, PhysicsSettings.MaxImpulseVelocityChangePerStep));
                if (!AngularImpulse.IsNearlyZero())
                {
                    SkeletalMesh->AddAngularImpulseInRadians(AngularImpulse, Item.BoneName, false);
                }
            }
        }
    }

    for (const FName& BoneName : SimulatedBones)
    {
        const FSkeletalBuoyancyBoneRule* Rule = FindSkeletalBuoyancyBoneRule(SkeletalMeshSettings, BoneName);
        const FBuoyancyPhysicsSettings BonePhysicsSettings = ResolveBuoyancyPhysicsSettings(CommonPhysicsSettings, Rule);
        ApplySkeletalBoneWaterVelocityLimits(SkeletalMesh, BoneName, CurrentWaterLevel, DeltaTime, BonePhysicsSettings);
        ApplySkeletalBoneWaterAngularVelocityLimits(SkeletalMesh, BoneName, CurrentWaterLevel, DeltaTime, BonePhysicsSettings);
    }

    return bAppliedAnyImpulse;
}


void UBuoyancyComponent::UpdateBuoyancyFromGameUpdate(float DeltaTime)
{
    if (!ensureMsgf(IsInGameThread(),
        TEXT("UBuoyancyComponent::UpdateBuoyancyFromGameUpdate must run on the game thread")))
    {
        return;
    }

    if (!bInWater || DeltaTime <= SMALL_NUMBER)
    {
        return;
    }

    UPrimitiveComponent* Primitive = TargetPrimitive.Get();
    if (!IsPrimitiveReadyForForces(Primitive))
    {
        UPrimitiveComponent* NewPrimitive = ResolveTargetPrimitive();
        if (NewPrimitive != Primitive)
        {
            ClearAutoGeneratedSamplePoints();
        }
        Primitive = NewPrimitive;
        TargetPrimitive = Primitive;
    }

    if (!IsPrimitiveReadyForForces(Primitive))
    {
        return;
    }

    // Skeletal meshes use per-body sampling and bone rules. The generic primitive path is only for non-skeletal physics.
    if (USkeletalMeshComponent* SkeletalMesh = Cast<USkeletalMeshComponent>(Primitive))
    {
        ApplySkeletalMeshBuoyancy(SkeletalMesh, DeltaTime);
        return;
    }

    if (!Primitive->IsSimulatingPhysics())
    {
        return;
    }

    if (LocalSamplePoints.Num() == 0)
    {
        RebuildSamplePoints();
    }

    if (LocalSamplePoints.Num() == 0)
    {
        return;
    }

    const FBuoyancyPhysicsSettings PhysicsSettings = CommonPhysicsSettings;
    const FTransform ComponentTransform = Primitive->GetComponentTransform();
    const FVector UpVector = FVector::UpVector;
    const float CurrentWaterLevel = WaterLevel;
    const float SafeSubmersionDepth = FMath::Max(1.0f, PhysicsSettings.SubmersionDepth);
    const float GravityMagnitude = GetGravityMagnitude(this);
    const float TotalMass = GetPrimitiveMass(Primitive);
    const float MassPerSample = TotalMass / static_cast<float>(FMath::Max(1, LocalSamplePoints.Num()));
    const float ForceLimitPerSample = ComputeForceLimitPerSample(
        TotalMass,
        GravityMagnitude,
        PhysicsSettings.BuoyancyAccelerationScale,
        PhysicsSettings.BuoyancyForcePerPoint,
        PhysicsSettings.MaxForcePerPoint,
        LocalSamplePoints.Num());
    const float DragLimitPerSample = FMath::Max(1.0f, PhysicsSettings.MaxDragForcePerPoint);

    TArray<FVector> WorldPoints;
    TArray<FVector> PointVelocities;
    WorldPoints.SetNum(LocalSamplePoints.Num());
    PointVelocities.SetNum(LocalSamplePoints.Num());
    for (int32 Index = 0; Index < LocalSamplePoints.Num(); ++Index)
    {
        const FVector WorldPoint = ComponentTransform.TransformPosition(LocalSamplePoints[Index]);
        WorldPoints[Index] = WorldPoint;
        PointVelocities[Index] = Primitive->GetPhysicsLinearVelocityAtPoint(WorldPoint);
    }

    TArray<FBuoyancySampleResult> Results;
    Results.SetNum(LocalSamplePoints.Num());

    const auto CalculatePrimitiveSample = [CurrentWaterLevel, UpVector, SafeSubmersionDepth, ForceLimitPerSample, DragLimitPerSample, MassPerSample, PhysicsSettings, &WorldPoints, &PointVelocities, &Results](int32 Index)
    {
        const FVector& WorldPoint = WorldPoints[Index];
        const float Depth = CurrentWaterLevel - WorldPoint.Z;
        if (Depth <= 0.0f)
        {
            return;
        }

        const float SubmergedAlpha = FMath::Clamp(Depth / SafeSubmersionDepth, 0.0f, 1.0f);
        const FVector BuoyancyForce = UpVector * ForceLimitPerSample * SubmergedAlpha;
        const FVector DragForce = ComputeWaterDragForce(
            PointVelocities[Index],
            MassPerSample,
            SubmergedAlpha,
            PhysicsSettings);
        const FVector LinearDampingForce = -PointVelocities[Index]
            * FMath::Max(0.0f, PhysicsSettings.LinearWaterDamping)
            * MassPerSample
            * SubmergedAlpha;

        Results[Index].WorldLocation = WorldPoint;
        Results[Index].PhysicsSettings = PhysicsSettings;
        Results[Index].SubmergedAlpha = SubmergedAlpha;
        Results[Index].Force = (BuoyancyForce + DragForce + LinearDampingForce).GetClampedToMaxSize(ForceLimitPerSample + DragLimitPerSample);
        Results[Index].bActive = true;
    };

    constexpr int32 ParallelBuoyancySampleThreshold = 64;
    if (LocalSamplePoints.Num() >= ParallelBuoyancySampleThreshold)
    {
        // Only immutable numeric snapshots and disjoint result slots cross the thread boundary.
        // Physics components are queried before this call and receive impulses afterwards on GT.
        ParallelFor(LocalSamplePoints.Num(), CalculatePrimitiveSample);
    }
    else
    {
        for (int32 Index = 0; Index < LocalSamplePoints.Num(); ++Index)
        {
            CalculatePrimitiveSample(Index);
        }
    }

    const int32 SubstepCount = GetBuoyancySubstepCount(DeltaTime, PhysicsSettings.MaxBuoyancyStepSeconds);
    const float StepDeltaTime = DeltaTime / static_cast<float>(SubstepCount);
    float MaxResultSubmergedAlpha = 0.0f;
    for (const FBuoyancySampleResult& Result : Results)
    {
        if (Result.bActive)
        {
            MaxResultSubmergedAlpha = FMath::Max(MaxResultSubmergedAlpha, Result.SubmergedAlpha);
        }
    }

    for (int32 SubstepIndex = 0; SubstepIndex < SubstepCount; ++SubstepIndex)
    {
        for (const FBuoyancySampleResult& Result : Results)
        {
            if (Result.bActive && Primitive->IsSimulatingPhysics())
            {
                const FVector Impulse = ClampImpulseByVelocityChange(Result.Force * StepDeltaTime, MassPerSample, PhysicsSettings.MaxImpulseVelocityChangePerStep);
                if (!Impulse.IsNearlyZero())
                {
                    Primitive->AddImpulseAtLocation(Impulse, Result.WorldLocation);
                }
            }
        }
    }

    if (PhysicsSettings.AngularWaterDamping > 0.0f && Primitive->IsSimulatingPhysics())
    {
        const float AngularDampingScale = FMath::Max(0.0f, PhysicsSettings.AngularWaterDamping) * TotalMass;
        for (int32 SubstepIndex = 0; SubstepIndex < SubstepCount; ++SubstepIndex)
        {
            const FVector AngularVelocity = Primitive->GetPhysicsAngularVelocityInRadians();
            const FVector AngularImpulse = (-AngularVelocity * AngularDampingScale * StepDeltaTime).GetClampedToMaxSize(TotalMass * PhysicsSettings.MaxImpulseVelocityChangePerStep);
            if (!AngularImpulse.IsNearlyZero())
            {
                Primitive->AddAngularImpulseInRadians(AngularImpulse, NAME_None, false);
            }
        }
    }

    ClampPrimitiveVelocities(Primitive, PhysicsSettings, DeltaTime, MaxResultSubmergedAlpha);
}
