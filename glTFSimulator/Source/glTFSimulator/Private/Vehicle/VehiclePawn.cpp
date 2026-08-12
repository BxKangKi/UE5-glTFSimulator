// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Vehicle/VehiclePawn.h"
#include "Camera/CameraComponent.h"
#include "CollisionShape.h"
#include "Character/CharacterComponent.h"
#include "Character/CharacterController.h"
#include "Character/PlayerCharacterController.h"
#include "Components/BoxComponent.h"
#include "GameFramework/Character.h"
#include "Components/CapsuleComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/SpringArmComponent.h"
#include "Interface/WaterInteract.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeFunctionLibrary.h"
#include "glTFRuntimeParser.h"
#include "Materials/MaterialInterface.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Model/InstancedEntitySubsystem.h"
#include "Model/glTFMaterialOverrideUtils.h"
#include "Setting/GameSettings.h"
#include "System/ActorHelper.h"
#include "System/FileFunctionLibrary.h"
#include "System/GlbValidation.h"
#include "System/MathHelper.h"
#include "System/MacroLibrary.h"
#include "System/PhysicsHelper.h"
#include "System/SafeFileIO.h"
#include "System/MultiplayerWorldSubSystem.h"
#include "System/glTFRuntimeSafety.h"
#include "Net/UnrealNetwork.h"
#include "Vehicle/VehicleSubSystem.h"
#include "World/BuoyancyComponent.h"
#include "World/WaterActor.h"

static bool IsVehicleWheelTaggedName(const FString& Name)
{
    if (Name.EndsWith(TEXT(";WHEL"), ESearchCase::IgnoreCase)
        || Name.EndsWith(TEXT(";WHEEL"), ESearchCase::IgnoreCase))
    {
        return true;
    }

    const FString LowerName = Name.ToLower();
    if (LowerName.Contains(TEXT("steering")))
    {
        return false;
    }

    return LowerName.Contains(TEXT(";wheel"))
        || LowerName.StartsWith(TEXT("wheel_"))
        || LowerName.Contains(TEXT("_wheel_"))
        || LowerName.EndsWith(TEXT("_wheel"))
        || LowerName.StartsWith(TEXT("tire_"))
        || LowerName.Contains(TEXT("_tire"))
        || LowerName.StartsWith(TEXT("tyre_"))
        || LowerName.Contains(TEXT("_tyre"));
}

static FTransform GetVehicleNodeWorldTransform(const TMap<int32, FglTFRuntimeNode>& NodeMap, const FglTFRuntimeNode& Node)
{
    FTransform WorldTransform = Node.Transform;
    int32 ParentIndex = Node.ParentIndex;
    TSet<int32> VisitedParents;

    while (const FglTFRuntimeNode* Parent = NodeMap.Find(ParentIndex))
    {
        if (VisitedParents.Contains(ParentIndex) || Parent->Transform.ContainsNaN())
        {
            break;
        }
        VisitedParents.Add(ParentIndex);
        WorldTransform = WorldTransform * Parent->Transform;
        ParentIndex = Parent->ParentIndex;
    }

    return WorldTransform.ContainsNaN() ? FTransform::Identity : WorldTransform;
}

struct FVehicleWheelVisual
{
    FglTFRuntimeNode Node;
    FTransform Transform = FTransform::Identity;
};

namespace
{
    constexpr float LoadedWheelGroundContactBuffer = 0.05f;
    constexpr float LoadedVisualBodyGroundClearance = 1.0f;
    constexpr float LoadedPhysicsBodyGroundClearance = 2.0f;
    constexpr float VehiclePhysicsMaxCatchUpSeconds = 1.0f;
    constexpr int32 VehiclePhysicsHardMaxSubsteps = 64;
    constexpr float VehicleTuningMaxForce = 5000000.0f;
    constexpr int32 MaxRuntimeVehicleNodeCount = 500000;
    constexpr float VehicleTuningMaxTorque = 2000000.0f;
    constexpr float VehicleTuningMaxSpeed = 12000.0f;
    constexpr float VehicleTuningMaxGrip = 10.0f;
    constexpr float VehicleWheelHeightOffsetLimit = 200.0f;
    constexpr float VehicleLowSpeedSlopeDampingMaxSpeed = 700.0f;
    constexpr float VehicleLowSpeedSlopeDampingRate = 1.65f;
    constexpr float VehicleLowSpeedSlopeDampingMaxAlpha = 0.22f;

    static FORCEINLINE float ComputeVehicleAeroSpeedAlpha(const float Speed, const float MinimumSpeed)
    {
        // Aerodynamic downforce is speed dependent. Keeping the force at zero near
        // rest prevents idle cars from buzzing on the suspension or ground contacts.
        const float StartSpeed = FMath::Max(1.0f, MinimumSpeed);
        if (Speed <= StartSpeed)
        {
            return 0.0f;
        }

        const float FullSpeed = FMath::Max(StartSpeed + 1.0f, StartSpeed * 2.25f);
        const float Alpha = FMath::Clamp((Speed - StartSpeed) / (FullSpeed - StartSpeed), 0.0f, 1.0f);
        return Alpha * Alpha * (3.0f - 2.0f * Alpha);
    }

    static FBox TransformVehicleBounds(const FBox& LocalBounds, const FTransform& Transform)
    {
        FBox Result(ForceInit);
        if (!LocalBounds.IsValid)
        {
            return Result;
        }

        const FVector Min = LocalBounds.Min;
        const FVector Max = LocalBounds.Max;
        const FVector Corners[8] =
        {
            FVector(Min.X, Min.Y, Min.Z),
            FVector(Min.X, Min.Y, Max.Z),
            FVector(Min.X, Max.Y, Min.Z),
            FVector(Min.X, Max.Y, Max.Z),
            FVector(Max.X, Min.Y, Min.Z),
            FVector(Max.X, Min.Y, Max.Z),
            FVector(Max.X, Max.Y, Min.Z),
            FVector(Max.X, Max.Y, Max.Z)
        };

        for (const FVector& Corner : Corners)
        {
            Result += Transform.TransformPosition(Corner);
        }
        return Result;
    }
}

static void GetVehicleExitPawnCapsuleSize(const APawn* PawnToExit, float& OutRadius, float& OutHalfHeight)
{
    OutRadius = 34.0f;
    OutHalfHeight = 88.0f;

    if (const ACharacter* CharacterToExit = Cast<ACharacter>(PawnToExit))
    {
        if (const UCapsuleComponent* Capsule = CharacterToExit->GetCapsuleComponent())
        {
            OutRadius = FMath::Max(1.0f, Capsule->GetScaledCapsuleRadius());
            OutHalfHeight = FMath::Max(OutRadius + 1.0f, Capsule->GetScaledCapsuleHalfHeight());
        }
        return;
    }

    if (IsValid(PawnToExit))
    {
        const FBox PawnBounds = PawnToExit->GetComponentsBoundingBox(true);
        if (PawnBounds.IsValid)
        {
            const FVector Extent = PawnBounds.GetExtent();
            OutRadius = FMath::Max(20.0f, FMath::Max(Extent.X, Extent.Y));
            OutHalfHeight = FMath::Max(OutRadius + 1.0f, Extent.Z);
        }
    }
}

static bool IsVehicleExitLocationInWater(const UObject* WorldContextObject, const FVector& ActorLocation, float CapsuleHalfHeight, float& OutWaterLevel)
{
    float DetectedWaterLevel = OutWaterLevel;
    const float SafeHalfHeight = FMath::Max(1.0f, CapsuleHalfHeight);

    // Probe both the actor origin and the capsule bottom. The origin is useful for deep water,
    // while the bottom catches shallow water where the center may sit slightly above the surface.
    const bool bInWater = AWaterActor::FindWaterLevelAtLocation(WorldContextObject, ActorLocation, DetectedWaterLevel)
        || AWaterActor::FindWaterLevelAtLocation(WorldContextObject, ActorLocation - FVector::UpVector * SafeHalfHeight, DetectedWaterLevel);

    if (bInWater)
    {
        OutWaterLevel = DetectedWaterLevel;
    }
    return bInWater;
}


static FString ResolveReplicatedVehicleGltfPathForCurrentWorld(const UObject* WorldContextObject, const FString& InPath)
{
    const FString NormalizedPath = GlbValidation::NormalizePath(InPath);
    if (FPaths::FileExists(NormalizedPath))
    {
        return NormalizedPath;
    }

    FString RelativeModelPath = FPaths::GetCleanFilename(NormalizedPath);
    const int32 ModelSegmentIndex = NormalizedPath.Find(TEXT("/model/"), ESearchCase::IgnoreCase, ESearchDir::FromStart);
    if (ModelSegmentIndex != INDEX_NONE)
    {
        RelativeModelPath = NormalizedPath.Mid(ModelSegmentIndex + 7);
    }

    if (const UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(WorldContextObject))
    {
        const FString WorldFolderName = Multiplayer->GetSelectedWorldFolderName();
        if (!WorldFolderName.IsEmpty())
        {
            const FString Candidate = GlbValidation::NormalizePath(
                FPaths::Combine(PATH_ROOT, WorldFolderName, TEXT("model"), RelativeModelPath));
            if (FPaths::FileExists(Candidate))
            {
                return Candidate;
            }
        }
    }

    return FString();
}

static void ApplyVehicleWaterExitState(APawn* RestoredPawn, float WaterLevel)
{
    if (!IsValid(RestoredPawn))
    {
        return;
    }

    // Directly refresh the water interaction because the pawn was hidden and collision-disabled
    // while seated in the vehicle, so overlap events can be stale or missing on the exit frame.
    if (IWaterInteract* WaterInteract = Cast<IWaterInteract>(RestoredPawn))
    {
        WaterInteract->EnterWater(WaterLevel);
    }

    if (ACharacter* Character = Cast<ACharacter>(RestoredPawn))
    {
        if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
        {
            Movement->StopMovementImmediately();
            Movement->SetMovementMode(MOVE_Swimming);
        }
    }
}

AVehiclePawn::AVehiclePawn()
{
    PrimaryActorTick.bCanEverTick = false;
    PrimaryActorTick.TickGroup = TG_PrePhysics;
    bReplicates = true;
    SetReplicateMovement(true);
    SetNetUpdateFrequency(30.0f);
    SetMinNetUpdateFrequency(10.0f);
    bUseAsyncVehiclePhysicsTick = false;
    bRunVehicleForcesInAsyncPhysicsTick = false;
    bAsyncPhysicsTickEnabled = false;

    // Always use the force-based wheel simulation by default. The old deterministic ground solver
    // is kept for compatibility but caused unrealistic sticking/snap behavior.
    bUseStableGroundRideHeight = false;

    // Default to a realistic passenger-car mass. ApplyVehicleBodyPhysicsSettings() pushes this value
    // into the Chaos body so acceleration, suspension support, and tire loads are all based on ~1 ton.
    VehicleMassKg = 1000.0f;

    Body = CreateDefaultSubobject<UBoxComponent>(TEXT("PhysicsBody"));
    SetRootComponent(Body);
    Body->InitBoxExtent(BodyExtent);
    Body->SetCollisionProfileName(TEXT("Vehicle"));
    Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Body->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
    Body->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
    Body->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);

    // SetMassOverrideInKg() and related body updates query physical materials internally.
    // They must not run while Unreal is constructing the native class default object for Cook.
    if (!HasAnyFlags(RF_ClassDefaultObject))
    {
        ApplyVehicleBodyPhysicsSettings();
        Body->SetUseCCD(true);
    }

    LowFrictionPhysicalMaterial = CreateDefaultSubobject<UPhysicalMaterial>(TEXT("VehicleLowFrictionMaterial"));
    if (LowFrictionPhysicalMaterial)
    {
        LowFrictionPhysicalMaterial->Friction = 0.06f;
        LowFrictionPhysicalMaterial->StaticFriction = 0.06f;
        LowFrictionPhysicalMaterial->bOverrideFrictionCombineMode = true;
        LowFrictionPhysicalMaterial->FrictionCombineMode = EFrictionCombineMode::Min;
        LowFrictionPhysicalMaterial->Restitution = 0.0f;
        if (!HasAnyFlags(RF_ClassDefaultObject))
        {
            Body->SetPhysMaterialOverride(LowFrictionPhysicalMaterial);
        }
    }

    BuoyancyComponent = CreateDefaultSubobject<UBuoyancyComponent>(TEXT("Buoyancy"));

    bUseControllerRotationPitch = false;
    bUseControllerRotationYaw = false;
    bUseControllerRotationRoll = false;

    SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("VehicleSpringArm"));
    SpringArm->SetupAttachment(Body);
    SpringArm->TargetArmLength = 550.0f;
    SpringArm->SetRelativeRotation(FRotator(-12.0f, 0.0f, 0.0f));
    SpringArm->bUsePawnControlRotation = true;
    SpringArm->bInheritPitch = true;
    SpringArm->bInheritYaw = true;
    SpringArm->bInheritRoll = false;
    SpringArm->bEnableCameraLag = true;
    SpringArm->CameraLagSpeed = 8.0f;
    SpringArm->bEnableCameraRotationLag = true;
    SpringArm->CameraRotationLagSpeed = 12.0f;

    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("VehicleCamera"));
    Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
    Camera->bUsePawnControlRotation = false;

}

void AVehiclePawn::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AVehiclePawn, ReplicatedSourceFilePath);
    DOREPLIFETIME(AVehiclePawn, ReplicatedObjectName);
}

void AVehiclePawn::OnRep_VehicleModelReplicationData()
{
    if (ReplicatedSourceFilePath.IsEmpty())
    {
        ClearLoadedVehicleModel();
        return;
    }

    const FString ResolvedPath = ResolveReplicatedVehicleGltfPathForCurrentWorld(this, ReplicatedSourceFilePath);
    if (ResolvedPath.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("VehiclePawn: replicated source file is missing; load skipped. Source=%s"), *ReplicatedSourceFilePath);
        ClearLoadedVehicleModel();
        return;
    }

    LoadVehicleModel(ResolvedPath, ReplicatedObjectName);
}

void AVehiclePawn::BeginPlay()
{
    Super::BeginPlay();
    // The runtime vehicle is tuned as an approximately one-ton car. Force the mass here so
    // older serialized/placed instances cannot keep an unintended lightweight value.
    VehicleMassKg = 1000.0f;
    bUseStableGroundRideHeight = false;
    bUseAsyncVehiclePhysicsTick = false;
    bRunVehicleForcesInAsyncPhysicsTick = false;
    bAsyncPhysicsTickEnabled = false;
    LastObservedAsyncVehiclePhysicsStepCounter = AsyncVehiclePhysicsStepCounter.GetValue();
    bHasObservedAsyncVehiclePhysicsStep = false;
    Body->InitBoxExtent(BodyExtent);
    Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Body->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
    Body->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
    Body->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
    ApplyVehicleBodyPhysicsSettings();
    Body->SetUseCCD(true);
    if (LowFrictionPhysicalMaterial)
    {
        Body->SetPhysMaterialOverride(LowFrictionPhysicalMaterial);
    }
    const bool bClientRenderOnlyVehicle = UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this) && !HasAuthority();
    if (bClientRenderOnlyVehicle)
    {
        Body->SetSimulatePhysics(false);
        Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Body->SetGenerateOverlapEvents(false);
    }
    else if (bVehicleModelLoaded)
    {
        ResetVehiclePoseAboveGround();
    }
    else
    {
        DeactivateVehicleUntilModelLoaded();
    }

    WheelSpinDegrees.Init(0.0f, WheelOffsets.Num());
    WheelSpringLengths.SetNum(WheelOffsets.Num());
    WheelVisualSpringLengths.SetNum(WheelOffsets.Num());
    for (int32 WheelIndex = 0; WheelIndex < WheelOffsets.Num(); ++WheelIndex)
    {
        const float TargetSpringLength = GetTargetWheelSpringLength(WheelIndex);
        WheelSpringLengths[WheelIndex] = TargetSpringLength;
        WheelVisualSpringLengths[WheelIndex] = TargetSpringLength;
    }
    const float InitialSupportForce = (WheelOffsets.Num() > 0 && GetWorld())
        ? FMath::Max(1.0f, VehicleMassKg) * FMath::Max(1.0f, FMath::Abs(GetWorld()->GetGravityZ())) / static_cast<float>(WheelOffsets.Num())
        : 0.0f;
    WheelSuspensionForces.Init(InitialSupportForce, WheelOffsets.Num());
    WheelLateralForces.Init(0.0f, WheelOffsets.Num());
    WheelGrounded.Init(false, WheelOffsets.Num());

    if (!bClientRenderOnlyVehicle && bVehicleModelLoaded)
    {
        if (UVehicleSubSystem* VehicleSubSystem = UVehicleSubSystem::Get(this))
        {
            VehicleSubSystem->RegisterVehicle(this);
        }
    }
}

void AVehiclePawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);
}

float AVehiclePawn::GetVehicleMassScale() const
{
    return FMath::Max(1.0f, VehicleMassKg) / 1000.0f;
}

float AVehiclePawn::GetEffectiveWheelRadius(int32 WheelIndex) const
{
    if (LoadedWheelGroundRadii.IsValidIndex(WheelIndex))
    {
        const float PerWheelRadius = LoadedWheelGroundRadii[WheelIndex];
        if (FMath::IsFinite(PerWheelRadius) && PerWheelRadius > 0.0f)
        {
            return FMath::Max(1.0f, PerWheelRadius);
        }
    }

    return FMath::Max(1.0f, RuntimeWheelRadius > 0.0f ? RuntimeWheelRadius : WheelRadius);
}

float AVehiclePawn::GetPhysicsBodyGroundClearance() const
{
    const float HalfWheelClearance = GetEffectiveWheelRadius() * FMath::Clamp(BodyGroundClearanceWheelRadiusRatio, 0.10f, 1.00f);
    const float ConfiguredClearance = LoadedWheelVisualRestBounds.IsValid
        ? LoadedPhysicsBodyGroundClearance
        : MinimumBodyGroundClearance;

    // Do not let old serialized defaults keep the body a full wheel-height above the road.
    // Use the smaller of the legacy configured clearance and the desired wheel-relative clearance,
    // but keep a small skid-plate gap for uneven ground.
    return FMath::Max(1.0f, FMath::Min(FMath::Max(0.0f, ConfiguredClearance), HalfWheelClearance));
}

float AVehiclePawn::GetMinimumWheelSpringLength(int32 WheelIndex) const
{
    const float SafeWheelRadius = GetEffectiveWheelRadius(WheelIndex);
    const float CompressionTravel = FMath::Clamp(MaxWheelCompressionTravel, 2.0f, 30.0f);
    const float FallbackMinimum = FMath::Max(3.0f, SafeWheelRadius * 0.30f);
    const float SafeBodyHalfHeight = FMath::Max(0.0f, BodyExtent.Z);
    const float SafeClearance = FMath::Max(0.0f, MinimumWheelBodyClearance);

    auto GetBodyClearanceMinimum = [this, SafeWheelRadius, SafeBodyHalfHeight, SafeClearance, FallbackMinimum](int32 InWheelIndex)
    {
        if (!WheelOffsets.IsValidIndex(InWheelIndex))
        {
            return FallbackMinimum;
        }

        // WheelOffsets are body-local. The chassis underside is at -BodyExtent.Z; therefore
        // a wheel mounted higher inside the wheel well still needs enough spring length to
        // keep the tire crown from crossing through the collision body.
        const float WheelMountHeightAboveBodyBottom = WheelOffsets[InWheelIndex].Z + SafeBodyHalfHeight;
        return FMath::Max(FallbackMinimum, WheelMountHeightAboveBodyBottom + SafeWheelRadius + SafeClearance);
    };

    if (WheelTargetSpringLengths.IsValidIndex(WheelIndex))
    {
        // Runtime glTF wheels already provide an authored ride pose. A single chassis box has no
        // wheel wells, so forcing the tire crown outside that box pushes the rendered wheels down
        // and creates an artificial gap below the body. Preserve the authored pose and reserve only
        // the configured upward compression stroke.
        const float AuthoredTargetLength = FMath::Max(FallbackMinimum + 1.0f, WheelTargetSpringLengths[WheelIndex]);
        return FMath::Max(FallbackMinimum, AuthoredTargetLength - CompressionTravel);
    }

    if (WheelIndex == INDEX_NONE && WheelTargetSpringLengths.Num() > 0)
    {
        float MinimumLength = FallbackMinimum;
        for (const float TargetLength : WheelTargetSpringLengths)
        {
            const float AuthoredTargetLength = FMath::Max(FallbackMinimum + 1.0f, TargetLength);
            MinimumLength = FMath::Max(MinimumLength, AuthoredTargetLength - CompressionTravel);
        }
        return MinimumLength;
    }

    if (WheelOffsets.IsValidIndex(WheelIndex))
    {
        return GetBodyClearanceMinimum(WheelIndex);
    }

    if (WheelIndex == INDEX_NONE && WheelOffsets.Num() > 0)
    {
        float MinimumLength = FallbackMinimum;
        for (int32 Index = 0; Index < WheelOffsets.Num(); ++Index)
        {
            MinimumLength = FMath::Max(MinimumLength, GetBodyClearanceMinimum(Index));
        }
        return MinimumLength;
    }

    return FallbackMinimum;
}

float AVehiclePawn::GetEffectiveSuspensionRestLength(int32 WheelIndex) const
{
    const float MinimumLength = GetMinimumWheelSpringLength(WheelIndex);
    const float CompressionTravel = FMath::Clamp(MaxWheelCompressionTravel, 2.0f, 30.0f);
    const float DroopTravel = FMath::Clamp(MaxWheelDroopTravel, 4.0f, 80.0f);

    const float SafeRideHeightOffset = FMath::Clamp(RideHeightOffset, -30.0f, 30.0f);
    if (WheelTargetSpringLengths.IsValidIndex(WheelIndex))
    {
        const float TargetLength = FMath::Max(
            MinimumLength + 1.0f,
            WheelTargetSpringLengths[WheelIndex] + SafeRideHeightOffset);
        return FMath::Max(TargetLength + DroopTravel, MinimumLength + CompressionTravel + DroopTravel);
    }

    if (WheelIndex == INDEX_NONE && WheelTargetSpringLengths.Num() > 0)
    {
        float MaxLength = FMath::Max(4.0f, SuspensionRestLength);
        for (float TargetLength : WheelTargetSpringLengths)
        {
            MaxLength = FMath::Max(MaxLength, TargetLength + SafeRideHeightOffset + DroopTravel);
        }
        return FMath::Max(MaxLength, MinimumLength + CompressionTravel + DroopTravel);
    }

    return FMath::Max(FMath::Max(4.0f, SuspensionRestLength), MinimumLength + CompressionTravel + DroopTravel);
}

float AVehiclePawn::GetTargetWheelSpringLength(int32 WheelIndex) const
{
    const float MinimumLength = GetMinimumWheelSpringLength(WheelIndex);
    const float RestLength = GetEffectiveSuspensionRestLength(WheelIndex);
    const float CompressionTravel = FMath::Clamp(MaxWheelCompressionTravel, 2.0f, 30.0f);

    const float SafeRideHeightOffset = FMath::Clamp(RideHeightOffset, -30.0f, 30.0f);
    if (WheelTargetSpringLengths.IsValidIndex(WheelIndex))
    {
        return FMath::Clamp(
            WheelTargetSpringLengths[WheelIndex] + SafeRideHeightOffset,
            MinimumLength + 1.0f,
            RestLength - 1.0f);
    }

    if (WheelIndex == INDEX_NONE && WheelTargetSpringLengths.Num() > 0)
    {
        float Sum = 0.0f;
        for (float TargetLength : WheelTargetSpringLengths)
        {
            Sum += TargetLength;
        }
        const float AverageTarget = Sum / static_cast<float>(WheelTargetSpringLengths.Num());
        return FMath::Clamp(AverageTarget + SafeRideHeightOffset, MinimumLength + 1.0f, RestLength - 1.0f);
    }

    // Fallback/default geometry: static ride point is one compression stroke above the safe
    // minimum, with a separate droop stroke below it for slopes and uneven terrain.
    const float RatioTarget = RestLength * FMath::Clamp(LoadedWheelVisualRestLengthRatio, 0.55f, 0.90f);
    const float StrokeTarget = MinimumLength + CompressionTravel;
    return FMath::Clamp(
        FMath::Max(RatioTarget, StrokeTarget) + SafeRideHeightOffset,
        MinimumLength + 1.0f,
        RestLength - 1.0f);
}

float AVehiclePawn::GetStableWheelVisualSpringLength() const
{
    return GetTargetWheelSpringLength(INDEX_NONE);
}


void AVehiclePawn::ApplyVehicleBodyPhysicsSettings()
{
    if (!IsValid(Body))
    {
        return;
    }

    Body->SetMassOverrideInKg(NAME_None, FMath::Max(1.0f, VehicleMassKg), true);
    Body->SetSimulatePhysics(!bUseStableGroundRideHeight);
    Body->SetEnableGravity(!bUseStableGroundRideHeight);
    // In deterministic wheel-physics mode gravity, damping, and integration are applied by our own
    // substepped solver. Chaos still owns collision sweeps/overlaps, but it should not add another
    // gravity or damping layer that changes behavior between sync and async physics.
    Body->SetLinearDamping(bUseStableGroundRideHeight ? 0.05f : 0.30f);
    Body->SetAngularDamping(bUseStableGroundRideHeight ? 0.20f : 3.6f);
    // Keep the chassis stable, but do not put the center of mass unrealistically far below the car.
    // A one-ton car with a low in-body COM gives natural pitch/roll on slopes while avoiding side wobble.
    const float CenterOfMassZ = FMath::Clamp(-BodyExtent.Z * 0.55f, -75.0f, -18.0f);
    Body->SetCenterOfMass(FVector(4.0f, 0.0f, CenterOfMassZ), NAME_None);
    Body->SetPhysicsMaxAngularVelocityInRadians(FMath::Max(0.5f, MaxAngularVelocityRadians), false);
}


void AVehiclePawn::RunVehiclePhysicsSteps(float DeltaSeconds, bool bFromAsyncPhysicsTick)
{
    if (DeltaSeconds <= 0.0f || !IsValid(Body) || !Body->IsSimulatingPhysics())
    {
        return;
    }

    // Keep the total simulated time instead of dropping long frames. The step size is bounded so
    // tire/suspension smoothing, input smoothing, and impulse integration see the same seconds in
    // normal Tick and in Chaos async physics tick.
    const float SafeDeltaSeconds = FMath::Clamp(DeltaSeconds, 0.0f, VehiclePhysicsMaxCatchUpSeconds);
    const float MaxStepSeconds = FMath::Clamp(
        bUseStableGroundRideHeight ? StableMaxSimulationStepSeconds : MaxVehiclePhysicsSubstepSeconds,
        0.004f,
        0.05f);
    const int32 RequestedMaxStepCount = bUseStableGroundRideHeight ? StableMaxSimulationSubsteps : MaxVehiclePhysicsSubsteps;
    const int32 MaxStepCount = FMath::Clamp(RequestedMaxStepCount, 1, VehiclePhysicsHardMaxSubsteps);
    const int32 StepCount = FMath::Clamp(FMath::CeilToInt(SafeDeltaSeconds / MaxStepSeconds), 1, MaxStepCount);
    const float StepSeconds = SafeDeltaSeconds / static_cast<float>(StepCount);

    for (int32 StepIndex = 0; StepIndex < StepCount; ++StepIndex)
    {
        StepVehiclePhysics(StepSeconds, bFromAsyncPhysicsTick);
    }
}

void AVehiclePawn::StepVehiclePhysics(float DeltaSeconds, bool bFromAsyncPhysicsTick)
{
    const float SafeStepSeconds = FMath::Clamp(DeltaSeconds, 0.001f, 0.05f);
    if (SafeStepSeconds <= 0.0f)
    {
        return;
    }

    // All physics-side vehicle work comes through this function. Forces and torques are converted
    // to impulses with this exact step delta, so the total impulse over one real second is independent
    // of game-frame rate and of whether Chaos async physics tick is enabled.
    bApplyingAsyncVehiclePhysicsStep = bFromAsyncPhysicsTick;
    CurrentVehiclePhysicsStepSeconds = SafeStepSeconds;

    if (!bSkipVehicleInputSmoothingForCurrentRun)
    {
        UpdateVehicleInputSmoothing(SafeStepSeconds);
    }

    if (bUseStableGroundRideHeight)
    {
        UpdateStableWheelVehicle(SafeStepSeconds);
    }
    else
    {
        ApplySuspensionAndDrive(SafeStepSeconds);
    }

    CurrentVehiclePhysicsStepSeconds = 0.0f;
    bApplyingAsyncVehiclePhysicsStep = false;
}

void AVehiclePawn::UpdateVehicleInputSmoothing(float DeltaSeconds)
{
    const float SafeDeltaSeconds = FMath::Clamp(DeltaSeconds, 0.0f, 0.05f);
    if (SafeDeltaSeconds <= 0.0f)
    {
        return;
    }

    SmoothedThrottleInput = FMath::FInterpTo(
        SmoothedThrottleInput,
        ThrottleInput,
        SafeDeltaSeconds,
        FMath::Max(0.1f, ThrottleInputInterpSpeed));

    const float ClampedSteeringTarget = FMath::Clamp(SteeringInput, -1.0f, 1.0f);
    const float CurvedSteeringTarget = FMath::Sign(ClampedSteeringTarget)
        * FMath::Pow(FMath::Abs(ClampedSteeringTarget), FMath::Clamp(SteeringInputCurveExponent, 1.0f, 3.0f));

    const FVector BodyForward = IsValid(Body) ? Body->GetForwardVector().GetSafeNormal() : GetActorForwardVector().GetSafeNormal();
    const FVector BodyVelocity = IsValid(Body) ? Body->GetPhysicsLinearVelocity() : FVector::ZeroVector;
    const float AbsForwardSpeed = FMath::Abs(FVector::DotProduct(BodyVelocity, BodyForward));
    const float SpeedAlpha = FMathHelper::Saturate(AbsForwardSpeed / FMath::Max(100.0f, SteeringSpeedForFullAssist));
    const float SpeedRateScale = FMath::Lerp(1.0f, 1.0f - FMath::Clamp(SteeringInputSpeedDamping, 0.0f, 1.0f), SpeedAlpha);

    const bool bReturningTowardCenter = FMath::Abs(CurvedSteeringTarget) < FMath::Abs(SmoothedSteeringInput)
        && (FMath::IsNearlyZero(CurvedSteeringTarget, 0.001f)
            || FMath::Sign(CurvedSteeringTarget) == FMath::Sign(SmoothedSteeringInput));
    const bool bReversingDirection = CurvedSteeringTarget * SmoothedSteeringInput < -KINDA_SMALL_NUMBER;
    float SteeringRate = bReturningTowardCenter
        ? FMath::Max(0.1f, SteeringInputReturnRate)
        : FMath::Max(0.1f, SteeringInputRiseRate);
    if (bReversingDirection)
    {
        // Crossing center is intentionally slower than snapping from left lock to right lock.
        SteeringRate = FMath::Min(SteeringRate, FMath::Max(0.1f, SteeringInputRiseRate) * 0.75f);
    }

    SmoothedSteeringInput = FMath::FInterpConstantTo(
        SmoothedSteeringInput,
        CurvedSteeringTarget,
        SafeDeltaSeconds,
        FMath::Max(0.1f, SteeringRate * SpeedRateScale));
}

AVehiclePawn::FVehicleParallelControlInput AVehiclePawn::BuildParallelControlInput(float DeltaSeconds) const
{
    FVehicleParallelControlInput Input;
    Input.DeltaSeconds = DeltaSeconds;
    Input.ThrottleInput = ThrottleInput;
    Input.SteeringInput = SteeringInput;
    Input.SmoothedThrottleInput = SmoothedThrottleInput;
    Input.SmoothedSteeringInput = SmoothedSteeringInput;
    Input.ThrottleInputInterpSpeed = ThrottleInputInterpSpeed;
    Input.SteeringInputRiseRate = SteeringInputRiseRate;
    Input.SteeringInputReturnRate = SteeringInputReturnRate;
    Input.SteeringInputSpeedDamping = SteeringInputSpeedDamping;
    Input.SteeringInputCurveExponent = SteeringInputCurveExponent;
    Input.SteeringSpeedForFullAssist = SteeringSpeedForFullAssist;
    Input.BodyForward = IsValid(Body) ? Body->GetForwardVector().GetSafeNormal() : GetActorForwardVector().GetSafeNormal();
    Input.BodyVelocity = IsValid(Body) ? Body->GetPhysicsLinearVelocity() : FVector::ZeroVector;
    return Input;
}

AVehiclePawn::FVehicleParallelControlOutput AVehiclePawn::CalculateParallelControlOutput(const FVehicleParallelControlInput& Input)
{
    FVehicleParallelControlOutput Output;

    const float SafeDeltaSeconds = FMath::Clamp(Input.DeltaSeconds, 0.0f, 0.05f);
    if (SafeDeltaSeconds <= 0.0f)
    {
        return Output;
    }

    Output.SmoothedThrottleInput = FMath::FInterpTo(
        Input.SmoothedThrottleInput,
        FMath::Clamp(Input.ThrottleInput, -1.0f, 1.0f),
        SafeDeltaSeconds,
        FMath::Max(0.1f, Input.ThrottleInputInterpSpeed));

    const float ClampedSteeringTarget = FMath::Clamp(Input.SteeringInput, -1.0f, 1.0f);
    const float CurvedSteeringTarget = FMath::Sign(ClampedSteeringTarget)
        * FMath::Pow(FMath::Abs(ClampedSteeringTarget), FMath::Clamp(Input.SteeringInputCurveExponent, 1.0f, 3.0f));

    const FVector BodyForward = Input.BodyForward.GetSafeNormal();
    const float AbsForwardSpeed = FMath::Abs(FVector::DotProduct(Input.BodyVelocity, BodyForward));
    const float SpeedAlpha = FMath::Clamp(AbsForwardSpeed / FMath::Max(100.0f, Input.SteeringSpeedForFullAssist), 0.0f, 1.0f);
    const float SpeedRateScale = FMath::Lerp(1.0f, 1.0f - FMath::Clamp(Input.SteeringInputSpeedDamping, 0.0f, 1.0f), SpeedAlpha);

    const bool bReturningTowardCenter = FMath::Abs(CurvedSteeringTarget) < FMath::Abs(Input.SmoothedSteeringInput)
        && (FMath::IsNearlyZero(CurvedSteeringTarget, 0.001f)
            || FMath::Sign(CurvedSteeringTarget) == FMath::Sign(Input.SmoothedSteeringInput));
    const bool bReversingDirection = CurvedSteeringTarget * Input.SmoothedSteeringInput < -KINDA_SMALL_NUMBER;
    float SteeringRate = bReturningTowardCenter
        ? FMath::Max(0.1f, Input.SteeringInputReturnRate)
        : FMath::Max(0.1f, Input.SteeringInputRiseRate);
    if (bReversingDirection)
    {
        SteeringRate = FMath::Min(SteeringRate, FMath::Max(0.1f, Input.SteeringInputRiseRate) * 0.75f);
    }

    Output.SmoothedSteeringInput = FMath::FInterpConstantTo(
        Input.SmoothedSteeringInput,
        CurvedSteeringTarget,
        SafeDeltaSeconds,
        FMath::Max(0.1f, SteeringRate * SpeedRateScale));
    Output.bValid = true;
    return Output;
}

void AVehiclePawn::ApplyParallelControlOutput(const FVehicleParallelControlOutput& Output)
{
    if (!Output.bValid)
    {
        return;
    }

    SmoothedThrottleInput = FMath::Clamp(Output.SmoothedThrottleInput, -1.0f, 1.0f);
    SmoothedSteeringInput = FMath::Clamp(Output.SmoothedSteeringInput, -1.0f, 1.0f);
}

void AVehiclePawn::UpdateVehicleFromSubSystem(float DeltaSeconds)
{
    if (!ShouldUpdateVehicleSimulation())
    {
        return;
    }

    if (UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this) && !HasAuthority())
    {
        return;
    }

    const float SafeFrameDeltaTime = FMath::Clamp(DeltaSeconds, 0.0f, VehiclePhysicsMaxCatchUpSeconds);
    if (SafeFrameDeltaTime <= 0.0f || !IsValid(Body))
    {
        return;
    }

    bUseAsyncVehiclePhysicsTick = false;
    bRunVehicleForcesInAsyncPhysicsTick = false;
    bAsyncPhysicsTickEnabled = false;

    {
        struct FScopedBoolOverride
        {
            bool& Flag;
            const bool PreviousValue;

            FScopedBoolOverride(bool& InFlag, const bool NewValue)
                : Flag(InFlag)
                , PreviousValue(InFlag)
            {
                Flag = NewValue;
            }

            ~FScopedBoolOverride()
            {
                Flag = PreviousValue;
            }
        } SkipInputSmoothingGuard(bSkipVehicleInputSmoothingForCurrentRun, true);

        RunVehiclePhysicsSteps(SafeFrameDeltaTime, false);
    }

    UpdateWheelVisuals(SafeFrameDeltaTime);
}


void AVehiclePawn::SetDriveInput(float Throttle, float Steering)
{
    ThrottleInput = FMath::Clamp(Throttle, -1.0f, 1.0f);
    SteeringInput = FMath::Clamp(Steering, -1.0f, 1.0f);

    if (!HasAuthority())
    {
        ServerSetDriveInput(ThrottleInput, SteeringInput);
    }

    if (IsValid(Body) && Body->IsSimulatingPhysics() && (!FMath::IsNearlyZero(ThrottleInput, 0.01f) || !FMath::IsNearlyZero(SteeringInput, 0.01f)))
    {
        Body->WakeRigidBody();
        Body->WakeAllRigidBodies();
    }
}

void AVehiclePawn::SetThrottleInput(float Throttle)
{
    SetDriveInput(Throttle, SteeringInput);
}

void AVehiclePawn::SetSteeringInput(float Steering)
{
    SetDriveInput(ThrottleInput, Steering);
}

void AVehiclePawn::ServerSetDriveInput_Implementation(float Throttle, float Steering)
{
    ThrottleInput = FMath::Clamp(Throttle, -1.0f, 1.0f);
    SteeringInput = FMath::Clamp(Steering, -1.0f, 1.0f);

    if (IsValid(Body) && Body->IsSimulatingPhysics() && (!FMath::IsNearlyZero(ThrottleInput, 0.01f) || !FMath::IsNearlyZero(SteeringInput, 0.01f)))
    {
        Body->WakeRigidBody();
        Body->WakeAllRigidBodies();
    }
}

void AVehiclePawn::ClearDriveInput()
{
    ThrottleInput = 0.0f;
    SteeringInput = 0.0f;
    SmoothedThrottleInput = 0.0f;
    SmoothedSteeringInput = 0.0f;
    SmoothedStableYawRate = 0.0f;

    if (!HasAuthority())
    {
        ServerSetDriveInput(0.0f, 0.0f);
    }
}

void AVehiclePawn::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UVehicleSubSystem* VehicleSubSystem = UVehicleSubSystem::Get(this))
    {
        VehicleSubSystem->UnregisterVehicle(this);
    }

    ReleaseRuntimeResources();
    Super::EndPlay(EndPlayReason);
}

void AVehiclePawn::Destroyed()
{
    ReleaseRuntimeResources();
    Super::Destroyed();
}

void AVehiclePawn::ReleaseRuntimeResources()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("AVehiclePawn runtime release must run on the game thread")))
    {
        return;
    }

    ClearDriveInput();
    if (IsOccupied())
    {
        ExitVehicle();
    }
    ClearLoadedVehicleModel();
}

void AVehiclePawn::ClearLoadedVehicleModel()
{
    if (UVehicleSubSystem* VehicleSubSystem = UVehicleSubSystem::Get(this))
    {
        VehicleSubSystem->UnregisterVehicle(this);
    }

    if (InstancedRenderRegistrationId != INDEX_NONE)
    {
        if (UInstancedEntitySubsystem* InstancedEntities = UInstancedEntitySubsystem::Get(this))
        {
            InstancedEntities->UnregisterEntity(InstancedRenderRegistrationId);
        }
        InstancedRenderRegistrationId = INDEX_NONE;
    }

    // Generated meshes are retained by the shared ISM actor while at least one entity uses the
    // source prefab. This actor owns only a temporary load cache and can release it immediately.
    MeshCache.Empty();
    if (IsValid(GltfAsset))
    {
        FglTFRuntimeSafety::RequestAssetRelease(GltfAsset);
        GltfAsset = nullptr;
    }

    LoadedWheelRenderPartIndices.Empty();
    LoadedWheelBaseRotations.Empty();
    LoadedWheelBaseScales.Empty();
    LoadedWheelVisualCenterOffsets.Empty();
    LoadedWheelGroundRadii.Empty();
    WheelOffsets.Empty();
    WheelTargetSpringLengths.Empty();
    WheelSpinDegrees.Empty();
    WheelSpringLengths.Empty();
    WheelVisualSpringLengths.Empty();
    WheelSuspensionForces.Empty();
    WheelLateralForces.Empty();
    WheelGrounded.Empty();
    LoadedBodyVisualBounds.Init();
    LoadedWheelVisualRestBounds.Init();
    RuntimeWheelRadius = 0.0f;

    if (const UClass* VehicleClass = GetClass())
    {
        const AVehiclePawn* Defaults = VehicleClass->GetDefaultObject<AVehiclePawn>();
        if (Defaults && Defaults != this)
        {
            BodyExtent = Defaults->BodyExtent;
        }
    }
    if (IsValid(Body))
    {
        Body->SetBoxExtent(BodyExtent, false);
    }

    StablePlanarVelocity = FVector::ZeroVector;
    StableVerticalVelocity = 0.0f;
    bStablePlanarVelocityInitialized = false;
    StablePhysicsLinearVelocity = FVector::ZeroVector;
    StablePhysicsAngularVelocity = FVector::ZeroVector;
    bStablePhysicsStateInitialized = false;

    SourceFilePath.Reset();
    BaseName.Reset();
    ObjectName.Reset();
    bVehicleModelLoaded = false;
    if (HasAuthority())
    {
        ReplicatedSourceFilePath.Reset();
        ReplicatedObjectName.Reset();
    }
    DeactivateVehicleUntilModelLoaded();
}


FString AVehiclePawn::ResolveVehicleTuningJsonPath(const FString& ModelPath) const
{
    if (ModelPath.IsEmpty())
    {
        return FString();
    }

    const FString FullModelPath = FPaths::ConvertRelativePathToFull(ModelPath);
    const FString ModelDirectory = FPaths::GetPath(FullModelPath);
    const FString ModelBaseName = FPaths::GetBaseFilename(FullModelPath);
    if (ModelDirectory.IsEmpty() || ModelBaseName.IsEmpty())
    {
        return FString();
    }

    // Per-model driving data lives beside the prefab/model file as "ModelName.json".
    return FPaths::Combine(ModelDirectory, ModelBaseName + TEXT(".json"));
}

FString AVehiclePawn::GetVehicleTuningJsonPath() const
{
    return ResolveVehicleTuningJsonPath(SourceFilePath);
}

bool AVehiclePawn::IsWheelMeshName(const FString& Name) const
{
    const FString TrimmedName = Name.TrimStartAndEnd();
    if (TrimmedName.IsEmpty())
    {
        return false;
    }

    if (IsVehicleWheelTaggedName(TrimmedName))
    {
        return true;
    }

    auto GetBaseNameBeforeTag = [](const FString& Value)
    {
        FString Result = Value.TrimStartAndEnd();
        int32 SeparatorIndex = INDEX_NONE;
        if (Result.FindChar(TEXT(';'), SeparatorIndex) && SeparatorIndex >= 0)
        {
            Result = Result.Left(SeparatorIndex).TrimStartAndEnd();
        }
        return Result;
    };

    const FString CandidateBaseName = GetBaseNameBeforeTag(TrimmedName);
    for (const FString& ConfiguredName : WheelMeshNames)
    {
        const FString ConfiguredFullName = ConfiguredName.TrimStartAndEnd();
        if (ConfiguredFullName.IsEmpty())
        {
            continue;
        }

        const FString ConfiguredBaseName = GetBaseNameBeforeTag(ConfiguredFullName);
        if (TrimmedName.Equals(ConfiguredFullName, ESearchCase::IgnoreCase)
            || (!CandidateBaseName.IsEmpty() && CandidateBaseName.Equals(ConfiguredFullName, ESearchCase::IgnoreCase))
            || (!ConfiguredBaseName.IsEmpty() && TrimmedName.Equals(ConfiguredBaseName, ESearchCase::IgnoreCase))
            || (!CandidateBaseName.IsEmpty() && !ConfiguredBaseName.IsEmpty()
                && CandidateBaseName.Equals(ConfiguredBaseName, ESearchCase::IgnoreCase)))
        {
            return true;
        }
    }
    return false;
}

void AVehiclePawn::ResetVehicleTuningToClassDefaults()
{
    const UClass* VehicleClass = GetClass();
    const AVehiclePawn* Defaults = VehicleClass ? VehicleClass->GetDefaultObject<AVehiclePawn>() : nullptr;
    if (!Defaults || Defaults == this)
    {
        return;
    }

    // Reset only designer/player-facing driving tune values. Physical constants such as mass,
    // suspension, gravity, body collision, and center of mass remain owned by code/editor defaults.
    EngineForce = Defaults->EngineForce;
    ReverseForce = Defaults->ReverseForce;
    BrakeForce = Defaults->BrakeForce;
    SteeringTorque = Defaults->SteeringTorque;
    MaxSteeringAngleDegrees = Defaults->MaxSteeringAngleDegrees;
    MinSteeringSpeedFactor = Defaults->MinSteeringSpeedFactor;
    MaxSteeringSpeedFactor = Defaults->MaxSteeringSpeedFactor;
    SteeringSpeedForFullAssist = Defaults->SteeringSpeedForFullAssist;
    SteeringYawRateAssist = Defaults->SteeringYawRateAssist;
    HighSpeedYawAssistStrength = Defaults->HighSpeedYawAssistStrength;
    HighSpeedYawAssistStartSpeed = Defaults->HighSpeedYawAssistStartSpeed;
    SteeringYawDamping = Defaults->SteeringYawDamping;
    MaxSteeringAssistTorque = Defaults->MaxSteeringAssistTorque;
    LowSpeedSteeringYawAssistSpeed = Defaults->LowSpeedSteeringYawAssistSpeed;
    FrontSteeringGripMultiplier = Defaults->FrontSteeringGripMultiplier;
    RearSteeringGripMultiplier = Defaults->RearSteeringGripMultiplier;
    HighSpeedFrontGripBoost = Defaults->HighSpeedFrontGripBoost;
    HighSpeedSteeringAuthorityScale = Defaults->HighSpeedSteeringAuthorityScale;
    LateralGrip = Defaults->LateralGrip;
    MaxLateralGripForce = Defaults->MaxLateralGripForce;
    RollingResistance = Defaults->RollingResistance;
    MaxSpeedForward = Defaults->MaxSpeedForward;
    TireLongitudinalFriction = Defaults->TireLongitudinalFriction;
    TireLateralFriction = Defaults->TireLateralFriction;
    TireCorneringStiffness = Defaults->TireCorneringStiffness;
    TireSlipReferenceSpeed = Defaults->TireSlipReferenceSpeed;
    HighSpeedLateralGripScale = Defaults->HighSpeedLateralGripScale;
    HighSpeedLateralGripSpeed = Defaults->HighSpeedLateralGripSpeed;
    SteeringLateralGripReserve = Defaults->SteeringLateralGripReserve;
    DrivenFrontTorqueShare = Defaults->DrivenFrontTorqueShare;
    EngineBrakingForce = Defaults->EngineBrakingForce;
    HighSpeedSteeringAngleDegrees = Defaults->HighSpeedSteeringAngleDegrees;
    AckermannStrength = Defaults->AckermannStrength;
    AerodynamicDragCoefficient = Defaults->AerodynamicDragCoefficient;
    MaxAerodynamicDrag = Defaults->MaxAerodynamicDrag;
    GroundedDownforceCoefficient = Defaults->GroundedDownforceCoefficient;
    MaxGroundedDownforce = Defaults->MaxGroundedDownforce;
    MinimumDownforceSpeed = Defaults->MinimumDownforceSpeed;
    FrontDownforceCoefficient = Defaults->FrontDownforceCoefficient;
    MaxFrontDownforce = Defaults->MaxFrontDownforce;
    ThrottleFrontDownforce = Defaults->ThrottleFrontDownforce;
    ThrottleInputInterpSpeed = Defaults->ThrottleInputInterpSpeed;
    SteeringInputInterpSpeed = Defaults->SteeringInputInterpSpeed;
    SteeringInputRiseRate = Defaults->SteeringInputRiseRate;
    SteeringInputReturnRate = Defaults->SteeringInputReturnRate;
    SteeringInputSpeedDamping = Defaults->SteeringInputSpeedDamping;
    SteeringInputCurveExponent = Defaults->SteeringInputCurveExponent;
    RideHeightOffset = Defaults->RideHeightOffset;
    WheelHeightOffset = Defaults->WheelHeightOffset;
    FrontWheelHeightOffset = Defaults->FrontWheelHeightOffset;
    RearWheelHeightOffset = Defaults->RearWheelHeightOffset;
    WheelHeightOffsets = Defaults->WheelHeightOffsets;
    WheelMeshNames = Defaults->WheelMeshNames;
    StableRideHeightGroundBuffer = Defaults->StableRideHeightGroundBuffer;
    WheelVisualGroundContactBuffer = Defaults->WheelVisualGroundContactBuffer;
}

bool AVehiclePawn::ApplyVehicleTuningJsonObject(const TSharedPtr<FJsonObject>& JsonObject)
{
    if (!JsonObject.IsValid())
    {
        return false;
    }

    TSharedPtr<FJsonObject> TuningObject = JsonObject;
    const TSharedPtr<FJsonObject>* NestedObject = nullptr;
    if (JsonObject->TryGetObjectField(TEXT("VehicleTuning"), NestedObject) && NestedObject && NestedObject->IsValid())
    {
        TuningObject = *NestedObject;
    }
    else if (JsonObject->TryGetObjectField(TEXT("Vehicle"), NestedObject) && NestedObject && NestedObject->IsValid())
    {
        TuningObject = *NestedObject;
    }

    bool bAppliedAnyField = false;
    auto ReadFloat = [TuningObject, &bAppliedAnyField](const TCHAR* Key, float& Target, float MinValue, float MaxValue) -> bool
    {
        if (!TuningObject.IsValid())
        {
            return false;
        }

        double NumberValue = 0.0;
        if (!TuningObject->TryGetNumberField(Key, NumberValue))
        {
            return false;
        }

        Target = FMath::Clamp(static_cast<float>(NumberValue), MinValue, MaxValue);
        bAppliedAnyField = true;
        return true;
    };

    ReadFloat(TEXT("MaxSpeedForward"), MaxSpeedForward, 0.0f, VehicleTuningMaxSpeed);
    ReadFloat(TEXT("Speed"), MaxSpeedForward, 0.0f, VehicleTuningMaxSpeed);
    ReadFloat(TEXT("TopSpeed"), MaxSpeedForward, 0.0f, VehicleTuningMaxSpeed);

    ReadFloat(TEXT("EngineForce"), EngineForce, 0.0f, VehicleTuningMaxForce);
    ReadFloat(TEXT("AccelerationForce"), EngineForce, 0.0f, VehicleTuningMaxForce);
    ReadFloat(TEXT("ForwardAccelerationForce"), EngineForce, 0.0f, VehicleTuningMaxForce);
    ReadFloat(TEXT("ReverseForce"), ReverseForce, 0.0f, VehicleTuningMaxForce);
    ReadFloat(TEXT("ReverseAccelerationForce"), ReverseForce, 0.0f, VehicleTuningMaxForce);
    ReadFloat(TEXT("BrakeForce"), BrakeForce, 0.0f, VehicleTuningMaxForce);
    ReadFloat(TEXT("EngineBrakingForce"), EngineBrakingForce, 0.0f, VehicleTuningMaxForce);
    ReadFloat(TEXT("RollingResistance"), RollingResistance, 0.0f, 0.20f);

    ReadFloat(TEXT("MaxSteeringAngleDegrees"), MaxSteeringAngleDegrees, 1.0f, 55.0f);
    ReadFloat(TEXT("SteeringAngle"), MaxSteeringAngleDegrees, 1.0f, 55.0f);
    ReadFloat(TEXT("HighSpeedSteeringAngleDegrees"), HighSpeedSteeringAngleDegrees, 1.0f, 45.0f);
    ReadFloat(TEXT("SteeringYawRateAssist"), SteeringYawRateAssist, 0.0f, VehicleTuningMaxTorque);
    ReadFloat(TEXT("HighSpeedYawAssistStrength"), HighSpeedYawAssistStrength, 0.0f, VehicleTuningMaxTorque);
    ReadFloat(TEXT("HighSpeedYawAssistStartSpeed"), HighSpeedYawAssistStartSpeed, 100.0f, VehicleTuningMaxSpeed);
    ReadFloat(TEXT("RotationForce"), SteeringYawRateAssist, 0.0f, VehicleTuningMaxTorque);
    ReadFloat(TEXT("TurnAssistTorque"), SteeringYawRateAssist, 0.0f, VehicleTuningMaxTorque);
    ReadFloat(TEXT("SteeringYawDamping"), SteeringYawDamping, 0.0f, VehicleTuningMaxTorque);
    ReadFloat(TEXT("MaxSteeringAssistTorque"), MaxSteeringAssistTorque, 0.0f, VehicleTuningMaxTorque);
    ReadFloat(TEXT("LowSpeedSteeringYawAssistSpeed"), LowSpeedSteeringYawAssistSpeed, 0.0f, VehicleTuningMaxSpeed);
    ReadFloat(TEXT("MinSteeringSpeedFactor"), MinSteeringSpeedFactor, 0.0f, 1.0f);
    ReadFloat(TEXT("MaxSteeringSpeedFactor"), MaxSteeringSpeedFactor, 0.0f, 1.0f);
    ReadFloat(TEXT("SteeringSpeedForFullAssist"), SteeringSpeedForFullAssist, 1.0f, VehicleTuningMaxSpeed);
    ReadFloat(TEXT("AckermannStrength"), AckermannStrength, 0.0f, 1.0f);
    ReadFloat(TEXT("FrontSteeringGripMultiplier"), FrontSteeringGripMultiplier, 0.1f, VehicleTuningMaxGrip);
    ReadFloat(TEXT("RearSteeringGripMultiplier"), RearSteeringGripMultiplier, 0.1f, VehicleTuningMaxGrip);
    ReadFloat(TEXT("HighSpeedFrontGripBoost"), HighSpeedFrontGripBoost, 1.0f, 2.0f);
    ReadFloat(TEXT("HighSpeedSteeringAuthorityScale"), HighSpeedSteeringAuthorityScale, 1.0f, 2.0f);

    ReadFloat(TEXT("LateralGrip"), LateralGrip, 0.1f, VehicleTuningMaxGrip);
    ReadFloat(TEXT("TireLateralForceScale"), TireLateralForceScale, 0.0f, 1.0f);
    ReadFloat(TEXT("MaxLateralGripForce"), MaxLateralGripForce, 0.0f, VehicleTuningMaxForce);
    ReadFloat(TEXT("TireLongitudinalFriction"), TireLongitudinalFriction, 0.1f, VehicleTuningMaxGrip);
    ReadFloat(TEXT("TireLateralFriction"), TireLateralFriction, 0.1f, VehicleTuningMaxGrip);
    ReadFloat(TEXT("TireCorneringStiffness"), TireCorneringStiffness, 0.1f, 100.0f);
    ReadFloat(TEXT("TireSlipReferenceSpeed"), TireSlipReferenceSpeed, 1.0f, VehicleTuningMaxSpeed);
    ReadFloat(TEXT("HighSpeedLateralGripScale"), HighSpeedLateralGripScale, 0.1f, 1.0f);
    ReadFloat(TEXT("HighSpeedLateralGripSpeed"), HighSpeedLateralGripSpeed, 100.0f, VehicleTuningMaxSpeed);
    ReadFloat(TEXT("SteeringLateralGripReserve"), SteeringLateralGripReserve, 0.0f, 0.90f);
    ReadFloat(TEXT("DrivenFrontTorqueShare"), DrivenFrontTorqueShare, 0.0f, 1.0f);

    ReadFloat(TEXT("AerodynamicDragCoefficient"), AerodynamicDragCoefficient, 0.0f, 1.0f);
    ReadFloat(TEXT("MaxAerodynamicDrag"), MaxAerodynamicDrag, 0.0f, VehicleTuningMaxForce);
    ReadFloat(TEXT("GroundedDownforceCoefficient"), GroundedDownforceCoefficient, 0.0f, 1.0f);
    ReadFloat(TEXT("MaxGroundedDownforce"), MaxGroundedDownforce, 0.0f, VehicleTuningMaxForce);
    ReadFloat(TEXT("MinimumDownforceSpeed"), MinimumDownforceSpeed, 0.0f, VehicleTuningMaxSpeed);
    ReadFloat(TEXT("FrontDownforceCoefficient"), FrontDownforceCoefficient, 0.0f, 1.0f);
    ReadFloat(TEXT("MaxFrontDownforce"), MaxFrontDownforce, 0.0f, VehicleTuningMaxForce);
    ReadFloat(TEXT("ThrottleFrontDownforce"), ThrottleFrontDownforce, 0.0f, VehicleTuningMaxForce);

    ReadFloat(TEXT("ThrottleInputInterpSpeed"), ThrottleInputInterpSpeed, 0.1f, 60.0f);
    ReadFloat(TEXT("SteeringInputInterpSpeed"), SteeringInputInterpSpeed, 0.1f, 60.0f);
    ReadFloat(TEXT("SteeringInputRiseRate"), SteeringInputRiseRate, 0.1f, 30.0f);
    ReadFloat(TEXT("SteeringInputReturnRate"), SteeringInputReturnRate, 0.1f, 60.0f);
    ReadFloat(TEXT("SteeringInputSpeedDamping"), SteeringInputSpeedDamping, 0.0f, 1.0f);
    ReadFloat(TEXT("SteeringInputCurveExponent"), SteeringInputCurveExponent, 1.0f, 3.0f);

    ReadFloat(TEXT("RideHeightOffset"), RideHeightOffset, -30.0f, 30.0f);
    ReadFloat(TEXT("GroundClearanceOffset"), RideHeightOffset, -30.0f, 30.0f);
    ReadFloat(TEXT("WheelHeightOffset"), WheelHeightOffset, -VehicleWheelHeightOffsetLimit, VehicleWheelHeightOffsetLimit);
    ReadFloat(TEXT("WheelZOffset"), WheelHeightOffset, -VehicleWheelHeightOffsetLimit, VehicleWheelHeightOffsetLimit);
    ReadFloat(TEXT("WheelMountHeightOffset"), WheelHeightOffset, -VehicleWheelHeightOffsetLimit, VehicleWheelHeightOffsetLimit);
    ReadFloat(TEXT("FrontWheelHeightOffset"), FrontWheelHeightOffset, -VehicleWheelHeightOffsetLimit, VehicleWheelHeightOffsetLimit);
    ReadFloat(TEXT("RearWheelHeightOffset"), RearWheelHeightOffset, -VehicleWheelHeightOffsetLimit, VehicleWheelHeightOffsetLimit);
    ReadFloat(TEXT("StableRideHeightGroundBuffer"), StableRideHeightGroundBuffer, 0.0f, 6.0f);
    ReadFloat(TEXT("WheelVisualGroundContactBuffer"), WheelVisualGroundContactBuffer, 0.0f, 6.0f);

    const TArray<TSharedPtr<FJsonValue>>* WheelHeightArray = nullptr;
    if (TuningObject->TryGetArrayField(TEXT("WheelHeightOffsets"), WheelHeightArray) && WheelHeightArray)
    {
        TArray<float> ParsedOffsets;
        ParsedOffsets.Reserve(FMath::Min(WheelHeightArray->Num(), 32));
        for (int32 Index = 0; Index < WheelHeightArray->Num() && Index < 32; ++Index)
        {
            double NumberValue = 0.0;
            if (!(*WheelHeightArray)[Index].IsValid() || !(*WheelHeightArray)[Index]->TryGetNumber(NumberValue))
            {
                UE_LOG(LogTemp, Warning, TEXT("VehiclePawn: WheelHeightOffsets[%d] is not numeric and was ignored."), Index);
                ParsedOffsets.Add(0.0f);
                continue;
            }
            ParsedOffsets.Add(FMath::Clamp(
                static_cast<float>(NumberValue),
                -VehicleWheelHeightOffsetLimit,
                VehicleWheelHeightOffsetLimit));
        }
        WheelHeightOffsets = MoveTemp(ParsedOffsets);
        bAppliedAnyField = true;
    }

    const TArray<TSharedPtr<FJsonValue>>* WheelNameArray = nullptr;
    if (TuningObject->TryGetArrayField(TEXT("WheelMeshNames"), WheelNameArray) && WheelNameArray)
    {
        TArray<FString> ParsedNames;
        ParsedNames.Reserve(FMath::Min(WheelNameArray->Num(), 32));
        for (int32 Index = 0; Index < WheelNameArray->Num() && Index < 32; ++Index)
        {
            if (!(*WheelNameArray)[Index].IsValid() || (*WheelNameArray)[Index]->Type != EJson::String)
            {
                UE_LOG(LogTemp, Warning, TEXT("VehiclePawn: WheelMeshNames[%d] is not a string and was ignored."), Index);
                continue;
            }

            FString Name = (*WheelNameArray)[Index]->AsString().TrimStartAndEnd();
            if (Name.IsEmpty() || Name.Len() > 256)
            {
                UE_LOG(LogTemp, Warning, TEXT("VehiclePawn: WheelMeshNames[%d] is empty or too long and was ignored."), Index);
                continue;
            }
            ParsedNames.AddUnique(MoveTemp(Name));
        }
        WheelMeshNames = MoveTemp(ParsedNames);
        bAppliedAnyField = true;
    }

    MaxSteeringSpeedFactor = FMath::Max(MaxSteeringSpeedFactor, MinSteeringSpeedFactor);
    HighSpeedSteeringAngleDegrees = FMath::Min(HighSpeedSteeringAngleDegrees, MaxSteeringAngleDegrees);

    return bAppliedAnyField;
}

bool AVehiclePawn::LoadVehicleTuningJson(const FString& JsonPath)
{
    if (JsonPath.IsEmpty())
    {
        return false;
    }

    const FString FullJsonPath = FPaths::ConvertRelativePathToFull(JsonPath);
    if (!IFileManager::Get().FileExists(*FullJsonPath))
    {
        UE_LOG(LogTemp, Warning, TEXT("VehiclePawn: tuning JSON does not exist: %s"), *FullJsonPath);
        return false;
    }

    const TSharedPtr<FJsonObject> JsonObject = UFileFunctionLibrary::FromJson(FullJsonPath);
    if (!JsonObject.IsValid())
    {
        return false;
    }

    ResetVehicleTuningToClassDefaults();
    const bool bApplied = ApplyVehicleTuningJsonObject(JsonObject);
    if (bApplied)
    {
        UE_LOG(LogTemp, Log, TEXT("VehiclePawn: loaded tuning JSON %s"), *FullJsonPath);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("VehiclePawn: tuning JSON contained no supported fields: %s"), *FullJsonPath);
    }
    return bApplied;
}

bool AVehiclePawn::SaveVehicleTuningJsonTemplate(const FString& JsonPath) const
{
    if (JsonPath.IsEmpty())
    {
        return false;
    }

    const FString FullJsonPath = FPaths::ConvertRelativePathToFull(JsonPath);
    TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
    RootObject->SetStringField(JSON_VERSION_FIELD, JSON_SCHEMA_VERSION);
    RootObject->SetStringField(TEXT("Schema"), TEXT("glTFSimulator.VehicleTuning.v3"));
    RootObject->SetStringField(TEXT("AssetType"), TEXT("Vehicle"));
    RootObject->SetStringField(TEXT("DisplayName"), ObjectName.IsEmpty() ? BaseName : ObjectName);
    RootObject->SetStringField(TEXT("Notes"), TEXT("User-authored read-only vehicle settings. Runtime geometry/hash caches are stored in the sibling .scz file. Positive RideHeightOffset raises the chassis; negative lowers it."));

    RootObject->SetNumberField(TEXT("MaxSpeedForward"), MaxSpeedForward);
    RootObject->SetNumberField(TEXT("EngineForce"), EngineForce);
    RootObject->SetNumberField(TEXT("ReverseForce"), ReverseForce);
    RootObject->SetNumberField(TEXT("BrakeForce"), BrakeForce);
    RootObject->SetNumberField(TEXT("EngineBrakingForce"), EngineBrakingForce);
    RootObject->SetNumberField(TEXT("RollingResistance"), RollingResistance);

    RootObject->SetNumberField(TEXT("RideHeightOffset"), RideHeightOffset);
    RootObject->SetNumberField(TEXT("WheelHeightOffset"), WheelHeightOffset);
    RootObject->SetNumberField(TEXT("FrontWheelHeightOffset"), FrontWheelHeightOffset);
    RootObject->SetNumberField(TEXT("RearWheelHeightOffset"), RearWheelHeightOffset);
    TArray<TSharedPtr<FJsonValue>> WheelHeightArray;
    const int32 TemplateWheelCount = FMath::Max(WheelOffsets.Num(), WheelHeightOffsets.Num());
    WheelHeightArray.Reserve(TemplateWheelCount);
    for (int32 WheelIndex = 0; WheelIndex < TemplateWheelCount; ++WheelIndex)
    {
        const float PerWheelOffset = WheelHeightOffsets.IsValidIndex(WheelIndex) ? WheelHeightOffsets[WheelIndex] : 0.0f;
        WheelHeightArray.Add(MakeShared<FJsonValueNumber>(PerWheelOffset));
    }
    RootObject->SetArrayField(TEXT("WheelHeightOffsets"), WheelHeightArray);

    TArray<TSharedPtr<FJsonValue>> WheelNameValues;
    WheelNameValues.Reserve(WheelMeshNames.Num());
    for (const FString& WheelMeshName : WheelMeshNames)
    {
        if (!WheelMeshName.IsEmpty())
        {
            WheelNameValues.Add(MakeShared<FJsonValueString>(WheelMeshName));
        }
    }
    RootObject->SetArrayField(TEXT("WheelMeshNames"), WheelNameValues);
    RootObject->SetNumberField(TEXT("StableRideHeightGroundBuffer"), StableRideHeightGroundBuffer);
    RootObject->SetNumberField(TEXT("WheelVisualGroundContactBuffer"), WheelVisualGroundContactBuffer);

    RootObject->SetNumberField(TEXT("MaxSteeringAngleDegrees"), MaxSteeringAngleDegrees);
    RootObject->SetNumberField(TEXT("HighSpeedSteeringAngleDegrees"), HighSpeedSteeringAngleDegrees);
    RootObject->SetNumberField(TEXT("SteeringYawRateAssist"), SteeringYawRateAssist);
    RootObject->SetNumberField(TEXT("HighSpeedYawAssistStrength"), HighSpeedYawAssistStrength);
    RootObject->SetNumberField(TEXT("HighSpeedYawAssistStartSpeed"), HighSpeedYawAssistStartSpeed);
    RootObject->SetNumberField(TEXT("SteeringYawDamping"), SteeringYawDamping);
    RootObject->SetNumberField(TEXT("MaxSteeringAssistTorque"), MaxSteeringAssistTorque);
    RootObject->SetNumberField(TEXT("LowSpeedSteeringYawAssistSpeed"), LowSpeedSteeringYawAssistSpeed);
    RootObject->SetNumberField(TEXT("SteeringSpeedForFullAssist"), SteeringSpeedForFullAssist);
    RootObject->SetNumberField(TEXT("AckermannStrength"), AckermannStrength);
    RootObject->SetNumberField(TEXT("FrontSteeringGripMultiplier"), FrontSteeringGripMultiplier);
    RootObject->SetNumberField(TEXT("RearSteeringGripMultiplier"), RearSteeringGripMultiplier);
    RootObject->SetNumberField(TEXT("HighSpeedFrontGripBoost"), HighSpeedFrontGripBoost);
    RootObject->SetNumberField(TEXT("HighSpeedSteeringAuthorityScale"), HighSpeedSteeringAuthorityScale);

    RootObject->SetNumberField(TEXT("LateralGrip"), LateralGrip);
    RootObject->SetNumberField(TEXT("TireLateralForceScale"), TireLateralForceScale);
    RootObject->SetNumberField(TEXT("MaxLateralGripForce"), MaxLateralGripForce);
    RootObject->SetNumberField(TEXT("TireLongitudinalFriction"), TireLongitudinalFriction);
    RootObject->SetNumberField(TEXT("TireLateralFriction"), TireLateralFriction);
    RootObject->SetNumberField(TEXT("TireCorneringStiffness"), TireCorneringStiffness);
    RootObject->SetNumberField(TEXT("TireSlipReferenceSpeed"), TireSlipReferenceSpeed);
    RootObject->SetNumberField(TEXT("HighSpeedLateralGripScale"), HighSpeedLateralGripScale);
    RootObject->SetNumberField(TEXT("HighSpeedLateralGripSpeed"), HighSpeedLateralGripSpeed);
    RootObject->SetNumberField(TEXT("SteeringLateralGripReserve"), SteeringLateralGripReserve);
    RootObject->SetNumberField(TEXT("DrivenFrontTorqueShare"), DrivenFrontTorqueShare);

    RootObject->SetNumberField(TEXT("AerodynamicDragCoefficient"), AerodynamicDragCoefficient);
    RootObject->SetNumberField(TEXT("MaxAerodynamicDrag"), MaxAerodynamicDrag);
    RootObject->SetNumberField(TEXT("GroundedDownforceCoefficient"), GroundedDownforceCoefficient);
    RootObject->SetNumberField(TEXT("MaxGroundedDownforce"), MaxGroundedDownforce);
    RootObject->SetNumberField(TEXT("MinimumDownforceSpeed"), MinimumDownforceSpeed);
    RootObject->SetNumberField(TEXT("FrontDownforceCoefficient"), FrontDownforceCoefficient);
    RootObject->SetNumberField(TEXT("MaxFrontDownforce"), MaxFrontDownforce);
    RootObject->SetNumberField(TEXT("ThrottleFrontDownforce"), ThrottleFrontDownforce);

    RootObject->SetNumberField(TEXT("ThrottleInputInterpSpeed"), ThrottleInputInterpSpeed);
    RootObject->SetNumberField(TEXT("SteeringInputInterpSpeed"), SteeringInputInterpSpeed);
    RootObject->SetNumberField(TEXT("SteeringInputRiseRate"), SteeringInputRiseRate);
    RootObject->SetNumberField(TEXT("SteeringInputReturnRate"), SteeringInputReturnRate);
    RootObject->SetNumberField(TEXT("SteeringInputSpeedDamping"), SteeringInputSpeedDamping);
    RootObject->SetNumberField(TEXT("SteeringInputCurveExponent"), SteeringInputCurveExponent);

    const FSafeFileWriteResult Result = FSafeFileIO::CreateJsonIfMissingBlocking(
        RootObject,
        FullJsonPath,
        64ll * 1024ll * 1024ll);
    if (!Result.IsSuccess())
    {
        UE_LOG(LogTemp, Warning, TEXT("VehiclePawn: could not create missing read-only tuning template. Path=%s Reason=%s"),
            *FullJsonPath,
            *Result.Error);
    }
    return Result.IsSuccess();
}

UStaticMesh* AVehiclePawn::LoadMeshByIndex(int32 MeshIndex)
{
    if (MeshIndex < 0)
    {
        return nullptr;
    }

    if (TObjectPtr<UStaticMesh>* Existing = MeshCache.Find(MeshIndex))
    {
        return Existing->Get();
    }

    UInstancedEntitySubsystem* InstancedEntities = UInstancedEntitySubsystem::Get(this);
    if (InstancedEntities)
    {
        if (UStaticMesh* SharedMesh = InstancedEntities->FindSharedMesh(SourceFilePath, MeshIndex))
        {
            MeshCache.Add(MeshIndex, SharedMesh);
            return SharedMesh;
        }
    }

    if (!IsValid(GltfAsset) || MeshIndex >= GltfAsset->GetNumMeshes())
    {
        return nullptr;
    }

    FglTFRuntimeStaticMeshConfig MeshConfig;
    MeshConfig.Outer = InstancedEntities ? InstancedEntities->GetRuntimeMeshOuter() : this;
    MeshConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    MeshConfig.MaterialsConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    MeshConfig.MaterialsConfig.bGeneratesMipMaps = false;
    const int32 TextureDimensionLimit = UGameSettings::ResolveMaxTextureResolution(this);
    MeshConfig.MaterialsConfig.ImagesConfig.MaxWidth = TextureDimensionLimit;
    MeshConfig.MaterialsConfig.ImagesConfig.MaxHeight = TextureDimensionLimit;
    MeshConfig.MaterialsConfig.ImagesConfig.bCompressMips = false;
    MeshConfig.MaterialsConfig.ImagesConfig.bStreaming = false;
    MeshConfig.MaterialsConfig.bLoadMipMaps = false;
    const TMap<EglTFRuntimeMaterialType, UMaterialInterface*> LitOverrides =
        glTFMaterialOverrideUtils::BuildOverrideMap(MaterialAssets);
    if (LitOverrides.Num() > 0)
    {
        MeshConfig.MaterialsConfig.UberMaterialsOverrideMap = LitOverrides;
        MeshConfig.MaterialsConfig.UnlitOverrideMap = LitOverrides;
    }
    glTFMaterialOverrideUtils::ApplyNamedOverrides(MaterialAssets, MeshConfig.MaterialsConfig);
    MeshConfig.bAllowCPUAccess = false;
    MeshConfig.bBuildLumenCards = true;
    MeshConfig.bBuildSimpleCollision = false;
    MeshConfig.bBuildComplexCollision = false;
    MeshConfig.bBuildNavCollision = false;
    MeshConfig.CollisionComplexity = ECollisionTraceFlag::CTF_UseDefault;

    UStaticMesh* Mesh = GltfAsset->LoadStaticMesh(MeshIndex, MeshConfig);
    if (IsValid(Mesh))
    {
        MeshCache.Add(MeshIndex, Mesh);
    }
    return Mesh;
}


bool AVehiclePawn::LoadVehicleModel(const FString& InFilePath, const FString& InObjectName)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("AVehiclePawn::LoadVehicleModel must run on the game thread")))
    {
        return false;
    }

    const FString NormalizedPath = GlbValidation::NormalizePath(InFilePath);
    if (NormalizedPath.IsEmpty() || !IFileManager::Get().FileExists(*NormalizedPath))
    {
        UE_LOG(LogTemp, Warning, TEXT("VehiclePawn: source file is missing; load skipped. Path=%s"), *NormalizedPath);
        return false;
    }

    FString ValidationReason;
    if (!GlbValidation::ValidateRuntimeModelFile(NormalizedPath, ValidationReason))
    {
        UE_LOG(LogTemp, Warning, TEXT("VehiclePawn: invalid glTF model skipped. Path=%s Reason=%s"), *NormalizedPath, *ValidationReason);
        return false;
    }

    ClearLoadedVehicleModel();
    SourceFilePath = NormalizedPath;
    BaseName = FPaths::GetBaseFilename(SourceFilePath);
    ObjectName = InObjectName.IsEmpty() ? BaseName : InObjectName;
    ResetVehicleTuningToClassDefaults();

    const FString TuningJsonPath = GetVehicleTuningJsonPath();
    const bool bHasTuningJson = !TuningJsonPath.IsEmpty() && IFileManager::Get().FileExists(*TuningJsonPath);
    if (bHasTuningJson)
    {
        LoadVehicleTuningJson(TuningJsonPath);
    }

    UInstancedEntitySubsystem* InstancedEntities = UInstancedEntitySubsystem::Get(this);
    if (!InstancedEntities)
    {
        UE_LOG(LogTemp, Warning, TEXT("VehiclePawn: instanced entity subsystem is unavailable."));
        ClearLoadedVehicleModel();
        return false;
    }

    auto BuildRegistrationOptions = [this](const bool bStoreAsVehicleTemplate)
    {
        FInstancedEntityRegistrationOptions Options;
        Options.bDynamic = !UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this) || HasAuthority();
        Options.bAllowPhysicsDistanceDeactivation = Options.bDynamic;
        Options.bAlwaysRelevant = IsOccupied();
        Options.bStoreAsVehicleTemplate = bStoreAsVehicleTemplate;
        Options.InterpolationSpeed = 32.0f;
        Options.TeleportDistance = 2500.0f;
        Options.MidDistance = 40000.0f;
        Options.PhysicsSuspendDistance = 80000.0f;
        Options.EndCullDistance = 150000.0f;
        Options.MidUpdateInterval = 1.0f / 30.0f;
        Options.FarUpdateInterval = 0.20f;
        return Options;
    };

    auto PrepareLoadedVehicleState = [this]()
    {
        if (LoadedBodyVisualBounds.IsValid && IsValid(Body))
        {
            // LoadedBodyVisualBounds contains only meshes that were not classified as wheels. The
            // model was rebased to this bounds center, so a centered box can use its true half extent
            // rather than the old max(abs(min), abs(max)) approximation that could include the pivot gap.
            const FVector VisualBodyExtent = LoadedBodyVisualBounds.GetExtent().GetAbs();
            BodyExtent.X = FMath::Clamp(VisualBodyExtent.X * 0.96f, 20.0f, 1200.0f);
            BodyExtent.Y = FMath::Clamp(VisualBodyExtent.Y * 0.94f, 20.0f, 800.0f);
            float CollisionHalfHeight = FMath::Clamp(VisualBodyExtent.Z * 0.90f, 8.0f, 600.0f);

            // Keep the chassis collider above every authored tire bottom. This is calculated from the
            // per-wheel radius and configured wheel offsets, so an oversized wheel AABB cannot lift all
            // other wheels and the box never reaches the road before the tires do.
            float HighestWheelBottom = -TNumericLimits<float>::Max();
            for (int32 WheelIndex = 0; WheelIndex < WheelOffsets.Num(); ++WheelIndex)
            {
                const float WheelBottom = WheelOffsets[WheelIndex].Z
                    - GetTargetWheelSpringLength(WheelIndex)
                    - GetEffectiveWheelRadius(WheelIndex);
                if (FMath::IsFinite(WheelBottom))
                {
                    // The wheel bottom closest to the chassis is the strict limit. Using the lowest
                    // tire bottom allowed the box to extend below a smaller/offset wheel and touch
                    // the road before that wheel, making the rendered vehicle appear to float.
                    HighestWheelBottom = FMath::Max(HighestWheelBottom, WheelBottom);
                }
            }
            if (HighestWheelBottom > -TNumericLimits<float>::Max())
            {
                const float MaximumCollisionHalfHeight = FMath::Max(8.0f, -HighestWheelBottom - 1.0f);
                CollisionHalfHeight = FMath::Min(CollisionHalfHeight, MaximumCollisionHalfHeight);
            }

            BodyExtent.Z = FMath::Clamp(CollisionHalfHeight, 8.0f, 600.0f);
            Body->SetBoxExtent(BodyExtent, true);
        }

        WheelSpinDegrees.Init(0.0f, WheelOffsets.Num());
        WheelSpringLengths.SetNum(WheelOffsets.Num());
        WheelVisualSpringLengths.SetNum(WheelOffsets.Num());
        for (int32 WheelIndex = 0; WheelIndex < WheelOffsets.Num(); ++WheelIndex)
        {
            const float TargetSpringLength = GetTargetWheelSpringLength(WheelIndex);
            WheelSpringLengths[WheelIndex] = TargetSpringLength;
            WheelVisualSpringLengths[WheelIndex] = TargetSpringLength;
        }
        WheelSuspensionForces.Init(0.0f, WheelOffsets.Num());
        WheelLateralForces.Init(0.0f, WheelOffsets.Num());
        WheelGrounded.Init(false, WheelOffsets.Num());

        bVehicleModelLoaded = true;
        ActivateVehicleAfterModelLoad();
        if (!UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this) || HasAuthority())
        {
            ResetVehiclePoseAboveGround();
        }
    };

    auto FinishSuccessfulModelLoad = [this, bHasTuningJson, TuningJsonPath]()
    {
        UpdateWheelVisuals(0.0f);

        if (!bHasTuningJson && !TuningJsonPath.IsEmpty())
        {
            // Generate a documented v3 template only after the authored wheel count is known.
            SaveVehicleTuningJsonTemplate(TuningJsonPath);
        }

        if (HasAuthority())
        {
            ReplicatedSourceFilePath = SourceFilePath;
            ReplicatedObjectName = ObjectName;
            ForceNetUpdate();
        }
    };

    // Reuse the shared vehicle prefab while at least one instance still owns the resource. This
    // avoids reopening and reparsing the glTF file for every vehicle while keeping JSON tuning per entity.
    FInstancedVehicleTemplateData CachedVehicleTemplate;
    if (InstancedEntities->GetVehicleTemplateData(SourceFilePath, CachedVehicleTemplate))
    {
        LoadedWheelRenderPartIndices = CachedVehicleTemplate.WheelPartIndices;
        LoadedWheelBaseRotations = CachedVehicleTemplate.WheelBaseRotations;
        LoadedWheelBaseScales = CachedVehicleTemplate.WheelBaseScales;
        LoadedWheelVisualCenterOffsets = CachedVehicleTemplate.WheelVisualCenterOffsets;
        WheelOffsets = CachedVehicleTemplate.AuthoredWheelOffsets;
        WheelTargetSpringLengths = CachedVehicleTemplate.WheelTargetSpringLengths;
        LoadedWheelGroundRadii = CachedVehicleTemplate.WheelGroundRadii;
        LoadedBodyVisualBounds = CachedVehicleTemplate.BodyVisualBounds;
        LoadedWheelVisualRestBounds = CachedVehicleTemplate.WheelVisualRestBounds;
        RuntimeWheelRadius = CachedVehicleTemplate.RuntimeWheelRadius;

        ApplyConfiguredWheelHeightOffsets();
        PrepareLoadedVehicleState();

        const FInstancedEntityRegistrationOptions RegistrationOptions = BuildRegistrationOptions(false);
        InstancedRenderRegistrationId = InstancedEntities->RegisterVehicleEntityFromTemplate(
            SourceFilePath,
            this,
            Body,
            RegistrationOptions);
        if (InstancedRenderRegistrationId == INDEX_NONE)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("VehiclePawn: failed to register cached vehicle prefab for %s"),
                *SourceFilePath);
            ClearLoadedVehicleModel();
            return false;
        }

        FinishSuccessfulModelLoad();
        return true;
    }

    FglTFRuntimeConfig LoaderConfig;
    LoaderConfig.bAllowExternalFiles = true;
    GltfAsset = UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(SourceFilePath, false, LoaderConfig);
    if (!IsValid(GltfAsset))
    {
        UE_LOG(LogTemp, Warning, TEXT("VehiclePawn: failed to load vehicle model %s"), *SourceFilePath);
        ClearLoadedVehicleModel();
        return false;
    }

    const TArray<FglTFRuntimeNode> Nodes = GltfAsset->GetNodes();
    if (Nodes.Num() > MaxRuntimeVehicleNodeCount)
    {
        UE_LOG(LogTemp, Warning, TEXT("VehiclePawn: glTF node count exceeds the runtime safety limit. Path=%s Nodes=%d"),
            *SourceFilePath, Nodes.Num());
        ClearLoadedVehicleModel();
        return false;
    }

    const int32 MeshCount = GltfAsset->GetNumMeshes();
    TMap<int32, FglTFRuntimeNode> NodeMap;
    for (const FglTFRuntimeNode& Node : Nodes)
    {
        if (Node.Index >= 0 && Node.Index < Nodes.Num() && !Node.Transform.ContainsNaN())
        {
            NodeMap.Add(Node.Index, Node);
        }
    }

    TMap<int32, FString> MeshNamesByIndex;
    if (GltfAsset->GetParser().IsValid())
    {
        const TArray<TSharedRef<FJsonObject>> MeshObjects = GltfAsset->GetParser()->GetMeshes();
        for (int32 MeshIndex = 0; MeshIndex < MeshObjects.Num(); ++MeshIndex)
        {
            FString MeshName;
            if (MeshObjects[MeshIndex]->TryGetStringField(TEXT("name"), MeshName))
            {
                MeshNamesByIndex.Add(MeshIndex, MeshName);
            }
        }
    }

    TArray<FInstancedEntityMeshPart> RenderParts;
    TArray<FVehicleWheelVisual> WheelNodes;
    int32 BodyRenderPartCount = 0;

    for (const FglTFRuntimeNode& Node : Nodes)
    {
        if (Node.Index < 0 || !NodeMap.Contains(Node.Index)
            || Node.MeshIndex < 0 || Node.MeshIndex >= MeshCount || Node.Transform.ContainsNaN())
        {
            continue;
        }

        UStaticMesh* Mesh = LoadMeshByIndex(Node.MeshIndex);
        if (!IsValid(Mesh))
        {
            continue;
        }

        const FTransform NodeWorldTransform = GetVehicleNodeWorldTransform(NodeMap, Node);
        if (NodeWorldTransform.ContainsNaN())
        {
            continue;
        }

        const FString MeshName = MeshNamesByIndex.FindRef(Node.MeshIndex);
        if (IsWheelMeshName(Node.Name) || IsWheelMeshName(MeshName))
        {
            FVehicleWheelVisual& WheelVisual = WheelNodes.AddDefaulted_GetRef();
            WheelVisual.Node = Node;
            WheelVisual.Transform = NodeWorldTransform;
            continue;
        }

        FInstancedEntityMeshPart& RenderPart = RenderParts.AddDefaulted_GetRef();
        RenderPart.MeshKey = Node.MeshIndex;
        RenderPart.Mesh = Mesh;
        RenderPart.LocalTransform = NodeWorldTransform;
        ++BodyRenderPartCount;

        const FBox BodyMeshBounds = TransformVehicleBounds(Mesh->GetBoundingBox(), NodeWorldTransform);
        if (BodyMeshBounds.IsValid)
        {
            LoadedBodyVisualBounds += BodyMeshBounds.Min;
            LoadedBodyVisualBounds += BodyMeshBounds.Max;
        }
    }

    // glTF vehicle roots may use any scene pivot. Center the physics proxy on the body-only bounds
    // in all three axes before deriving wheel mounts. Wheel meshes have already been separated, so
    // they can never enlarge the chassis hitbox. Applying the same offset to body and wheels preserves
    // the authored relationship while keeping the box collider aligned to the visible chassis.
    if (LoadedBodyVisualBounds.IsValid)
    {
        const FVector AuthoredBodyCenter = LoadedBodyVisualBounds.GetCenter();
        if (!AuthoredBodyCenter.ContainsNaN())
        {
            const FVector ModelToPhysicsOffset = -AuthoredBodyCenter;
            for (FInstancedEntityMeshPart& RenderPart : RenderParts)
            {
                RenderPart.LocalTransform.AddToTranslation(ModelToPhysicsOffset);
            }
            for (FVehicleWheelVisual& WheelVisual : WheelNodes)
            {
                WheelVisual.Transform.AddToTranslation(ModelToPhysicsOffset);
            }
            LoadedBodyVisualBounds.Min += ModelToPhysicsOffset;
            LoadedBodyVisualBounds.Max += ModelToPhysicsOffset;
        }
    }

    WheelNodes.Sort([](const FVehicleWheelVisual& A, const FVehicleWheelVisual& B)
    {
        const FVector AL = A.Transform.GetLocation();
        const FVector BL = B.Transform.GetLocation();
        if (!FMath::IsNearlyEqual(AL.X, BL.X, 1.0f))
        {
            return AL.X > BL.X;
        }
        return AL.Y > BL.Y;
    });

    for (const FVehicleWheelVisual& WheelNode : WheelNodes)
    {
        UStaticMesh* Mesh = LoadMeshByIndex(WheelNode.Node.MeshIndex);
        if (!IsValid(Mesh))
        {
            continue;
        }

        const int32 RenderPartIndex = RenderParts.Num();
        FInstancedEntityMeshPart& RenderPart = RenderParts.AddDefaulted_GetRef();
        RenderPart.MeshKey = WheelNode.Node.MeshIndex;
        RenderPart.Mesh = Mesh;
        RenderPart.LocalTransform = WheelNode.Transform;
        LoadedWheelRenderPartIndices.Add(RenderPartIndex);
        LoadedWheelBaseRotations.Add(WheelNode.Transform.GetRotation().GetNormalized());
        LoadedWheelBaseScales.Add(WheelNode.Transform.GetScale3D());

        const FBox MeshBounds = Mesh->GetBoundingBox();
        const FVector MeshCenterOffset = MeshBounds.IsValid ? MeshBounds.GetCenter() : FVector::ZeroVector;
        LoadedWheelVisualCenterOffsets.Add(MeshCenterOffset);

        const FVector AuthoredWheelCenter = WheelNode.Transform.TransformPosition(MeshCenterOffset);
        const FBox WheelBodyBounds = TransformVehicleBounds(MeshBounds, WheelNode.Transform);
        float GroundRadius = FMath::Max(1.0f, WheelRadius);
        if (WheelBodyBounds.IsValid)
        {
            LoadedWheelVisualRestBounds += WheelBodyBounds.Min;
            LoadedWheelVisualRestBounds += WheelBodyBounds.Max;
            const float CenterToBottom = AuthoredWheelCenter.Z - WheelBodyBounds.Min.Z;
            if (FMath::IsFinite(CenterToBottom) && CenterToBottom > 0.0f)
            {
                GroundRadius = FMath::Max(1.0f, CenterToBottom);
            }
        }
        LoadedWheelGroundRadii.Add(GroundRadius);
        RuntimeWheelRadius = FMath::Max(RuntimeWheelRadius, GroundRadius);

        const float VisualRestLength = GetStableWheelVisualSpringLength();
        WheelOffsets.Add(AuthoredWheelCenter + FVector(0.0f, 0.0f, VisualRestLength));
        WheelTargetSpringLengths.Add(VisualRestLength);
    }

    // No procedural/default vehicle is created. A valid vehicle needs authored body and wheel meshes.
    if (BodyRenderPartCount == 0 || WheelOffsets.Num() == 0 || RenderParts.Num() == 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("VehiclePawn: vehicle requires at least one body mesh and one wheel mesh (name tag or JSON WheelMeshNames). Load skipped: %s"),
            *SourceFilePath);
        ClearLoadedVehicleModel();
        return false;
    }

    const TArray<FVector> AuthoredWheelOffsets = WheelOffsets;
    ApplyConfiguredWheelHeightOffsets();
    PrepareLoadedVehicleState();

    FBox CombinedLocalBounds = LoadedBodyVisualBounds;
    if (LoadedWheelVisualRestBounds.IsValid)
    {
        CombinedLocalBounds += LoadedWheelVisualRestBounds.Min;
        CombinedLocalBounds += LoadedWheelVisualRestBounds.Max;
    }

    const FInstancedEntityRegistrationOptions RegistrationOptions = BuildRegistrationOptions(true);
    InstancedRenderRegistrationId = InstancedEntities->RegisterEntity(
        SourceFilePath,
        this,
        Body,
        RenderParts,
        RegistrationOptions,
        CombinedLocalBounds);
    if (InstancedRenderRegistrationId == INDEX_NONE)
    {
        UE_LOG(LogTemp, Warning, TEXT("VehiclePawn: failed to register instanced renderer for %s"), *SourceFilePath);
        ClearLoadedVehicleModel();
        return false;
    }

    FInstancedVehicleTemplateData VehicleTemplateData;
    VehicleTemplateData.WheelPartIndices = LoadedWheelRenderPartIndices;
    VehicleTemplateData.WheelBaseRotations = LoadedWheelBaseRotations;
    VehicleTemplateData.WheelBaseScales = LoadedWheelBaseScales;
    VehicleTemplateData.WheelVisualCenterOffsets = LoadedWheelVisualCenterOffsets;
    VehicleTemplateData.AuthoredWheelOffsets = AuthoredWheelOffsets;
    VehicleTemplateData.WheelTargetSpringLengths = WheelTargetSpringLengths;
    VehicleTemplateData.WheelGroundRadii = LoadedWheelGroundRadii;
    VehicleTemplateData.BodyVisualBounds = LoadedBodyVisualBounds;
    VehicleTemplateData.WheelVisualRestBounds = LoadedWheelVisualRestBounds;
    VehicleTemplateData.CombinedLocalBounds = CombinedLocalBounds;
    VehicleTemplateData.RuntimeWheelRadius = RuntimeWheelRadius;
    if (!InstancedEntities->StoreVehicleTemplateData(SourceFilePath, VehicleTemplateData))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("VehiclePawn: shared vehicle metadata could not be cached; this instance remains usable. Path=%s"),
            *SourceFilePath);
    }

    // The shared ISM actor now retains the meshes. This vehicle no longer needs a parser/cache.
    MeshCache.Empty();
    if (IsValid(GltfAsset))
    {
        FglTFRuntimeSafety::RequestAssetRelease(GltfAsset);
        GltfAsset = nullptr;
    }

    FinishSuccessfulModelLoad();
    return true;
}

float AVehiclePawn::GetConfiguredWheelHeightOffset(int32 WheelIndex, float FrontRearSplitX) const
{
    const float AxleOffset = WheelOffsets.IsValidIndex(WheelIndex) && WheelOffsets[WheelIndex].X >= FrontRearSplitX
        ? FrontWheelHeightOffset
        : RearWheelHeightOffset;
    const float PerWheelOffset = WheelHeightOffsets.IsValidIndex(WheelIndex)
        ? WheelHeightOffsets[WheelIndex]
        : 0.0f;
    return WheelHeightOffset + AxleOffset + PerWheelOffset;
}

void AVehiclePawn::ApplyConfiguredWheelHeightOffsets()
{
    if (WheelOffsets.Num() == 0)
    {
        return;
    }

    float MinX = WheelOffsets[0].X;
    float MaxX = WheelOffsets[0].X;
    for (const FVector& Offset : WheelOffsets)
    {
        MinX = FMath::Min(MinX, Offset.X);
        MaxX = FMath::Max(MaxX, Offset.X);
    }
    const float FrontRearSplitX = (MinX + MaxX) * 0.5f;

    for (int32 WheelIndex = 0; WheelIndex < WheelOffsets.Num(); ++WheelIndex)
    {
        WheelOffsets[WheelIndex].Z += GetConfiguredWheelHeightOffset(WheelIndex, FrontRearSplitX);
    }
}

void AVehiclePawn::DeactivateVehicleUntilModelLoaded()
{
    if (UVehicleSubSystem* VehicleSubSystem = UVehicleSubSystem::Get(this))
    {
        VehicleSubSystem->UnregisterVehicle(this);
    }

    if (IsValid(Body))
    {
        Body->SetSimulatePhysics(false);
        Body->SetEnableGravity(false);
        Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Body->SetGenerateOverlapEvents(false);
        Body->SetPhysicsLinearVelocity(FVector::ZeroVector);
        Body->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
    }
}

void AVehiclePawn::ActivateVehicleAfterModelLoad()
{
    if (!bVehicleModelLoaded || !IsValid(Body))
    {
        DeactivateVehicleUntilModelLoaded();
        return;
    }

    const bool bClientRenderOnlyVehicle =
        UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this) && !HasAuthority();
    if (bClientRenderOnlyVehicle)
    {
        Body->SetSimulatePhysics(false);
        Body->SetEnableGravity(false);
        Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Body->SetGenerateOverlapEvents(false);
        if (UVehicleSubSystem* VehicleSubSystem = UVehicleSubSystem::Get(this))
        {
            VehicleSubSystem->UnregisterVehicle(this);
        }
        return;
    }

    Body->SetCollisionProfileName(TEXT("Vehicle"));
    Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Body->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
    Body->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
    Body->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
    Body->SetGenerateOverlapEvents(true);
    ApplyVehicleBodyPhysicsSettings();
    Body->SetUseCCD(true);
    if (LowFrictionPhysicalMaterial)
    {
        Body->SetPhysMaterialOverride(LowFrictionPhysicalMaterial);
    }

    if (HasActorBegunPlay())
    {
        if (UVehicleSubSystem* VehicleSubSystem = UVehicleSubSystem::Get(this))
        {
            VehicleSubSystem->RegisterVehicle(this);
        }
    }
}

bool AVehiclePawn::ShouldUpdateVehicleSimulation() const
{
    if (!bVehicleModelLoaded || !IsValid(Body))
    {
        return false;
    }

    if (InstancedRenderRegistrationId != INDEX_NONE)
    {
        if (const UInstancedEntitySubsystem* InstancedEntities = UInstancedEntitySubsystem::Get(this))
        {
            if (!InstancedEntities->IsEntityPhysicsActive(InstancedRenderRegistrationId))
            {
                return false;
            }
        }
    }

    return bUseStableGroundRideHeight || Body->IsSimulatingPhysics();
}

void AVehiclePawn::ResetVehiclePoseAboveGround()
{
    if (!IsValid(Body))
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    const FVector CurrentLocation = GetActorLocation();
    const FVector TraceStart = CurrentLocation + FVector(0.0f, 0.0f, 400.0f);
    const FVector TraceEnd = CurrentLocation - FVector(0.0f, 0.0f, 1400.0f);
    FHitResult Hit;
    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(VehicleGroundClearanceTrace), false, this);
    QueryParams.AddIgnoredActor(this);

    if (FPhysicsHelper::Raycast(this, TraceStart, TraceEnd, QueryParams, Hit))
    {
        const float TargetZ = Hit.ImpactPoint.Z + GetDesiredCenterHeightAboveGround();
        SetActorLocation(FVector(CurrentLocation.X, CurrentLocation.Y, TargetZ), false, nullptr, ETeleportType::TeleportPhysics);
    }
    else
    {
        SetActorLocation(CurrentLocation + FVector(0.0f, 0.0f, GetDesiredCenterHeightAboveGround()), false, nullptr, ETeleportType::TeleportPhysics);
    }

    // Keep runtime-loaded vehicles at the configured mass after model/collision changes.
    // The default value is 1000 kg, matching a roughly one-ton small car.
    ApplyVehicleBodyPhysicsSettings();

    if (Body->IsSimulatingPhysics())
    {
        Body->SetPhysicsLinearVelocity(FVector::ZeroVector);
        Body->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
    }
    StablePlanarVelocity = FVector::ZeroVector;
    StableVerticalVelocity = 0.0f;
    bStablePlanarVelocityInitialized = false;
    StablePhysicsLinearVelocity = FVector::ZeroVector;
    StablePhysicsAngularVelocity = FVector::ZeroVector;
    bStablePhysicsStateInitialized = false;

    const float ResetSupportForce = (WheelOffsets.Num() > 0)
        ? FMath::Max(1.0f, VehicleMassKg) * FMath::Max(1.0f, FMath::Abs(World->GetGravityZ())) / static_cast<float>(WheelOffsets.Num())
        : 0.0f;
    WheelSpringLengths.SetNum(WheelOffsets.Num());
    WheelVisualSpringLengths.SetNum(WheelOffsets.Num());
    WheelSuspensionForces.SetNum(WheelOffsets.Num());
    WheelLateralForces.SetNum(WheelOffsets.Num());
    WheelGrounded.SetNum(WheelOffsets.Num());
    for (int32 WheelIndex = 0; WheelIndex < WheelOffsets.Num(); ++WheelIndex)
    {
        const float TargetSpringLength = GetTargetWheelSpringLength(WheelIndex);
        WheelSpringLengths[WheelIndex] = TargetSpringLength;
        WheelVisualSpringLengths[WheelIndex] = TargetSpringLength;
        WheelSuspensionForces[WheelIndex] = ResetSupportForce;
        WheelLateralForces[WheelIndex] = 0.0f;
        WheelGrounded[WheelIndex] = false;
    }

    ApplyStableVehicleGrounding(0.0f);
    UpdateWheelVisuals(0.0f);
    if (Body->IsSimulatingPhysics())
    {
        Body->WakeRigidBody();
    }
}

void AVehiclePawn::UpdateStableWheelVehicle(float DeltaSeconds)
{
    if (!IsValid(Body))
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World || DeltaSeconds <= 0.0f)
    {
        return;
    }

    const float SafeDeltaSeconds = FMath::Clamp(DeltaSeconds, 0.0f, 0.05f);
    if (SafeDeltaSeconds <= 0.0f)
    {
        return;
    }

    const float SafeMassKg = FMath::Max(1.0f, VehicleMassKg);
    const float MassScale = GetVehicleMassScale();
    const float SuspensionTravel = GetEffectiveSuspensionRestLength(INDEX_NONE);
    const float GroundBuffer = FMath::Max(0.0f, StableRideHeightGroundBuffer);
    // Do not let wheel sphere sweeps treat curb sides or sharp edges as suspension ground.
    // Accept only surfaces that are actually drivable for the configured max slope.
    const float DrivableNormalZ = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(StableMaxSlopeDegrees, 1.0f, 60.0f)));
    const float RequiredNormalZ = FMath::Clamp(FMath::Max(MinSuspensionHitNormalDot, DrivableNormalZ), 0.0f, 1.0f);
    const float GravityAcceleration = FMath::Max(1.0f, FMath::Abs(World->GetGravityZ()));
    const float RequiredSupportForcePerWheel = WheelOffsets.Num() > 0
        ? SafeMassKg * GravityAcceleration / static_cast<float>(WheelOffsets.Num())
        : 0.0f;
    WheelGrounded.SetNum(WheelOffsets.Num());
    WheelSpringLengths.SetNum(WheelOffsets.Num());
    WheelSuspensionForces.SetNum(WheelOffsets.Num());
    WheelLateralForces.SetNum(WheelOffsets.Num());

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(VehicleStableWheelTrace), false, this);
    QueryParams.AddIgnoredActor(this);

    if (!bStablePhysicsStateInitialized)
    {
        StablePhysicsLinearVelocity = Body->GetPhysicsLinearVelocity();
        StablePhysicsAngularVelocity = Body->GetPhysicsAngularVelocityInRadians();
        bStablePhysicsStateInitialized = true;
        bStablePlanarVelocityInitialized = true;
    }

    FVector CurrentLocation = GetActorLocation();
    FQuat CurrentQuat = GetActorQuat().GetNormalized();
    FVector Up = CurrentQuat.RotateVector(FVector::UpVector).GetSafeNormal();
    FVector Forward = CurrentQuat.RotateVector(FVector::ForwardVector).GetSafeNormal();
    FVector Right = CurrentQuat.RotateVector(FVector::RightVector).GetSafeNormal();
    if (Up.IsNearlyZero() || Forward.IsNearlyZero() || Right.IsNearlyZero())
    {
        Up = FVector::UpVector;
        Forward = GetActorForwardVector().GetSafeNormal();
        Right = FVector::CrossProduct(Up, Forward).GetSafeNormal();
    }

    float FrontMostWheelX = WheelOffsets.Num() > 0 ? WheelOffsets[0].X : 0.0f;
    float RearMostWheelX = FrontMostWheelX;
    float RightMostWheelY = WheelOffsets.Num() > 0 ? WheelOffsets[0].Y : 0.0f;
    float LeftMostWheelY = RightMostWheelY;
    for (const FVector& Offset : WheelOffsets)
    {
        FrontMostWheelX = FMath::Max(FrontMostWheelX, Offset.X);
        RearMostWheelX = FMath::Min(RearMostWheelX, Offset.X);
        RightMostWheelY = FMath::Max(RightMostWheelY, Offset.Y);
        LeftMostWheelY = FMath::Min(LeftMostWheelY, Offset.Y);
    }
    const float AxleSplitX = (FrontMostWheelX + RearMostWheelX) * 0.5f;
    const float Wheelbase = FMath::Max(80.0f, FrontMostWheelX - RearMostWheelX);
    const float TrackWidth = FMath::Max(60.0f, RightMostWheelY - LeftMostWheelY);

    const FVector LocalCenterOfMass(4.0f, 0.0f, FMath::Clamp(-BodyExtent.Z * 0.58f, -76.0f, -12.0f));
    const FVector CenterOfMassWorld = CurrentLocation + CurrentQuat.RotateVector(LocalCenterOfMass);

    auto TraceWheel = [&](const FTransform& ProbeTransform, int32 WheelIndex, FHitResult& OutHit, float& OutSpringLength) -> bool
    {
        const float WheelSuspensionTravel = GetEffectiveSuspensionRestLength(WheelIndex);
        OutSpringLength = WheelSuspensionTravel;
        if (!WheelOffsets.IsValidIndex(WheelIndex))
        {
            return false;
        }

        const float SafeWheelRadius = GetEffectiveWheelRadius(WheelIndex);
        const float TraceUp = FMath::Max(8.0f, SafeWheelRadius * 0.25f);
        const float TraceDown = FMath::Max(80.0f, WheelSuspensionTravel + SuspensionTraceExtra + SafeWheelRadius + GroundBuffer + 12.0f);
        const FVector ProbeUp = ProbeTransform.GetUnitAxis(EAxis::Z).GetSafeNormal();
        const FVector MountWorld = ProbeTransform.TransformPosition(WheelOffsets[WheelIndex]);
        const FVector TraceStart = MountWorld + ProbeUp * TraceUp;
        const FVector TraceEnd = MountWorld - ProbeUp * TraceDown;

        bool bHit = false;
        if (bUseSuspensionSweep)
        {
            const float SweepRadius = FMath::Clamp(SafeWheelRadius * SuspensionSweepRadiusScale, 2.0f, SafeWheelRadius * 0.85f);
            bHit = World->SweepSingleByChannel(
                OutHit,
                TraceStart,
                TraceEnd,
                FQuat::Identity,
                ECC_Visibility,
                FCollisionShape::MakeSphere(SweepRadius),
                QueryParams);
        }
        if (!bHit)
        {
            bHit = FPhysicsHelper::Raycast(this, TraceStart, TraceEnd, QueryParams, OutHit);
        }
        if (!bHit || !OutHit.bBlockingHit || OutHit.bStartPenetrating)
        {
            return false;
        }

        const FVector HitNormal = OutHit.ImpactNormal.GetSafeNormal();
        if (FVector::DotProduct(HitNormal, ProbeUp) < RequiredNormalZ)
        {
            return false;
        }

        const float MountToGround = FVector::DotProduct(MountWorld - OutHit.ImpactPoint, ProbeUp);
        OutSpringLength = FMath::Clamp(MountToGround - SafeWheelRadius - GroundBuffer, 0.0f, WheelSuspensionTravel);
        return true;
    };

    // Stop the wheel solver from treating the top of a tall block as ordinary ground. This preserves
    // curbs and ramps, but a ledge higher than StableMaxStepHeight becomes an obstacle instead of a lift.
    const FVector HorizontalVelocity(StablePhysicsLinearVelocity.X, StablePhysicsLinearVelocity.Y, 0.0f);
    if (HorizontalVelocity.SizeSquared2D() > 1.0f && StableMaxStepHeight > 0.0f)
    {
        const FVector HorizontalDelta = HorizontalVelocity * SafeDeltaSeconds;
        const FTransform CurrentProbeTransform(CurrentQuat, CurrentLocation);
        const FTransform PredictedProbeTransform(CurrentQuat, CurrentLocation + HorizontalDelta);
        const float MaxSlopeRisePerCm = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(StableMaxSlopeDegrees, 1.0f, 60.0f)));
        bool bBlockedByTallStep = false;

        for (int32 WheelIndex = 0; WheelIndex < WheelOffsets.Num(); ++WheelIndex)
        {
            FHitResult CurrentHit;
            FHitResult PredictedHit;
            float CurrentSpringLength = SuspensionTravel;
            float PredictedSpringLength = SuspensionTravel;
            if (!TraceWheel(CurrentProbeTransform, WheelIndex, CurrentHit, CurrentSpringLength)
                || !TraceWheel(PredictedProbeTransform, WheelIndex, PredictedHit, PredictedSpringLength))
            {
                continue;
            }

            const FVector CurrentMount = CurrentProbeTransform.TransformPosition(WheelOffsets[WheelIndex]);
            const FVector PredictedMount = PredictedProbeTransform.TransformPosition(WheelOffsets[WheelIndex]);
            const float HorizontalMoveCm = FMath::Max(1.0f, FVector::Dist2D(CurrentMount, PredictedMount));
            const float AllowedRise = FMath::Max(StableMaxStepHeight, HorizontalMoveCm * MaxSlopeRisePerCm + 2.0f);
            const float GroundRise = PredictedHit.ImpactPoint.Z - CurrentHit.ImpactPoint.Z;
            if (GroundRise > AllowedRise && GroundRise > StableMaxStepHeight)
            {
                bBlockedByTallStep = true;
                break;
            }
        }

        if (bBlockedByTallStep)
        {
            const FVector MoveDirection = HorizontalVelocity.GetSafeNormal2D();
            const float VelocityIntoStep = FVector::DotProduct(StablePhysicsLinearVelocity, MoveDirection);
            if (VelocityIntoStep > 0.0f)
            {
                StablePhysicsLinearVelocity -= MoveDirection * VelocityIntoStep;
            }
        }
    }

    struct FStableWheelPhysicsState
    {
        int32 Index = INDEX_NONE;
        FVector LocalOffset = FVector::ZeroVector;
        FVector MountWorld = FVector::ZeroVector;
        FVector ContactWorld = FVector::ZeroVector;
        FVector ImpactNormal = FVector::UpVector;
        FVector WheelForward = FVector::ForwardVector;
        FVector WheelRight = FVector::RightVector;
        bool bGrounded = false;
        bool bFront = false;
        bool bRightSide = false;
        float SpringLength = 0.0f;
        float Compression = 0.0f;
        float NormalForce = 0.0f;
        float ForwardSpeed = 0.0f;
        float LateralSpeed = 0.0f;
    };

    TArray<FStableWheelPhysicsState> WheelStates;
    WheelStates.SetNum(WheelOffsets.Num());

    FVector TotalForce(0.0f, 0.0f, World->GetGravityZ() * SafeMassKg);
    FVector TotalTorque = FVector::ZeroVector;
    auto AccumulateForceAtLocation = [&](const FVector& Force, const FVector& WorldLocation)
    {
        if (Force.IsNearlyZero())
        {
            return;
        }
        TotalForce += Force;
        TotalTorque += FVector::CrossProduct(WorldLocation - CenterOfMassWorld, Force);
    };

    const float BodyForwardSpeed = FVector::DotProduct(StablePhysicsLinearVelocity, Forward);
    const float AbsBodyForwardSpeed = FMath::Abs(BodyForwardSpeed);
    const float SteeringSpeedAlphaRaw = FMath::Clamp(AbsBodyForwardSpeed / FMath::Max(100.0f, SteeringSpeedForFullAssist), 0.0f, 1.0f);
    const float SteeringSpeedAlpha = SteeringSpeedAlphaRaw * SteeringSpeedAlphaRaw * (3.0f - 2.0f * SteeringSpeedAlphaRaw);
    // Preserve deliberate steering authority at speed. Older JSON templates commonly contained a
    // very small HighSpeedSteeringAngleDegrees value; the v3 authority scale upgrades those files
    // without rewriting them and still caps the result at the low-speed steering lock.
    const float HighSpeedSteeringAuthority = FMath::Clamp(
        HighSpeedSteeringAuthorityScale * FMath::Lerp(1.0f, 1.15f, SteeringSpeedAlpha),
        1.0f,
        2.0f);
    const float EffectiveHighSpeedSteeringDegrees = FMath::Clamp(
        HighSpeedSteeringAngleDegrees * HighSpeedSteeringAuthority,
        1.0f,
        FMath::Max(1.0f, MaxSteeringAngleDegrees));
    const float EffectiveMaxSteeringDegrees = FMath::Lerp(
        MaxSteeringAngleDegrees,
        EffectiveHighSpeedSteeringDegrees,
        SteeringSpeedAlpha);
    const float BaseSteeringAngle = FMath::DegreesToRadians(EffectiveMaxSteeringDegrees * SmoothedSteeringInput);
    const float HighSpeedGripAlphaRaw = FMath::Clamp(AbsBodyForwardSpeed / FMath::Max(100.0f, HighSpeedLateralGripSpeed), 0.0f, 1.0f);
    const float HighSpeedGripAlpha = HighSpeedGripAlphaRaw * HighSpeedGripAlphaRaw * (3.0f - 2.0f * HighSpeedGripAlphaRaw);
    const float SpeedLateralGripScale = FMath::Lerp(1.0f, FMath::Clamp(HighSpeedLateralGripScale, 0.1f, 1.0f), HighSpeedGripAlpha);

    int32 GroundedWheels = 0;
    int32 GroundedFrontWheels = 0;
    int32 GroundedRearWheels = 0;
    float FrontZ = 0.0f;
    float RearZ = 0.0f;
    float RightZ = 0.0f;
    float LeftZ = 0.0f;
    int32 FrontCount = 0;
    int32 RearCount = 0;
    int32 RightCount = 0;
    int32 LeftCount = 0;
    FVector AverageGroundNormal = FVector::ZeroVector;
    float TotalCompressionForGroundedWheels = 0.0f;

    const FTransform BodyTransform(CurrentQuat, CurrentLocation);
    for (int32 WheelIndex = 0; WheelIndex < WheelOffsets.Num(); ++WheelIndex)
    {
        FStableWheelPhysicsState& WheelState = WheelStates[WheelIndex];
        WheelState.Index = WheelIndex;
        WheelState.LocalOffset = WheelOffsets[WheelIndex];
        WheelState.MountWorld = BodyTransform.TransformPosition(WheelState.LocalOffset);
        WheelState.bFront = WheelState.LocalOffset.X >= AxleSplitX;
        WheelState.bRightSide = WheelState.LocalOffset.Y > 0.0f;

        const bool bWasGrounded = WheelGrounded.IsValidIndex(WheelIndex) && WheelGrounded[WheelIndex];
        const float SafeWheelRadius = GetEffectiveWheelRadius(WheelIndex);
        const float WheelSuspensionTravel = GetEffectiveSuspensionRestLength(WheelIndex);
        const float PreviousSpringLength = WheelSpringLengths.IsValidIndex(WheelIndex) ? WheelSpringLengths[WheelIndex] : GetTargetWheelSpringLength(WheelIndex);
        FHitResult Hit;
        float SpringLength = WheelSuspensionTravel;
        if (!TraceWheel(BodyTransform, WheelIndex, Hit, SpringLength))
        {
            WheelGrounded[WheelIndex] = false;
            WheelSpringLengths[WheelIndex] = FMath::FInterpTo(PreviousSpringLength, WheelSuspensionTravel, SafeDeltaSeconds, FMath::Max(0.1f, SuspensionContactSmoothingSpeed));
            WheelSuspensionForces[WheelIndex] = FMath::FInterpTo(WheelSuspensionForces[WheelIndex], 0.0f, SafeDeltaSeconds, FMath::Max(0.1f, SuspensionForceInterpSpeed));
            WheelLateralForces[WheelIndex] = FMath::FInterpTo(WheelLateralForces[WheelIndex], 0.0f, SafeDeltaSeconds, FMath::Max(0.1f, TireForceInterpSpeed));
            continue;
        }

        const float RawSuspensionVelocity = bWasGrounded
            ? (PreviousSpringLength - SpringLength) / FMath::Max(0.001f, SafeDeltaSeconds)
            : 0.0f;
        const float SuspensionVelocityLimit = FMath::Max(20.0f, MaxSuspensionVelocity);
        const float SuspensionVelocity = FMath::Clamp(RawSuspensionVelocity, -SuspensionVelocityLimit, SuspensionVelocityLimit);
        const float Compression = FMath::Max(0.0f, WheelSuspensionTravel - SpringLength);

        // Conservative raycast-car suspension. The spring supports the chassis, but rebound energy is
        // deliberately bled off so a curb edge, trace jitter, or a wheel regaining contact cannot kick
        // the vehicle into a self-sustaining hop.
        const float NeutralSpringLength = FMath::Clamp(GetTargetWheelSpringLength(WheelIndex), GetMinimumWheelSpringLength(WheelIndex), GetEffectiveSuspensionRestLength(WheelIndex));
        const float NeutralCompression = FMath::Max(1.0f, WheelSuspensionTravel - NeutralSpringLength);
        const float ContactAlpha = FMath::Clamp(Compression / NeutralCompression, 0.0f, 1.0f);
        const float RideHeightError = NeutralSpringLength - SpringLength;
        const float PositionCorrection = RideHeightError * SuspensionStrength * 0.34f * MassScale;
        const float StableDamperResponseMultiplier = SuspensionVelocity >= 0.0f ? 1.24f : 0.58f;
        const float DampingCorrection = SuspensionVelocity * SuspensionDamping * 0.24f * StableDamperResponseMultiplier * MassScale;
        const float StaticSupport = RequiredSupportForcePerWheel * FMath::Lerp(0.62f, 1.0f, ContactAlpha);
        const float RawTargetSuspensionForce = (StaticSupport + PositionCorrection + DampingCorrection)
            * FMath::Clamp(SuspensionForceScale, 0.0f, 1.0f);
        const float SuspensionForceLimit = FMath::Max(
            MaxSuspensionForcePerWheel * MassScale,
            RequiredSupportForcePerWheel * FMath::Clamp(StableSuspensionForceLimitMultiplier, 1.0f, 1.65f));
        float TargetSuspensionForce = FMath::Clamp(RawTargetSuspensionForce, 0.0f, SuspensionForceLimit);
        const float StableBumpStopStartLength = FMath::Clamp(NeutralSpringLength * 0.52f, 5.0f, WheelSuspensionTravel * 0.58f);
        if (SpringLength < StableBumpStopStartLength)
        {
            const float BumpStopAlpha = FMath::Clamp((StableBumpStopStartLength - SpringLength) / FMath::Max(1.0f, StableBumpStopStartLength), 0.0f, 1.0f);
            const float BumpStopForce = (BumpStopAlpha * BumpStopAlpha * RequiredSupportForcePerWheel * 0.48f)
                + (StableBumpStopStartLength - SpringLength) * SuspensionStrength * MassScale * 0.18f;
            TargetSuspensionForce = FMath::Clamp(TargetSuspensionForce + BumpStopForce, 0.0f, SuspensionForceLimit);
        }

        const FVector WheelPointVelocity = StablePhysicsLinearVelocity + FVector::CrossProduct(
            StablePhysicsAngularVelocity,
            WheelState.MountWorld - CenterOfMassWorld);
        const float UpwardSpeedAtMount = FVector::DotProduct(WheelPointVelocity, Up);
        const float ReboundSpeedLimit = FMath::Max(20.0f, StableMaxGroundedUpSpeed);
        if (UpwardSpeedAtMount > ReboundSpeedLimit)
        {
            const float ReboundReleaseAlpha = FMath::Clamp(
                (UpwardSpeedAtMount - ReboundSpeedLimit) / FMath::Max(1.0f, ReboundSpeedLimit * 2.0f),
                0.0f,
                1.0f);
            TargetSuspensionForce = FMath::Lerp(
                TargetSuspensionForce,
                FMath::Min(TargetSuspensionForce, RequiredSupportForcePerWheel * 0.35f),
                ReboundReleaseAlpha);
        }
        if (!bWasGrounded)
        {
            const bool bReboundingOnNewContact = UpwardSpeedAtMount > ReboundSpeedLimit;
            TargetSuspensionForce = FMath::Min(
                TargetSuspensionForce,
                RequiredSupportForcePerWheel * (bReboundingOnNewContact ? 0.50f : 0.92f));
        }

        const float PreviousSuspensionForce = WheelSuspensionForces.IsValidIndex(WheelIndex) ? WheelSuspensionForces[WheelIndex] : 0.0f;
        const bool bIncreasingSuspensionForce = TargetSuspensionForce > PreviousSuspensionForce;
        const bool bCompressingSuspension = SuspensionVelocity > 0.0f || SpringLength < PreviousSpringLength - 0.5f;
        float ForceInterpSpeed = bWasGrounded
            ? FMath::Max(2.5f, SuspensionForceInterpSpeed)
            : FMath::Max(1.8f, SuspensionForceInterpSpeed * 0.70f);
        ForceInterpSpeed *= bIncreasingSuspensionForce
            ? (bCompressingSuspension ? FMath::Max(0.1f, SuspensionCompressionRiseMultiplier) : 1.30f)
            : FMath::Max(0.1f, SuspensionReboundReleaseMultiplier);
        const float SmoothedSuspensionForce = FMath::FInterpTo(PreviousSuspensionForce, TargetSuspensionForce, SafeDeltaSeconds, ForceInterpSpeed);
        const float ForceStepMultiplier = bIncreasingSuspensionForce
            ? (bCompressingSuspension ? FMath::Max(0.1f, SuspensionCompressionRiseMultiplier) : 1.30f)
            : FMath::Max(0.1f, SuspensionReboundReleaseMultiplier);
        const float SuspensionForceStep = FMath::Max(1000.0f, MaxSuspensionForceChangePerSecond) * MassScale * ForceStepMultiplier * SafeDeltaSeconds;
        const float SuspensionForce = FMath::Clamp(SmoothedSuspensionForce, PreviousSuspensionForce - SuspensionForceStep, PreviousSuspensionForce + SuspensionForceStep);

        WheelState.bGrounded = true;
        WheelState.ContactWorld = Hit.ImpactPoint;
        WheelState.ImpactNormal = Hit.ImpactNormal.GetSafeNormal();
        WheelState.SpringLength = SpringLength;
        WheelState.Compression = Compression;
        WheelState.NormalForce = SuspensionForce;

        WheelGrounded[WheelIndex] = true;
        WheelSpringLengths[WheelIndex] = FMath::Clamp(SpringLength, FMath::Max(3.0f, SafeWheelRadius * 0.28f), WheelSuspensionTravel);
        WheelSuspensionForces[WheelIndex] = SuspensionForce;
        ++GroundedWheels;
        TotalCompressionForGroundedWheels += Compression;
        AverageGroundNormal += WheelState.ImpactNormal;
        if (WheelState.bFront)
        {
            ++GroundedFrontWheels;
            FrontZ += WheelState.ContactWorld.Z;
            ++FrontCount;
        }
        else
        {
            ++GroundedRearWheels;
            RearZ += WheelState.ContactWorld.Z;
            ++RearCount;
        }
        if (WheelState.bRightSide)
        {
            RightZ += WheelState.ContactWorld.Z;
            ++RightCount;
        }
        else
        {
            LeftZ += WheelState.ContactWorld.Z;
            ++LeftCount;
        }

        const FVector SuspensionAxis = FMath::Lerp(WheelState.ImpactNormal, Up, 0.38f).GetSafeNormal();
        AccumulateForceAtLocation(SuspensionAxis * SuspensionForce, WheelState.MountWorld);
    }

    auto ApplyAntiRollForAxle = [&](bool bFrontAxle)
    {
        FStableWheelPhysicsState* LeftWheel = nullptr;
        FStableWheelPhysicsState* RightWheel = nullptr;
        for (FStableWheelPhysicsState& WheelState : WheelStates)
        {
            if (!WheelState.bGrounded || WheelState.bFront != bFrontAxle)
            {
                continue;
            }
            if (WheelState.bRightSide)
            {
                RightWheel = &WheelState;
            }
            else
            {
                LeftWheel = &WheelState;
            }
        }
        if (!LeftWheel || !RightWheel)
        {
            return;
        }

        const float CompressionDifference = RightWheel->Compression - LeftWheel->Compression;
        const float AntiRollForce = FMath::Clamp(CompressionDifference * AntiRollBarStiffness * MassScale, -MaxAntiRollForce * MassScale, MaxAntiRollForce * MassScale);
        const FVector AntiRollAxis = FMath::Lerp(FVector::UpVector, Up, 0.40f).GetSafeNormal();
        AccumulateForceAtLocation(AntiRollAxis * AntiRollForce, RightWheel->MountWorld);
        AccumulateForceAtLocation(-AntiRollAxis * AntiRollForce, LeftWheel->MountWorld);
    };

    ApplyAntiRollForAxle(true);
    ApplyAntiRollForAxle(false);

    const float FrontDriveShare = FMath::Clamp(DrivenFrontTorqueShare, 0.0f, 1.0f);
    const float RearDriveShare = 1.0f - FrontDriveShare;
    const float AbsThrottle = FMath::Abs(SmoothedThrottleInput);
    const float SafeSpeedLimit = FMath::Max(500.0f, MaxSpeedForward);
    const float SpeedLimitAlpha = FMath::Clamp((AbsBodyForwardSpeed - SafeSpeedLimit * 0.96f) / FMath::Max(1.0f, SafeSpeedLimit * 0.04f), 0.0f, 1.0f);
    const bool bAcceleratingTowardLimit = !FMath::IsNearlyZero(SmoothedThrottleInput, 0.01f)
        && FMath::Sign(SmoothedThrottleInput) == FMath::Sign(BodyForwardSpeed);
    const float SpeedLimiter = bAcceleratingTowardLimit ? (1.0f - SpeedLimitAlpha) : 1.0f;

    for (FStableWheelPhysicsState& WheelState : WheelStates)
    {
        if (!WheelState.bGrounded || WheelState.NormalForce <= KINDA_SMALL_NUMBER)
        {
            continue;
        }

        float WheelSteerAngle = 0.0f;
        if (WheelState.bFront)
        {
            WheelSteerAngle = BaseSteeringAngle;
            if (!FMath::IsNearlyZero(WheelSteerAngle, 0.001f) && AckermannStrength > 0.0f)
            {
                const float TurnSign = FMath::Sign(WheelSteerAngle);
                const float HalfTrack = FMath::Max(20.0f, FMath::Abs(WheelState.LocalOffset.Y));
                const float BaseTurnRadius = Wheelbase / FMath::Max(0.05f, FMath::Tan(FMath::Abs(WheelSteerAngle)));
                const bool bInnerWheel = TurnSign * WheelState.LocalOffset.Y > 0.0f;
                const float AdjustedRadius = FMath::Max(50.0f, BaseTurnRadius + (bInnerWheel ? -HalfTrack : HalfTrack));
                const float AckermannAngle = TurnSign * FMath::Atan(Wheelbase / AdjustedRadius);
                WheelSteerAngle = FMath::Lerp(WheelSteerAngle, AckermannAngle, FMath::Clamp(AckermannStrength, 0.0f, 1.0f));
            }
        }

        const FQuat SteerQuat(Up, WheelSteerAngle);
        FVector WheelForward = FVector::VectorPlaneProject(SteerQuat.RotateVector(Forward), WheelState.ImpactNormal).GetSafeNormal();
        if (WheelForward.IsNearlyZero())
        {
            WheelForward = FVector::VectorPlaneProject(Forward, WheelState.ImpactNormal).GetSafeNormal();
        }
        if (WheelForward.IsNearlyZero())
        {
            WheelForward = Forward;
        }
        FVector WheelRight = FVector::CrossProduct(WheelState.ImpactNormal, WheelForward).GetSafeNormal();
        if (WheelRight.IsNearlyZero())
        {
            WheelRight = Right;
        }

        const FVector ContactVelocity = StablePhysicsLinearVelocity + FVector::CrossProduct(StablePhysicsAngularVelocity, WheelState.ContactWorld - CenterOfMassWorld);
        WheelState.WheelForward = WheelForward;
        WheelState.WheelRight = WheelRight;
        WheelState.ForwardSpeed = FVector::DotProduct(ContactVelocity, WheelForward);
        WheelState.LateralSpeed = FVector::DotProduct(ContactVelocity, WheelRight);

        const int32 GroundedAxleCount = WheelState.bFront ? FMath::Max(1, GroundedFrontWheels) : FMath::Max(1, GroundedRearWheels);
        const float AxleDriveShare = WheelState.bFront ? FrontDriveShare : RearDriveShare;
        const bool bThrottleIsBrake = AbsThrottle > 0.05f
            && FMath::Abs(WheelState.ForwardSpeed) > 120.0f
            && FMath::Sign(SmoothedThrottleInput) != FMath::Sign(WheelState.ForwardSpeed);

        float LongitudinalDemand = 0.0f;
        if (bThrottleIsBrake)
        {
            LongitudinalDemand = -FMath::Sign(WheelState.ForwardSpeed) * BrakeForce * MassScale * AbsThrottle / static_cast<float>(FMath::Max(1, GroundedWheels));
        }
        else if (AbsThrottle > 0.02f)
        {
            const float DriveMagnitude = (SmoothedThrottleInput >= 0.0f ? EngineForce : ReverseForce) * MassScale;
            LongitudinalDemand = SmoothedThrottleInput * DriveMagnitude * AxleDriveShare * SpeedLimiter / static_cast<float>(GroundedAxleCount);
        }
        else if (FMath::Abs(WheelState.ForwardSpeed) > 25.0f)
        {
            LongitudinalDemand = -FMath::Sign(WheelState.ForwardSpeed) * EngineBrakingForce * MassScale / static_cast<float>(FMath::Max(1, GroundedWheels));
        }

        if (FMath::Abs(WheelState.ForwardSpeed) > 15.0f && RollingResistance > 0.0f)
        {
            LongitudinalDemand += -FMath::Sign(WheelState.ForwardSpeed) * WheelState.NormalForce * RollingResistance;
        }

        const float UphillForwardAmount = FMath::Clamp(
            FVector::DotProduct(WheelState.WheelForward, FVector::UpVector) * FMath::Sign(SmoothedThrottleInput),
            0.0f,
            0.35f);
        const float SlopeTractionAssist = FMath::Clamp(UphillForwardAmount / 0.22f, 0.0f, 1.0f);
        const float LongitudinalLimit = FMath::Max(
            1.0f,
            WheelState.NormalForce * TireLongitudinalFriction * FMath::Lerp(1.0f, 1.22f, SlopeTractionAssist));
        float LongitudinalForce = FMath::Clamp(LongitudinalDemand, -LongitudinalLimit, LongitudinalLimit);

        // Preserve a bounded part of the friction circle for cornering. Previously full drive force
        // could produce LongitudinalUsage=1, reducing the lateral limit to zero exactly when a fast
        // vehicle needed front-tire authority. The reservation is almost absent at parking speed,
        // increases smoothly with speed/steering input, and is slightly stronger on the front axle.
        const float SteeringReserveActivity = FMath::Clamp(FMath::Abs(SmoothedSteeringInput), 0.0f, 1.0f)
            * FMath::Lerp(0.25f, 1.0f, HighSpeedGripAlpha);
        const float AxleReserveScale = WheelState.bFront ? 1.0f : 0.72f;
        const float LateralGripReserve = FMath::Clamp(
            SteeringLateralGripReserve * FMath::Lerp(1.0f, HighSpeedSteeringAuthority, HighSpeedGripAlpha)
                * SteeringReserveActivity * AxleReserveScale,
            0.0f,
            0.90f);
        const float MaxLongitudinalUsageForSteering = FMath::Sqrt(FMath::Max(
            0.0f,
            1.0f - LateralGripReserve * LateralGripReserve));
        const float SteeringLongitudinalLimit = LongitudinalLimit * MaxLongitudinalUsageForSteering;
        LongitudinalForce = FMath::Clamp(
            LongitudinalForce,
            -SteeringLongitudinalLimit,
            SteeringLongitudinalLimit);

        // Tire side force is generated from slip angle, not by directly rotating the chassis.
        // The small reference speed keeps low-speed steering responsive without creating a snap turn.
        const float SlipReferenceSpeed = FMath::Max(1.0f, TireSlipReferenceSpeed + FMath::Abs(WheelState.ForwardSpeed) * 0.05f);
        const float EffectiveLongitudinalSpeed = FMath::Max(SlipReferenceSpeed, FMath::Abs(WheelState.ForwardSpeed));
        const float SlipAngle = FMath::Atan2(WheelState.LateralSpeed, EffectiveLongitudinalSpeed);
        const float SteeringGripMultiplier = WheelState.bFront ? FMath::Max(0.1f, FrontSteeringGripMultiplier) : FMath::Max(0.1f, RearSteeringGripMultiplier);
        const float SteeringActivity = FMath::Clamp(FMath::Abs(SmoothedSteeringInput), 0.0f, 1.0f);
        const float EffectiveHighSpeedFrontGripBoost = FMath::Clamp(
            1.0f + (FMath::Clamp(HighSpeedFrontGripBoost, 1.0f, 2.0f) - 1.0f) * HighSpeedSteeringAuthority,
            1.0f,
            2.0f);
        const float FrontGripBoost = WheelState.bFront
            ? FMath::Lerp(1.0f, EffectiveHighSpeedFrontGripBoost, HighSpeedGripAlpha * SteeringActivity)
            : 1.0f;
        // HighSpeedLateralGripScale limits capacity only once. Applying it to both stiffness and
        // capacity compounded the loss and made fast vehicles ignore steering input.
        const float CorneringStiffness = FMath::Max(0.1f, TireCorneringStiffness)
            * FMath::Max(0.1f, LateralGrip) * SteeringGripMultiplier * FrontGripBoost;
        const float LateralDemand = -SlipAngle * CorneringStiffness * WheelState.NormalForce;
        const float LateralCapacityBoost = WheelState.bFront ? FMath::Lerp(1.0f, FrontGripBoost, 0.35f) : 1.0f;
        const float LateralLimitBase = FMath::Min(WheelState.NormalForce * TireLateralFriction, MaxLateralGripForce * MassScale)
            * SpeedLateralGripScale * LateralCapacityBoost;
        const float LongitudinalUsage = FMath::Clamp(FMath::Abs(LongitudinalForce) / LongitudinalLimit, 0.0f, 1.0f);
        const float LateralLimit = LateralLimitBase * FMath::Sqrt(FMath::Max(0.0f, 1.0f - LongitudinalUsage * LongitudinalUsage));
        const float TargetLateralForce = FMath::Clamp(LateralDemand, -LateralLimit, LateralLimit) * FMath::Clamp(TireLateralForceScale, 0.0f, 1.0f);
        const float PreviousLateralForce = WheelLateralForces.IsValidIndex(WheelState.Index) ? WheelLateralForces[WheelState.Index] : 0.0f;
        const float SmoothedLateralForce = FMath::FInterpTo(PreviousLateralForce, TargetLateralForce, SafeDeltaSeconds, FMath::Max(0.1f, TireForceInterpSpeed));
        const float LateralForceStep = FMath::Max(1000.0f, MaxLateralForceChangePerSecond) * MassScale * SafeDeltaSeconds;
        const float LateralForce = FMath::Clamp(SmoothedLateralForce, PreviousLateralForce - LateralForceStep, PreviousLateralForce + LateralForceStep);
        if (WheelLateralForces.IsValidIndex(WheelState.Index))
        {
            WheelLateralForces[WheelState.Index] = LateralForce;
        }

        const float HeightToCenter = FVector::DotProduct(CenterOfMassWorld - WheelState.ContactWorld, Up);
        const FVector CenterHeightLocation = WheelState.ContactWorld + Up * HeightToCenter;
        const FVector LongitudinalForceLocation = FMath::Lerp(WheelState.ContactWorld, CenterHeightLocation, FMath::Clamp(DriveForceCenterOfMassHeightBlend, 0.0f, 1.0f));
        const FVector LateralForceLocation = FMath::Lerp(WheelState.ContactWorld, CenterHeightLocation, FMath::Clamp(LateralForceCenterOfMassHeightBlend, 0.0f, 1.0f));

        AccumulateForceAtLocation(WheelForward * LongitudinalForce, LongitudinalForceLocation);
        AccumulateForceAtLocation(WheelRight * LateralForce, LateralForceLocation);
    }

    const float GroundedRatio = static_cast<float>(GroundedWheels) / static_cast<float>(FMath::Max(1, WheelOffsets.Num()));
    if (GroundedWheels > 0)
    {
        const float FrontAverageZ = FrontCount > 0 ? FrontZ / static_cast<float>(FrontCount) : 0.0f;
        const float RearAverageZ = RearCount > 0 ? RearZ / static_cast<float>(RearCount) : 0.0f;
        const float RightAverageZ = RightCount > 0 ? RightZ / static_cast<float>(RightCount) : 0.0f;
        const float LeftAverageZ = LeftCount > 0 ? LeftZ / static_cast<float>(LeftCount) : 0.0f;

        FVector DesiredForward = Forward;
        FVector DesiredRight = Right;
        if (FrontCount > 0 && RearCount > 0)
        {
            DesiredForward = (Forward + FVector::UpVector * FMath::Clamp((FrontAverageZ - RearAverageZ) / Wheelbase, -0.70f, 0.70f)).GetSafeNormal();
        }
        if (RightCount > 0 && LeftCount > 0)
        {
            DesiredRight = (Right + FVector::UpVector * FMath::Clamp((RightAverageZ - LeftAverageZ) / TrackWidth, -0.60f, 0.60f)).GetSafeNormal();
        }

        FVector DesiredUp = FVector::CrossProduct(DesiredForward, DesiredRight).GetSafeNormal();
        if (!AverageGroundNormal.IsNearlyZero())
        {
            DesiredUp = FMath::Lerp(DesiredUp, AverageGroundNormal.GetSafeNormal(), 0.20f).GetSafeNormal();
        }
        if (DesiredUp.IsNearlyZero() || DesiredUp.Z < 0.20f)
        {
            DesiredUp = FVector::UpVector;
        }

        const FVector AttitudeAxis = FVector::CrossProduct(Up, DesiredUp);
        const FVector AttitudeTorque = (AttitudeAxis * FMath::Max(0.0f, StableTerrainAttitudeStrength) * MassScale * GroundedRatio)
            .GetClampedToMaxSize(38000.0f * MassScale);
        TotalTorque += AttitudeTorque;

        const FVector YawAngularVelocity = Up * FVector::DotProduct(StablePhysicsAngularVelocity, Up);
        const FVector PitchRollAngularVelocity = StablePhysicsAngularVelocity - YawAngularVelocity;
        const FVector PitchRollDampingTorque = (-PitchRollAngularVelocity * FMath::Max(0.0f, StablePitchRollDamping) * MassScale * GroundedRatio)
            .GetClampedToMaxSize(62000.0f * MassScale);
        TotalTorque += PitchRollDampingTorque;

        const float CurrentYawRate = FVector::DotProduct(StablePhysicsAngularVelocity, Up);
        const float YawDirectionSign = AbsBodyForwardSpeed > 35.0f
            ? FMath::Sign(BodyForwardSpeed)
            : (FMath::Abs(SmoothedThrottleInput) > 0.05f ? FMath::Sign(SmoothedThrottleInput) : 1.0f);
        const float LowSpeedTurnActivity = FMath::Clamp(
            FMath::Abs(SmoothedSteeringInput) * FMath::Max(FMath::Abs(SmoothedThrottleInput), AbsBodyForwardSpeed / 300.0f),
            0.0f,
            1.0f);
        const float EffectiveYawSpeed = FMath::Max(AbsBodyForwardSpeed, FMath::Max(0.0f, LowSpeedSteeringYawAssistSpeed) * LowSpeedTurnActivity);
        const float DesiredYawRate = FMath::Clamp(
            YawDirectionSign * EffectiveYawSpeed * FMath::Tan(BaseSteeringAngle) / FMath::Max(80.0f, Wheelbase),
            -FMath::Max(0.1f, StableMaxYawRateRadians),
            FMath::Max(0.1f, StableMaxYawRateRadians));
        const float YawRateError = DesiredYawRate - CurrentYawRate;
        const float ParkingSpeedAssistAlpha = FMath::Clamp(
            (FMath::Max(1.0f, LowSpeedSteeringYawAssistSpeed) * 2.5f - AbsBodyForwardSpeed) /
            FMath::Max(1.0f, LowSpeedSteeringYawAssistSpeed * 2.5f),
            0.0f,
            1.0f);
        const float SteeringAmount = FMath::Clamp(FMath::Abs(SmoothedSteeringInput), 0.0f, 1.0f);
        const float HighSpeedAssistStart = FMath::Max(100.0f, HighSpeedYawAssistStartSpeed);
        const float HighSpeedAssistAlpha = FMath::Clamp(
            (AbsBodyForwardSpeed - HighSpeedAssistStart) / FMath::Max(600.0f, FMath::Min(1800.0f, SteeringSpeedForFullAssist - HighSpeedAssistStart)),
            0.0f,
            1.0f);
        const float DesiredYawAbs = FMath::Abs(DesiredYawRate);
        const float DesiredYawDirection = FMath::Sign(DesiredYawRate);
        const float DirectedCurrentYawRate = DesiredYawDirection == 0.0f ? 0.0f : CurrentYawRate * DesiredYawDirection;
        const float UndersteerAlpha = SteeringAmount > 0.02f
            ? FMath::Clamp((DesiredYawAbs - FMath::Max(0.0f, DirectedCurrentYawRate)) / FMath::Max(0.12f, DesiredYawAbs), 0.0f, 1.0f)
            : 0.0f;
        const float AssistStrength = FMath::Lerp(
            SteeringYawRateAssist,
            FMath::Max(SteeringYawRateAssist, HighSpeedYawAssistStrength) * HighSpeedSteeringAuthority,
            HighSpeedAssistAlpha);
        const float AssistActivity = FMath::Max(ParkingSpeedAssistAlpha * 0.26f, HighSpeedAssistAlpha * UndersteerAlpha);
        const float YawAssistTorque = YawRateError * AssistStrength * MassScale * GroundedRatio * AssistActivity * SteeringAmount;
        const float YawDampingTorque = FMath::Lerp(
            -CurrentYawRate * SteeringYawDamping * 0.08f * MassScale * GroundedRatio,
            YawRateError * SteeringYawDamping * 0.060f * MassScale * GroundedRatio,
            SteeringAmount);
        const float EffectiveSteeringTorqueLimit = MaxSteeringAssistTorque * MassScale
            * FMath::Lerp(1.0f, HighSpeedSteeringAuthority, HighSpeedAssistAlpha);
        TotalTorque += Up * FMath::Clamp(
            YawAssistTorque + YawDampingTorque,
            -EffectiveSteeringTorqueLimit,
            EffectiveSteeringTorqueLimit);
    }
    else
    {
        const FVector AirDampingTorque = (-StablePhysicsAngularVelocity * FMath::Max(0.0f, AirborneAngularDampingTorque) * MassScale)
            .GetClampedToMaxSize(FMath::Max(1.0f, MaxAirborneAngularDampingTorque * MassScale));
        TotalTorque += AirDampingTorque;
    }

    if (GroundedWheels > 0)
    {
        const float VerticalVelocityAlongUp = FVector::DotProduct(StablePhysicsLinearVelocity, Up);
        const float ReboundSpeedLimit = FMath::Max(20.0f, StableMaxGroundedUpSpeed);
        if (VerticalVelocityAlongUp > ReboundSpeedLimit)
        {
            const float GroundedVerticalDampingForce = FMath::Clamp(
                -(VerticalVelocityAlongUp - ReboundSpeedLimit) * FMath::Max(0.0f, StableGroundedVerticalDamping) * MassScale * GroundedRatio,
                -RequiredSupportForcePerWheel * static_cast<float>(GroundedWheels) * 0.75f,
                0.0f);
            TotalForce += Up * GroundedVerticalDampingForce;
        }
    }

    const float Speed = StablePhysicsLinearVelocity.Size();
    if (Speed > 50.0f && MaxAerodynamicDrag > 0.0f && AerodynamicDragCoefficient > 0.0f)
    {
        const float DragMagnitude = FMath::Clamp(Speed * Speed * FMath::Max(0.0f, AerodynamicDragCoefficient) * MassScale, 0.0f, MaxAerodynamicDrag * MassScale);
        TotalForce += -StablePhysicsLinearVelocity.GetSafeNormal() * DragMagnitude;
    }
    if (GroundedWheels > 0)
    {
        const float DownforceSpeedAlpha = ComputeVehicleAeroSpeedAlpha(Speed, MinimumDownforceSpeed);
        float Downforce = 0.0f;
        if (DownforceSpeedAlpha > 0.0f && GroundedDownforceCoefficient > 0.0f)
        {
            Downforce += FMath::Clamp(Speed * Speed * GroundedDownforceCoefficient * MassScale, 0.0f, MaxGroundedDownforce * MassScale) * GroundedRatio * DownforceSpeedAlpha;
        }
        if (DownforceSpeedAlpha > 0.0f && SmoothedThrottleInput > 0.02f && ThrottleFrontDownforce > 0.0f)
        {
            Downforce += FMath::Clamp(SmoothedThrottleInput * ThrottleFrontDownforce * 0.65f * MassScale, 0.0f, MaxGroundedDownforce * 0.45f * MassScale) * GroundedRatio * DownforceSpeedAlpha;
        }
        TotalForce += -Up * Downforce;
    }

    StablePhysicsLinearVelocity += (TotalForce / SafeMassKg) * SafeDeltaSeconds;
    const float MaxStableFallSpeed = FMath::Max(100.0f, StableMaxVerticalSpeed);
    float MaxStableRiseSpeed = FMath::Max(
        20.0f,
        StableMaxClimbRate > 0.0f ? FMath::Min(StableMaxVerticalSpeed, StableMaxClimbRate) : StableMaxVerticalSpeed);
    if (GroundedWheels > 0)
    {
        MaxStableRiseSpeed = FMath::Min(MaxStableRiseSpeed, FMath::Max(20.0f, StableMaxGroundedUpSpeed));
    }
    StablePhysicsLinearVelocity.Z = FMath::Clamp(StablePhysicsLinearVelocity.Z, -MaxStableFallSpeed, MaxStableRiseSpeed);

    if (GroundedWheels > 0)
    {
        const float UpVelocity = FVector::DotProduct(StablePhysicsLinearVelocity, Up);
        const float AllowedGroundedUpSpeed = FMath::Max(20.0f, StableMaxGroundedUpSpeed);
        if (UpVelocity > AllowedGroundedUpSpeed)
        {
            StablePhysicsLinearVelocity -= Up * (UpVelocity - AllowedGroundedUpSpeed);
        }
    }

    FVector PlanarVelocity(StablePhysicsLinearVelocity.X, StablePhysicsLinearVelocity.Y, 0.0f);
    const float MaxPlanarSpeed = FMath::Max(500.0f, MaxSpeedForward) * 1.12f;
    if (PlanarVelocity.SizeSquared2D() > FMath::Square(MaxPlanarSpeed))
    {
        PlanarVelocity = PlanarVelocity.GetSafeNormal2D() * MaxPlanarSpeed;
        StablePhysicsLinearVelocity.X = PlanarVelocity.X;
        StablePhysicsLinearVelocity.Y = PlanarVelocity.Y;
    }

    const float FullX = FMath::Max(1.0f, BodyExtent.X * 2.0f);
    const float FullY = FMath::Max(1.0f, BodyExtent.Y * 2.0f);
    const float FullZ = FMath::Max(1.0f, BodyExtent.Z * 2.0f);
    const FVector LocalInertia(
        SafeMassKg * (FullY * FullY + FullZ * FullZ) / 12.0f,
        SafeMassKg * (FullX * FullX + FullZ * FullZ) / 12.0f,
        SafeMassKg * (FullX * FullX + FullY * FullY) / 12.0f);
    const FVector LocalTorque = CurrentQuat.UnrotateVector(TotalTorque);
    const FVector LocalAngularAcceleration(
        LocalTorque.X / FMath::Max(1.0f, LocalInertia.X),
        LocalTorque.Y / FMath::Max(1.0f, LocalInertia.Y),
        LocalTorque.Z / FMath::Max(1.0f, LocalInertia.Z));
    StablePhysicsAngularVelocity += CurrentQuat.RotateVector(LocalAngularAcceleration) * SafeDeltaSeconds;

    FVector LocalAngularVelocity = CurrentQuat.UnrotateVector(StablePhysicsAngularVelocity);
    const float MaxPitchRollRate = FMath::Max(0.1f, StableMaxPitchRollRateRadians);
    LocalAngularVelocity.X = FMath::Clamp(LocalAngularVelocity.X, -MaxPitchRollRate, MaxPitchRollRate);
    LocalAngularVelocity.Y = FMath::Clamp(LocalAngularVelocity.Y, -MaxPitchRollRate, MaxPitchRollRate);
    LocalAngularVelocity.Z = FMath::Clamp(LocalAngularVelocity.Z, -FMath::Max(0.5f, MaxAngularVelocityRadians), FMath::Max(0.5f, MaxAngularVelocityRadians));
    StablePhysicsAngularVelocity = CurrentQuat.RotateVector(LocalAngularVelocity);

    const FVector RequestedLocation = CurrentLocation + StablePhysicsLinearVelocity * SafeDeltaSeconds;
    FQuat RequestedQuat = CurrentQuat;
    const float AngularSpeed = StablePhysicsAngularVelocity.Size();
    if (AngularSpeed > KINDA_SMALL_NUMBER)
    {
        RequestedQuat = (FQuat(StablePhysicsAngularVelocity.GetSafeNormal(), AngularSpeed * SafeDeltaSeconds) * CurrentQuat).GetNormalized();
    }

    FRotator RequestedRotator = RequestedQuat.Rotator();
    RequestedRotator.Pitch = FMath::Clamp(FMath::UnwindDegrees(RequestedRotator.Pitch), -45.0f, 45.0f);
    RequestedRotator.Roll = FMath::Clamp(FMath::UnwindDegrees(RequestedRotator.Roll), -40.0f, 40.0f);
    RequestedQuat = RequestedRotator.Quaternion();

    Body->SetEnableGravity(false);
    FHitResult MoveHit;
    Body->MoveComponent(
        RequestedLocation - CurrentLocation,
        RequestedQuat,
        true,
        &MoveHit,
        MOVECOMP_NoFlags,
        ETeleportType::None);

    FVector ActualLocation = GetActorLocation();
    FQuat ActualQuat = GetActorQuat().GetNormalized();
    if (MoveHit.bBlockingHit)
    {
        const FVector HitNormal = MoveHit.Normal.GetSafeNormal();
        const float VelocityIntoSurface = FVector::DotProduct(StablePhysicsLinearVelocity, HitNormal);
        if (VelocityIntoSurface < 0.0f)
        {
            StablePhysicsLinearVelocity -= HitNormal * VelocityIntoSurface;
        }
        if (HitNormal.Z < 0.35f)
        {
            StablePhysicsLinearVelocity *= 0.55f;
        }
        else if (StablePhysicsLinearVelocity.Z > 0.0f && GroundedWheels > 0)
        {
            StablePhysicsLinearVelocity.Z *= 0.35f;
        }
        StablePhysicsAngularVelocity *= 0.65f;
        ActualLocation = GetActorLocation();
        ActualQuat = GetActorQuat().GetNormalized();
    }

    FVector ActualVelocity = (ActualLocation - CurrentLocation) / FMath::Max(0.001f, SafeDeltaSeconds);
    if (GroundedWheels > 0)
    {
        const float AllowedGroundedUpSpeed = FMath::Max(20.0f, StableMaxGroundedUpSpeed);
        const float ActualUpVelocity = FVector::DotProduct(ActualVelocity, Up);
        if (ActualUpVelocity > AllowedGroundedUpSpeed)
        {
            ActualVelocity -= Up * (ActualUpVelocity - AllowedGroundedUpSpeed);
        }
    }
    StablePhysicsLinearVelocity = FMath::Lerp(StablePhysicsLinearVelocity, ActualVelocity, MoveHit.bBlockingHit ? 0.65f : 0.20f);
    if (GroundedWheels > 0)
    {
        const float AllowedGroundedUpSpeed = FMath::Max(20.0f, StableMaxGroundedUpSpeed);
        const float SmoothedUpVelocity = FVector::DotProduct(StablePhysicsLinearVelocity, Up);
        if (SmoothedUpVelocity > AllowedGroundedUpSpeed)
        {
            StablePhysicsLinearVelocity -= Up * (SmoothedUpVelocity - AllowedGroundedUpSpeed);
        }
    }


    const FTransform FinalTransform(ActualQuat, ActualLocation);
    const FVector FinalUp = FinalTransform.GetUnitAxis(EAxis::Z).GetSafeNormal();
    for (int32 WheelIndex = 0; WheelIndex < WheelOffsets.Num(); ++WheelIndex)
    {
        FHitResult FinalHit;
        float FinalSpringLength = SuspensionTravel;
        const bool bWheelHit = TraceWheel(FinalTransform, WheelIndex, FinalHit, FinalSpringLength);
        WheelGrounded[WheelIndex] = bWheelHit;
        if (bWheelHit)
        {
            const float MinVisualSpringLength = GetMinimumWheelSpringLength(WheelIndex);
            const float MaxVisualSpringLength = GetEffectiveSuspensionRestLength(WheelIndex);
            WheelSpringLengths[WheelIndex] = FMath::Clamp(FinalSpringLength, MinVisualSpringLength, MaxVisualSpringLength);
        }
        else
        {
            const float MaxVisualSpringLength = GetEffectiveSuspensionRestLength(WheelIndex);
            const float PreviousSpringLength = WheelSpringLengths.IsValidIndex(WheelIndex) ? WheelSpringLengths[WheelIndex] : MaxVisualSpringLength;
            WheelSpringLengths[WheelIndex] = FMath::FInterpTo(PreviousSpringLength, MaxVisualSpringLength, SafeDeltaSeconds, FMath::Max(0.1f, SuspensionContactSmoothingSpeed));
        }
    }

    StablePlanarVelocity = FVector(StablePhysicsLinearVelocity.X, StablePhysicsLinearVelocity.Y, 0.0f);
    StableVerticalVelocity = StablePhysicsLinearVelocity.Z;
    SmoothedStableYawRate = FVector::DotProduct(StablePhysicsAngularVelocity, FinalUp);
    bStablePlanarVelocityInitialized = true;

    if (Body->IsSimulatingPhysics())
    {
        Body->SetPhysicsLinearVelocity(StablePhysicsLinearVelocity);
        Body->SetPhysicsAngularVelocityInRadians(StablePhysicsAngularVelocity);
        Body->WakeRigidBody();
    }
}

void AVehiclePawn::ApplyStableVehicleGrounding(float DeltaSeconds)
{
    // Disabled: snap-grounding can inject artificial vertical motion. Runtime driving uses UpdateStableWheelVehicle.
    return;

    if (!bUseStableGroundRideHeight || !IsValid(Body))
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    const float SafeDeltaSeconds = FMath::Max(0.0f, DeltaSeconds);
    const float StableSpringLength = GetStableWheelVisualSpringLength();
    const float SafeWheelRadius = GetEffectiveWheelRadius();
    const FVector PreservedLinearVelocity = Body->IsSimulatingPhysics() ? Body->GetPhysicsLinearVelocity() : FVector::ZeroVector;
    const float PreservedYawAngularVelocity = Body->IsSimulatingPhysics()
        ? FVector::DotProduct(Body->GetPhysicsAngularVelocityInRadians(), FVector::UpVector)
        : 0.0f;

    if (bLockBodyPitchAndRoll)
    {
        const FRotator CurrentRotation = GetActorRotation();
        if (!FMath::IsNearlyZero(CurrentRotation.Pitch, 0.01f) || !FMath::IsNearlyZero(CurrentRotation.Roll, 0.01f))
        {
            SetActorRotation(FRotator(0.0f, CurrentRotation.Yaw, 0.0f), ETeleportType::TeleportPhysics);
        }
    }

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(VehicleStableGroundingTrace), false, this);
    QueryParams.AddIgnoredActor(this);

    const FTransform BodyTransform = Body->GetComponentTransform();
    const float TraceUp = FMath::Max(FMath::Max(20.0f, StableGroundTraceUp), SafeWheelRadius + 80.0f);
    const float TraceDown = FMath::Max(FMath::Max(120.0f, StableGroundTraceDown), StableSpringLength + SafeWheelRadius + SuspensionTraceExtra + 220.0f);
    const float RequiredNormalZ = FMath::Clamp(MinSuspensionHitNormalDot, 0.0f, 1.0f);
    const float GroundBuffer = FMath::Max(0.0f, StableRideHeightGroundBuffer);

    bool bHasTargetHeight = false;
    float TargetActorZ = GetActorLocation().Z;
    auto AddTargetHeight = [&](float CandidateZ)
    {
        TargetActorZ = bHasTargetHeight ? FMath::Max(TargetActorZ, CandidateZ) : CandidateZ;
        bHasTargetHeight = true;
    };

    for (const FVector& LocalWheelOffset : WheelOffsets)
    {
        const FVector MountWorld = BodyTransform.TransformPosition(LocalWheelOffset);
        const FVector TraceStart = MountWorld + FVector::UpVector * TraceUp;
        const FVector TraceEnd = MountWorld - FVector::UpVector * TraceDown;

        FHitResult Hit;
        if (!FPhysicsHelper::Raycast(this, TraceStart, TraceEnd, QueryParams, Hit) || !Hit.bBlockingHit)
        {
            continue;
        }

        if (Hit.ImpactNormal.Z < RequiredNormalZ)
        {
            continue;
        }

        // With pitch/roll locked, local Z is stable. Place the actor so the wheel visual center
        // stays one radius above the floor at the fixed visual suspension length.
        AddTargetHeight(Hit.ImpactPoint.Z + GroundBuffer + StableSpringLength + SafeWheelRadius - LocalWheelOffset.Z);
        if (!LoadedWheelVisualRestBounds.IsValid)
        {
            AddTargetHeight(Hit.ImpactPoint.Z + GroundBuffer + BodyExtent.Z
                + FMath::Clamp(GetPhysicsBodyGroundClearance(), 1.0f, 8.0f));
        }
    }

    if (!bHasTargetHeight)
    {
        const FVector ActorLocation = GetActorLocation();
        const FVector TraceStart = ActorLocation + FVector::UpVector * TraceUp;
        const FVector TraceEnd = ActorLocation - FVector::UpVector * TraceDown;
        FHitResult Hit;
        if (FPhysicsHelper::Raycast(this, TraceStart, TraceEnd, QueryParams, Hit) && Hit.bBlockingHit && Hit.ImpactNormal.Z >= RequiredNormalZ)
        {
            AddTargetHeight(Hit.ImpactPoint.Z + GroundBuffer + GetDesiredCenterHeightAboveGround());
        }
    }

    if (bHasTargetHeight)
    {
        const FVector CurrentLocation = GetActorLocation();
        const float SnapTolerance = FMath::Max(0.1f, GroundBuffer * 0.25f + SafeDeltaSeconds * 0.0f);
        const FRotator CurrentRotation = GetActorRotation();
        const FRotator StableRotation = bLockBodyPitchAndRoll
            ? FRotator(0.0f, CurrentRotation.Yaw, 0.0f)
            : CurrentRotation;
        if (!FMath::IsNearlyEqual(CurrentLocation.Z, TargetActorZ, SnapTolerance)
            || (bLockBodyPitchAndRoll && (!FMath::IsNearlyZero(CurrentRotation.Pitch, 0.01f) || !FMath::IsNearlyZero(CurrentRotation.Roll, 0.01f))))
        {
            SetActorLocationAndRotation(FVector(CurrentLocation.X, CurrentLocation.Y, TargetActorZ), StableRotation, false, nullptr, ETeleportType::TeleportPhysics);
        }
    }

    if (Body->IsSimulatingPhysics())
    {
        Body->SetEnableGravity(false);

        FVector LinearVelocity = Body->GetPhysicsLinearVelocity();
        LinearVelocity.X = PreservedLinearVelocity.X;
        LinearVelocity.Y = PreservedLinearVelocity.Y;
        LinearVelocity.Z = 0.0f;
        Body->SetPhysicsLinearVelocity(LinearVelocity);

        if (bLockBodyPitchAndRoll)
        {
            // Kill only pitch/roll angular velocity. Preserve yaw so steering can keep rotating the vehicle.
            Body->SetPhysicsAngularVelocityInRadians(FVector::UpVector * PreservedYawAngularVelocity);
        }
    }

    WheelSpringLengths.SetNum(WheelOffsets.Num());
    WheelVisualSpringLengths.SetNum(WheelOffsets.Num());
    for (int32 WheelIndex = 0; WheelIndex < WheelSpringLengths.Num(); ++WheelIndex)
    {
        WheelSpringLengths[WheelIndex] = StableSpringLength;
        WheelVisualSpringLengths[WheelIndex] = StableSpringLength;
    }

    const float StaticSupportForce = WheelOffsets.Num() > 0
        ? FMath::Max(1.0f, VehicleMassKg) * FMath::Max(1.0f, FMath::Abs(World->GetGravityZ())) / static_cast<float>(WheelOffsets.Num())
        : 0.0f;
    WheelSuspensionForces.SetNum(WheelOffsets.Num());
    for (float& WheelSuspensionForce : WheelSuspensionForces)
    {
        WheelSuspensionForce = StaticSupportForce;
    }
}

FPlacedObjectRecord AVehiclePawn::ToPlacementRecord(int32 VehicleRecordIndex) const
{
    FPlacedObjectRecord Record;
    Record.ObjectName = ObjectName.IsEmpty()
        ? (VehicleRecordIndex == 0 ? TEXT("Vehicle") : TEXT("Vehicle;INST"))
        : ObjectName;
    Record.BaseName = BaseName.IsEmpty() ? TEXT("Vehicle") : BaseName;
    Record.SourceFile = SourceFilePath;
    Record.Kind = EPlacedObjectKind::Vehicle;
    Record.Transform = GetActorTransform();
    return Record;
}

bool AVehiclePawn::EnterVehicle(APlayerController* PlayerController, APawn* PreviousPawn)
{
    if (!bVehicleModelLoaded || !IsValid(PlayerController) || IsOccupied())
    {
        return false;
    }

    if (ACharacterController* CharacterPawn = Cast<ACharacterController>(PreviousPawn))
    {
        if (UCharacterComponent* CharacterState = CharacterPawn->GetCharacterComponent())
        {
            if (CharacterState->IsRagdollActive() || CharacterState->IsGettingUp())
            {
                return false;
            }
        }

        CharacterPawn->ClearTransientInputState();
    }

    if (APlayerCharacterController* PlayerCharacterController = Cast<APlayerCharacterController>(PlayerController))
    {
        PlayerCharacterController->ClearLatchedMovementInput();
    }

    OccupyingController = PlayerController;
    StoredPawn = PreviousPawn;
    StoredControlRotation = PlayerController->GetControlRotation();
    bHasStoredControlRotation = true;
    bHasStoredPawnTransform = IsValid(StoredPawn);
    if (IsValid(StoredPawn))
    {
        StoredPawnTransformBeforeEnter = StoredPawn->GetActorTransform();
        StoredPawn->SetActorHiddenInGame(true);
        StoredPawn->SetActorEnableCollision(false);
        StoredPawn->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
    }

    ClearDriveInput();
    PlayerController->Possess(this);
    PlayerController->SetViewTarget(this);
    if (bHasStoredControlRotation)
    {
        PlayerController->SetControlRotation(StoredControlRotation);
    }
    if (UInstancedEntitySubsystem* InstancedEntities = UInstancedEntitySubsystem::Get(this))
    {
        InstancedEntities->SetEntityAlwaysRelevant(InstancedRenderRegistrationId, true);
    }
    return true;
}

void AVehiclePawn::ExitVehicle()
{
    if (!IsValid(OccupyingController))
    {
        return;
    }

    ClearDriveInput();

    if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(OccupyingController))
    {
        PlayerController->ClearLatchedMovementInput();
    }

    if (ACharacterController* CharacterPawn = Cast<ACharacterController>(StoredPawn))
    {
        CharacterPawn->ClearTransientInputState();
    }

    if (IsValid(StoredPawn))
    {
        const FRotator RestoreRotation = bHasStoredControlRotation
            ? FRotator(0.0f, StoredControlRotation.Yaw, 0.0f)
            : (bHasStoredPawnTransform ? StoredPawnTransformBeforeEnter.GetRotation().Rotator() : StoredPawn->GetActorRotation());

        FVector SafeExitLocation = GetExitLocation();
        FRotator SafeExitRotation = RestoreRotation;
        if (!FindSafeExitTransform(StoredPawn, SafeExitLocation, SafeExitRotation))
        {
            UE_LOG(LogTemp, Warning, TEXT("VehiclePawn: no ground or water exit location found within one vehicle length. Staying in vehicle."));
            return;
        }

        float ExitCapsuleRadius = 34.0f;
        float ExitCapsuleHalfHeight = 88.0f;
        GetVehicleExitPawnCapsuleSize(StoredPawn.Get(), ExitCapsuleRadius, ExitCapsuleHalfHeight);

        float ExitWaterLevel = 0.0f;
        const bool bExitIntoWater = IsVehicleExitLocationInWater(this, SafeExitLocation, ExitCapsuleHalfHeight, ExitWaterLevel);

        StoredPawn->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
        StoredPawn->SetActorLocationAndRotation(SafeExitLocation, SafeExitRotation, false, nullptr, ETeleportType::TeleportPhysics);
        StoredPawn->SetActorHiddenInGame(false);
        StoredPawn->SetActorEnableCollision(true);
        OccupyingController->Possess(StoredPawn);
        RestoreStoredPawnCamera(OccupyingController, StoredPawn);

        if (bExitIntoWater)
        {
            ApplyVehicleWaterExitState(StoredPawn.Get(), ExitWaterLevel);
        }

        // After collision is re-enabled, resync overlaps so water volumes can notify any
        // water-aware components that were hidden while the pawn was seated in the vehicle.
        AWaterActor::CheckOverlappingWater(StoredPawn.Get());

        if (APlayerCharacterController* PlayerController = Cast<APlayerCharacterController>(OccupyingController))
        {
            PlayerController->ApplyGameInputMode();
            PlayerController->ClearLatchedMovementInput();
        }
    }

    OccupyingController = nullptr;
    StoredPawn = nullptr;
    StoredControlRotation = FRotator::ZeroRotator;
    StoredPawnTransformBeforeEnter = FTransform::Identity;
    bHasStoredControlRotation = false;
    bHasStoredPawnTransform = false;
    if (UInstancedEntitySubsystem* InstancedEntities = UInstancedEntitySubsystem::Get(this))
    {
        InstancedEntities->SetEntityAlwaysRelevant(InstancedRenderRegistrationId, false);
    }
}

FVector AVehiclePawn::GetExitLocation() const
{
    return GetActorLocation() + GetActorRightVector() * 220.0f + FVector(0.0f, 0.0f, 80.0f);
}

bool AVehiclePawn::FindSafeExitTransform(APawn* PawnToExit, FVector& OutLocation, FRotator& OutRotation) const
{
    if (!IsValid(PawnToExit))
    {
        return false;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return false;
    }

    float CapsuleRadius = 34.0f;
    float CapsuleHalfHeight = 88.0f;
    GetVehicleExitPawnCapsuleSize(PawnToExit, CapsuleRadius, CapsuleHalfHeight);

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(VehicleExitTrace), false, this);
    QueryParams.AddIgnoredActor(this);
    QueryParams.AddIgnoredActor(PawnToExit);

    const FCollisionShape CapsuleShape = FCollisionShape::MakeCapsule(CapsuleRadius, CapsuleHalfHeight);
    const FVector VehicleLocation = GetActorLocation();
    const FVector Forward = GetActorForwardVector().GetSafeNormal2D();
    const FVector Right = GetActorRightVector().GetSafeNormal2D();
    const float OneVehicleLength = FMath::Max(BodyExtent.X * 2.0f, BodyExtent.Y * 2.0f) + CapsuleRadius + 80.0f;
    const float MinSideDistance = FMath::Clamp(BodyExtent.Y + CapsuleRadius + 65.0f, CapsuleRadius + 60.0f, OneVehicleLength);
    const float MinFrontBackDistance = FMath::Clamp(BodyExtent.X + CapsuleRadius + 55.0f, CapsuleRadius + 70.0f, OneVehicleLength);

    struct FExitCandidate
    {
        FVector Location = FVector::ZeroVector;
        float DistanceSquared = 0.0f;
    };

    auto AddCandidate = [&](TArray<FExitCandidate>& OutCandidates, const FVector& Direction, float Distance)
    {
        const FVector FlatDirection = Direction.GetSafeNormal2D();
        if (FlatDirection.IsNearlyZero() || Distance > OneVehicleLength + KINDA_SMALL_NUMBER)
        {
            return;
        }

        FExitCandidate Candidate;
        Candidate.Location = VehicleLocation + FlatDirection * Distance;
        Candidate.DistanceSquared = FVector::DistSquared2D(Candidate.Location, VehicleLocation);

        for (const FExitCandidate& Existing : OutCandidates)
        {
            if (FVector::DistSquared2D(Existing.Location, Candidate.Location) <= FMath::Square(8.0f))
            {
                return;
            }
        }

        OutCandidates.Add(Candidate);
    };

    auto SortCandidatesByDistance = [](TArray<FExitCandidate>& InOutCandidates)
    {
        InOutCandidates.Sort([](const FExitCandidate& A, const FExitCandidate& B)
        {
            return A.DistanceSquared < B.DistanceSquared;
        });
    };

    const FVector Directions[] = {
        Right,
        -Right,
        Forward,
        -Forward,
        (Right + Forward).GetSafeNormal2D(),
        (-Right + Forward).GetSafeNormal2D(),
        (Right - Forward).GetSafeNormal2D(),
        (-Right - Forward).GetSafeNormal2D()
    };

    TArray<FExitCandidate> LocalCandidates;
    AddCandidate(LocalCandidates, Right, MinSideDistance);
    AddCandidate(LocalCandidates, -Right, MinSideDistance);
    AddCandidate(LocalCandidates, -Forward, MinFrontBackDistance);
    AddCandidate(LocalCandidates, Forward, MinFrontBackDistance);

    const float LocalDiagonalDistance = FMath::Min(OneVehicleLength, FMath::Max(MinSideDistance, MinFrontBackDistance) * 0.90f);
    AddCandidate(LocalCandidates, (Right + Forward).GetSafeNormal2D(), LocalDiagonalDistance);
    AddCandidate(LocalCandidates, (-Right + Forward).GetSafeNormal2D(), LocalDiagonalDistance);
    AddCandidate(LocalCandidates, (Right - Forward).GetSafeNormal2D(), LocalDiagonalDistance);
    AddCandidate(LocalCandidates, (-Right - Forward).GetSafeNormal2D(), LocalDiagonalDistance);
    SortCandidatesByDistance(LocalCandidates);

    TArray<FExitCandidate> BroadCandidates = LocalCandidates;
    const float Step = FMath::Max(45.0f, CapsuleRadius + 22.0f);
    for (float Distance = FMath::Min(MinSideDistance, MinFrontBackDistance) + Step; Distance <= OneVehicleLength + KINDA_SMALL_NUMBER; Distance += Step)
    {
        for (const FVector& Direction : Directions)
        {
            AddCandidate(BroadCandidates, Direction, Distance);
        }
    }
    SortCandidatesByDistance(BroadCandidates);

    const float RequiredWalkableZ = 0.55f;
    const FQuat CandidateRotation = OutRotation.Quaternion();

    // Keep scene queries on the game thread. UWorld trace calls are not safe to fan out through
    // arbitrary worker threads during possession changes, so the search is batched, sorted, and bounded.
    auto TryCandidateSet = [&](const TArray<FExitCandidate>& Candidates, float TraceUp, float TraceDown, float MaxGroundHeightAboveVehicle) -> bool
    {
        for (const FExitCandidate& Candidate : Candidates)
        {
            const FVector TraceStart(Candidate.Location.X, Candidate.Location.Y, VehicleLocation.Z + TraceUp);
            const FVector TraceEnd(Candidate.Location.X, Candidate.Location.Y, VehicleLocation.Z - TraceDown);

            FHitResult GroundHit;
            if (!FPhysicsHelper::Raycast(this, TraceStart, TraceEnd, QueryParams, GroundHit))
            {
                continue;
            }

            if (!GroundHit.bBlockingHit || GroundHit.bStartPenetrating || GroundHit.ImpactNormal.Z < RequiredWalkableZ)
            {
                continue;
            }

            // A high hit is usually the top of a roof/ceiling above the car. Reject it so exiting in a garage
            // searches near the vehicle floor first instead of teleporting the pawn onto the ceiling.
            if (GroundHit.ImpactPoint.Z - VehicleLocation.Z > MaxGroundHeightAboveVehicle)
            {
                continue;
            }

            const FVector CandidateActorLocation = GroundHit.ImpactPoint + FVector::UpVector * (CapsuleHalfHeight + 2.0f);
            if (World->OverlapBlockingTestByChannel(CandidateActorLocation, CandidateRotation, ECC_Pawn, CapsuleShape, QueryParams))
            {
                continue;
            }

            // A small downward sweep verifies that the capsule can stand on the traced surface and is not being
            // placed on the far side of a thin one-sided polygon.
            FHitResult FloorSweep;
            const FVector SweepStart = CandidateActorLocation + FVector::UpVector * 5.0f;
            const FVector SweepEnd = CandidateActorLocation - FVector::UpVector * 12.0f;
            if (World->SweepSingleByChannel(FloorSweep, SweepStart, SweepEnd, CandidateRotation, ECC_Pawn, CapsuleShape, QueryParams)
                && FloorSweep.bBlockingHit
                && FloorSweep.ImpactNormal.Z < RequiredWalkableZ)
            {
                continue;
            }

            OutLocation = CandidateActorLocation;
            return true;
        }

        return false;
    };

    // First pass: short vertical traces around the vehicle. This avoids selecting roof/ceiling hits in tight spaces.
    const float LocalTraceUp = FMath::Clamp(BodyExtent.Z + 48.0f, 72.0f, 132.0f);
    const float LocalTraceDown = FMath::Max(280.0f, BodyExtent.Z + CapsuleHalfHeight + 180.0f);
    const float LocalMaxGroundUp = FMath::Max(BodyExtent.Z + 45.0f, CapsuleHalfHeight * 0.65f);
    if (TryCandidateSet(LocalCandidates, LocalTraceUp, LocalTraceDown, LocalMaxGroundUp))
    {
        return true;
    }

    // Second pass: wider search, still rejecting very high hits so ceiling tops are not considered exits.
    const float BroadTraceUp = BodyExtent.Z + CapsuleHalfHeight + 420.0f;
    const float BroadTraceDown = BodyExtent.Z + CapsuleHalfHeight + 740.0f;
    const float BroadMaxGroundUp = FMath::Max(150.0f, BodyExtent.Z + CapsuleHalfHeight * 0.75f);
    if (TryCandidateSet(BroadCandidates, BroadTraceUp, BroadTraceDown, BroadMaxGroundUp))
    {
        return true;
    }

    float VehicleWaterLevel = 0.0f;
    bool bVehicleInWater = false;
    if (IsValid(BuoyancyComponent) && BuoyancyComponent->IsInWater())
    {
        VehicleWaterLevel = BuoyancyComponent->GetWaterLevel();
        bVehicleInWater = true;
    }

    const FVector VehicleBottomLocation = VehicleLocation - FVector::UpVector * FMath::Max(BodyExtent.Z, CapsuleHalfHeight * 0.5f);
    bVehicleInWater = AWaterActor::FindWaterLevelAtLocation(this, VehicleLocation, VehicleWaterLevel)
        || AWaterActor::FindWaterLevelAtLocation(this, VehicleBottomLocation, VehicleWaterLevel)
        || bVehicleInWater;

    if (!bVehicleInWater)
    {
        return false;
    }

    // No walkable floor was found. If the vehicle is submerged, falling back to a water
    // exit lets the player leave the car and swim instead of being trapped by the ground-only search.
    const float ExitSubmergeDepth = FMath::Clamp(CapsuleHalfHeight * 0.30f, 24.0f, 64.0f);
    for (const FExitCandidate& Candidate : BroadCandidates)
    {
        float CandidateWaterLevel = VehicleWaterLevel;
        FVector CandidateActorLocation(Candidate.Location.X, Candidate.Location.Y, CandidateWaterLevel - ExitSubmergeDepth);
        if (!IsVehicleExitLocationInWater(this, CandidateActorLocation, CapsuleHalfHeight, CandidateWaterLevel))
        {
            continue;
        }

        CandidateActorLocation.Z = CandidateWaterLevel - ExitSubmergeDepth;
        if (World->OverlapBlockingTestByChannel(CandidateActorLocation, CandidateRotation, ECC_Pawn, CapsuleShape, QueryParams))
        {
            continue;
        }

        OutLocation = CandidateActorLocation;
        return true;
    }

    return false;
}

void AVehiclePawn::AddVehicleForce(const FVector& Force)
{
    if (!IsValid(Body) || Force.IsNearlyZero(1.0f))
    {
        return;
    }

    const float StepSeconds = CurrentVehiclePhysicsStepSeconds > 0.0f
        ? CurrentVehiclePhysicsStepSeconds
        : FMath::Clamp(GetWorld() ? GetWorld()->GetDeltaSeconds() : MaxVehiclePhysicsSubstepSeconds, 0.001f, 0.05f);

    Body->AddImpulse(Force * StepSeconds, NAME_None, false);
}

void AVehiclePawn::AddVehicleForceAtLocation(const FVector& Force, const FVector& Location)
{
    if (!IsValid(Body) || Force.IsNearlyZero(1.0f))
    {
        return;
    }

    const float StepSeconds = CurrentVehiclePhysicsStepSeconds > 0.0f
        ? CurrentVehiclePhysicsStepSeconds
        : FMath::Clamp(GetWorld() ? GetWorld()->GetDeltaSeconds() : MaxVehiclePhysicsSubstepSeconds, 0.001f, 0.05f);

    Body->AddImpulseAtLocation(Force * StepSeconds, Location);
}

void AVehiclePawn::AddVehicleTorqueInRadians(const FVector& Torque)
{
    if (!IsValid(Body) || Torque.IsNearlyZero(1.0f))
    {
        return;
    }

    const float StepSeconds = CurrentVehiclePhysicsStepSeconds > 0.0f
        ? CurrentVehiclePhysicsStepSeconds
        : FMath::Clamp(GetWorld() ? GetWorld()->GetDeltaSeconds() : MaxVehiclePhysicsSubstepSeconds, 0.001f, 0.05f);

    Body->AddAngularImpulseInRadians(Torque * StepSeconds, NAME_None, false);
}

void AVehiclePawn::ApplyChassisClearanceProtection(UWorld* World, const FTransform& BodyTransform, const FCollisionQueryParams& QueryParams)
{
    if (!World || !IsValid(Body) || MaxChassisAntiGroundStickForce <= 0.0f || ChassisAntiGroundStickStrength <= 0.0f)
    {
        return;
    }

    const float MassScale = GetVehicleMassScale();
    // Bottom-out guard only. Use a small real clearance here instead of the full spawn ride-height
    // so this does not turn back into the old ground-snap / hover behavior.
    const float DesiredClearance = FMath::Clamp(GetPhysicsBodyGroundClearance(), 1.0f, 8.0f);
    const float Strength = FMath::Max(0.0f, ChassisAntiGroundStickStrength) * MassScale;
    const float Damping = FMath::Max(0.0f, ChassisAntiGroundStickDamping) * MassScale;
    const float MaxForcePerPoint = FMath::Max(1.0f, MaxChassisAntiGroundStickForce * MassScale / 5.0f);

    const FVector LocalBottomPoints[] = {
        FVector(BodyExtent.X * 0.82f, BodyExtent.Y * 0.82f, -BodyExtent.Z),
        FVector(BodyExtent.X * 0.82f, -BodyExtent.Y * 0.82f, -BodyExtent.Z),
        FVector(-BodyExtent.X * 0.82f, BodyExtent.Y * 0.82f, -BodyExtent.Z),
        FVector(-BodyExtent.X * 0.82f, -BodyExtent.Y * 0.82f, -BodyExtent.Z),
        FVector(0.0f, 0.0f, -BodyExtent.Z)
    };

    for (const FVector& LocalPoint : LocalBottomPoints)
    {
        const FVector BottomWorld = BodyTransform.TransformPosition(LocalPoint);
        const FVector TraceStart = BottomWorld + FVector::UpVector * 18.0f;
        const FVector TraceEnd = BottomWorld - FVector::UpVector * (DesiredClearance + 100.0f);

        FHitResult Hit;
        if (!FPhysicsHelper::Raycast(this, TraceStart, TraceEnd, QueryParams, Hit))
        {
            continue;
        }

        const FVector HitNormal = Hit.ImpactNormal.GetSafeNormal();
        if (FVector::DotProduct(HitNormal, FVector::UpVector) < FMath::Max(0.54f, FMath::Clamp(MinSuspensionHitNormalDot, 0.0f, 1.0f) - 0.08f))
        {
            continue;
        }

        // Measure clearance along the road normal, not just world-up. On an uphill face world-up can
        // say the floor is clear while the chassis is actually pressing into the ramp surface.
        const float CurrentClearance = FVector::DotProduct(BottomWorld - Hit.ImpactPoint, HitNormal);
        const float ClearanceError = DesiredClearance - CurrentClearance;
        if (ClearanceError <= 0.0f)
        {
            continue;
        }

        const FVector ClearanceAxis = FMath::Lerp(HitNormal, FVector::UpVector, 0.26f).GetSafeNormal();
        const float LiftSpeedAtPoint = FVector::DotProduct(Body->GetPhysicsLinearVelocityAtPoint(BottomWorld), ClearanceAxis);
        const float LiftForce = FMath::Clamp(ClearanceError * Strength - LiftSpeedAtPoint * Damping, 0.0f, MaxForcePerPoint);
        if (LiftForce > KINDA_SMALL_NUMBER)
        {
            AddVehicleForceAtLocation(ClearanceAxis * LiftForce, BottomWorld);
        }
    }
}

void AVehiclePawn::ApplySuspensionAndDrive(float DeltaSeconds)
{
    if (!IsValid(Body) || !Body->IsSimulatingPhysics())
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World || DeltaSeconds <= 0.0f)
    {
        return;
    }

    const float ForceControlDeltaSeconds = FMath::Clamp(DeltaSeconds, 0.001f, FMath::Clamp(MaxWheelForceControlDeltaSeconds, 0.004f, 0.05f));

    const FTransform BodyTransform = Body->GetComponentTransform();
    const FVector Up = Body->GetUpVector().GetSafeNormal();
    const FVector Forward = Body->GetForwardVector().GetSafeNormal();
    const FVector BodyVelocity = Body->GetPhysicsLinearVelocity();
    const FVector AngularVelocity = Body->GetPhysicsAngularVelocityInRadians();
    const float BodyForwardSpeed = FVector::DotProduct(BodyVelocity, Forward);
    const float AbsBodyForwardSpeed = FMath::Abs(BodyForwardSpeed);
    const float MassScale = GetVehicleMassScale();
    const float GravityAcceleration = FMath::Max(1.0f, FMath::Abs(World->GetGravityZ()));
    const float RequiredSupportForcePerWheel = (WheelOffsets.Num() > 0)
        ? (FMath::Max(1.0f, VehicleMassKg) * GravityAcceleration / static_cast<float>(WheelOffsets.Num()))
        : 0.0f;
    WheelGrounded.SetNum(WheelOffsets.Num());
    WheelSpringLengths.SetNum(WheelOffsets.Num());
    WheelSuspensionForces.SetNum(WheelOffsets.Num());
    WheelLateralForces.SetNum(WheelOffsets.Num());

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(VehicleSuspensionTrace), false, this);
    QueryParams.AddIgnoredActor(this);

    if (!bUseStableGroundRideHeight)
    {
        ApplyChassisClearanceProtection(World, BodyTransform, QueryParams);
    }

    // Center clearance guard: only lifts when the chassis is actually below the intended ride height.
    // It is deliberately capped below suspension capacity so it prevents scraping without making the car float.
    if (!bUseStableGroundRideHeight && MaxChassisAntiGroundStickForce > 0.0f && ChassisAntiGroundStickStrength > 0.0f)
    {
        const FVector BodyLocation = Body->GetComponentLocation();
        // The center guard is also a bottom-out bump-stop, not a ride-height controller.
        const float CollisionGuardHeight = BodyExtent.Z
            + FMath::Clamp(GetPhysicsBodyGroundClearance(), 1.0f, 8.0f);
        const float DesiredCenterHeight = LoadedWheelVisualRestBounds.IsValid
            ? FMath::Min(CollisionGuardHeight, GetDesiredCenterHeightAboveGround())
            : CollisionGuardHeight;
        const FVector ClearanceTraceStart = BodyLocation + FVector(0.0f, 0.0f, 64.0f);
        const FVector ClearanceTraceEnd = BodyLocation - FVector(0.0f, 0.0f, DesiredCenterHeight + 120.0f);
        FHitResult ClearanceHit;
        if (FPhysicsHelper::Raycast(this, ClearanceTraceStart, ClearanceTraceEnd, QueryParams, ClearanceHit))
        {
            const FVector ClearanceNormal = ClearanceHit.ImpactNormal.GetSafeNormal();
            if (ClearanceNormal.Z >= FMath::Max(0.54f, FMath::Clamp(MinSuspensionHitNormalDot, 0.0f, 1.0f) - 0.08f))
            {
                const float CurrentCenterHeight = BodyLocation.Z - ClearanceHit.ImpactPoint.Z;
                const float HeightError = DesiredCenterHeight - CurrentCenterHeight;
                if (HeightError > 1.0f)
                {
                    const FVector ClearanceAxis = FMath::Lerp(ClearanceNormal, FVector::UpVector, 0.30f).GetSafeNormal();
                    const float ClearanceSpeed = FVector::DotProduct(BodyVelocity, ClearanceAxis);
                    const float LiftForce = FMath::Clamp(
                        HeightError * ChassisAntiGroundStickStrength * MassScale - ClearanceSpeed * ChassisAntiGroundStickDamping * MassScale,
                        0.0f,
                        MaxChassisAntiGroundStickForce * MassScale * 0.52f);
                    AddVehicleForce(ClearanceAxis * LiftForce);
                }
            }
        }
    }

    struct FVehicleWheelState
    {
        int32 Index = INDEX_NONE;
        FVector LocalOffset = FVector::ZeroVector;
        FVector MountWorld = FVector::ZeroVector;
        FVector ContactWorld = FVector::ZeroVector;
        FVector WheelForward = FVector::ForwardVector;
        FVector WheelRight = FVector::RightVector;
        FVector ImpactNormal = FVector::UpVector;
        bool bGrounded = false;
        bool bHasRoadTrace = false;
        bool bFront = false;
        bool bRightSide = false;
        float SpringLength = 0.0f;
        float RawSpringLength = 0.0f;
        float Compression = 0.0f;
        float NormalForce = 0.0f;
        float ForwardSpeed = 0.0f;
        float LateralSpeed = 0.0f;
    };

    TArray<FVehicleWheelState> WheelStates;
    WheelStates.SetNum(WheelOffsets.Num());

    float FrontMostWheelX = WheelOffsets.Num() > 0 ? WheelOffsets[0].X : 0.0f;
    float RearMostWheelX = FrontMostWheelX;
    for (const FVector& Offset : WheelOffsets)
    {
        FrontMostWheelX = FMath::Max(FrontMostWheelX, Offset.X);
        RearMostWheelX = FMath::Min(RearMostWheelX, Offset.X);
    }
    const float AxleSplitX = (FrontMostWheelX + RearMostWheelX) * 0.5f;
    const float Wheelbase = FMath::Max(80.0f, FrontMostWheelX - RearMostWheelX);

    const float SteeringSpeedAlphaRaw = FMath::Clamp(AbsBodyForwardSpeed / FMath::Max(100.0f, SteeringSpeedForFullAssist), 0.0f, 1.0f);
    const float SteeringSpeedAlpha = SteeringSpeedAlphaRaw * SteeringSpeedAlphaRaw * (3.0f - 2.0f * SteeringSpeedAlphaRaw);
    // Preserve deliberate steering authority at speed. Older JSON templates commonly contained a
    // very small HighSpeedSteeringAngleDegrees value; the v3 authority scale upgrades those files
    // without rewriting them and still caps the result at the low-speed steering lock.
    const float HighSpeedSteeringAuthority = FMath::Clamp(
        HighSpeedSteeringAuthorityScale * FMath::Lerp(1.0f, 1.15f, SteeringSpeedAlpha),
        1.0f,
        2.0f);
    const float EffectiveHighSpeedSteeringDegrees = FMath::Clamp(
        HighSpeedSteeringAngleDegrees * HighSpeedSteeringAuthority,
        1.0f,
        FMath::Max(1.0f, MaxSteeringAngleDegrees));
    const float EffectiveMaxSteeringDegrees = FMath::Lerp(
        MaxSteeringAngleDegrees,
        EffectiveHighSpeedSteeringDegrees,
        SteeringSpeedAlpha);
    const float BaseSteeringAngle = FMath::DegreesToRadians(EffectiveMaxSteeringDegrees * SmoothedSteeringInput);
    const float HighSpeedGripAlphaRaw = FMath::Clamp(AbsBodyForwardSpeed / FMath::Max(100.0f, HighSpeedLateralGripSpeed), 0.0f, 1.0f);
    const float HighSpeedGripAlpha = HighSpeedGripAlphaRaw * HighSpeedGripAlphaRaw * (3.0f - 2.0f * HighSpeedGripAlphaRaw);
    const float SpeedLateralGripScale = FMath::Lerp(1.0f, FMath::Clamp(HighSpeedLateralGripScale, 0.1f, 1.0f), HighSpeedGripAlpha);

    int32 GroundedWheels = 0;
    int32 GroundedFrontWheels = 0;
    int32 GroundedRearWheels = 0;
    bool bAnyWheelCompressingIntoGround = false;

    for (int32 WheelIndex = 0; WheelIndex < WheelOffsets.Num(); ++WheelIndex)
    {
        FVehicleWheelState& WheelState = WheelStates[WheelIndex];
        WheelState.Index = WheelIndex;
        WheelState.LocalOffset = WheelOffsets[WheelIndex];
        WheelState.bFront = WheelState.LocalOffset.X >= AxleSplitX;
        WheelState.bRightSide = WheelState.LocalOffset.Y > 0.0f;
        const float SafeWheelRadius = GetEffectiveWheelRadius(WheelIndex);
        const bool bWasGrounded = WheelGrounded.IsValidIndex(WheelIndex) && WheelGrounded[WheelIndex];
        const float MinSpringLength = GetMinimumWheelSpringLength(WheelIndex);
        const float MaxSpringLength = GetEffectiveSuspensionRestLength(WheelIndex);
        const float TargetRideSpringLengthForWheel = GetTargetWheelSpringLength(WheelIndex);
        const float PreviousSpringLength = WheelSpringLengths.IsValidIndex(WheelIndex)
            ? FMath::Clamp(WheelSpringLengths[WheelIndex], MinSpringLength, MaxSpringLength)
            : TargetRideSpringLengthForWheel;
        WheelGrounded[WheelIndex] = false;
        WheelSpringLengths[WheelIndex] = TargetRideSpringLengthForWheel;

        const FVector MountWorld = BodyTransform.TransformPosition(WheelState.LocalOffset);
        const FVector SuspensionTraceAxis = FMath::Lerp(Up, FVector::UpVector, 0.32f).GetSafeNormal();
        const FVector SafeTraceAxis = SuspensionTraceAxis.IsNearlyZero() ? Up : SuspensionTraceAxis;
        const float TraceUpDistance = FMath::Max(4.0f, SafeWheelRadius * 0.24f);
        const float TraceDownDistance = MaxSpringLength + SuspensionTraceExtra + SafeWheelRadius + FMath::Max(8.0f, SafeWheelRadius * 0.18f);
        const FVector TraceStart = MountWorld + SafeTraceAxis * TraceUpDistance;
        const FVector TraceEnd = MountWorld - SafeTraceAxis * TraceDownDistance;
        FHitResult Hit;
        bool bHasSuspensionHit = false;
        if (bUseSuspensionSweep)
        {
            const float SweepRadius = FMath::Clamp(SafeWheelRadius * SuspensionSweepRadiusScale, 2.0f, SafeWheelRadius * 0.9f);
            bHasSuspensionHit = World->SweepSingleByChannel(
                Hit,
                TraceStart,
                TraceEnd,
                FQuat::Identity,
                ECC_Visibility,
                FCollisionShape::MakeSphere(SweepRadius),
                QueryParams);
        }
        if (!bHasSuspensionHit)
        {
            bHasSuspensionHit = FPhysicsHelper::Raycast(this, TraceStart, TraceEnd, QueryParams, Hit);
        }
        const float MinAcceptableSuspensionNormalDot = FMath::Clamp(MinSuspensionHitNormalDot, 0.0f, 1.0f);
        if (bHasSuspensionHit)
        {
            const FVector CandidateNormal = Hit.ImpactNormal.GetSafeNormal();
            const float CandidateNormalDot = FMath::Max(
                FVector::DotProduct(CandidateNormal, Up),
                FVector::DotProduct(CandidateNormal, FVector::UpVector));
            bHasSuspensionHit = CandidateNormalDot >= MinAcceptableSuspensionNormalDot;
        }
        if (!bHasSuspensionHit && Hit.bBlockingHit)
        {
            // Sphere sweeps hit the vertical face of a curb/step before the tire reaches the top.
            // Probe a short distance over that face so the suspension can start lifting the chassis
            // with the wheel instead of letting the wheel visual consume the whole ledge.
            const FHitResult RejectedHit = Hit;
            bool bRecoveredStepTop = false;
            if (!bUseStableGroundRideHeight)
            {
                FVector StepProbeDirection = FVector::VectorPlaneProject(BodyVelocity, FVector::UpVector);
                if (StepProbeDirection.SizeSquared() <= 100.0f)
                {
                    StepProbeDirection = FVector::VectorPlaneProject(Forward, FVector::UpVector);
                }
                if (!StepProbeDirection.Normalize())
                {
                    StepProbeDirection = Forward.GetSafeNormal();
                }

                const float MaxStepProbeHeight = FMath::Clamp(StableMaxStepHeight, 4.0f, 60.0f);
                const float ForwardProbeDistance = FMath::Clamp(SafeWheelRadius * 0.55f, 8.0f, 26.0f);
                const FVector ProbeBase = RejectedHit.ImpactPoint + StepProbeDirection * ForwardProbeDistance;
                const FVector StepProbeStart = ProbeBase + Up * (MaxStepProbeHeight + SafeWheelRadius + 10.0f);
                const FVector StepProbeEnd = ProbeBase - Up * (SafeWheelRadius + 12.0f);

                FHitResult StepTopHit;
                if (FPhysicsHelper::Raycast(this, StepProbeStart, StepProbeEnd, QueryParams, StepTopHit)
                    && StepTopHit.bBlockingHit
                    && FMath::Max(
                        FVector::DotProduct(StepTopHit.ImpactNormal.GetSafeNormal(), Up),
                        FVector::DotProduct(StepTopHit.ImpactNormal.GetSafeNormal(), FVector::UpVector)) >= MinAcceptableSuspensionNormalDot)
                {
                    const float StepHeight = FVector::DotProduct(StepTopHit.ImpactPoint - RejectedHit.ImpactPoint, Up);
                    if (StepHeight >= -2.0f && StepHeight <= MaxStepProbeHeight + 2.0f)
                    {
                        Hit = StepTopHit;
                        bRecoveredStepTop = true;
                    }
                }
            }

            bHasSuspensionHit = bRecoveredStepTop;
        }

        if (!bHasSuspensionHit)
        {
            const float RelaxedSpringLength = bUseStableGroundRideHeight
                ? TargetRideSpringLengthForWheel
                : FMath::FInterpTo(PreviousSpringLength, MaxSpringLength, ForceControlDeltaSeconds, FMath::Max(0.1f, SuspensionContactSmoothingSpeed));
            WheelState.SpringLength = RelaxedSpringLength;
            WheelState.MountWorld = MountWorld;
            WheelSpringLengths[WheelIndex] = RelaxedSpringLength;

            // Fade cached tire/suspension forces out when the wheel loses contact.
            // This prevents a stale side force from kicking the chassis on the next contact frame.
            WheelSuspensionForces[WheelIndex] = FMath::FInterpTo(WheelSuspensionForces[WheelIndex], 0.0f, ForceControlDeltaSeconds, FMath::Max(0.1f, SuspensionForceInterpSpeed));
            WheelLateralForces[WheelIndex] = FMath::FInterpTo(WheelLateralForces[WheelIndex], 0.0f, ForceControlDeltaSeconds, FMath::Max(0.1f, TireForceInterpSpeed));
            continue;
        }

        const FVector HitNormal = Hit.ImpactNormal.GetSafeNormal();
        const float HitDistance = FMath::Abs(FVector::DotProduct(MountWorld - Hit.ImpactPoint, Up));
        const float UnclampedSpringLength = HitDistance - SafeWheelRadius;
        WheelState.MountWorld = MountWorld;
        WheelState.ContactWorld = Hit.ImpactPoint;
        WheelState.ImpactNormal = HitNormal;
        WheelState.RawSpringLength = FMath::Max(0.0f, UnclampedSpringLength);
        WheelState.bHasRoadTrace = true;

        if (!bUseStableGroundRideHeight && UnclampedSpringLength > MaxSpringLength + 3.5f)
        {
            // The road was seen by the wheel trace, but the tire is beyond real droop reach.
            // Do not invent support force or tire grip; use this trace only to help the chassis
            // rotate toward the ramp so physical contact can return naturally. A small tolerance
            // avoids flicker on polygon edges where the tire is visually almost touching.
            const float RelaxedSpringLength = FMath::FInterpTo(PreviousSpringLength, MaxSpringLength, ForceControlDeltaSeconds, FMath::Max(0.1f, SuspensionContactSmoothingSpeed));
            WheelState.SpringLength = RelaxedSpringLength;
            WheelSpringLengths[WheelIndex] = RelaxedSpringLength;
            WheelSuspensionForces[WheelIndex] = FMath::FInterpTo(WheelSuspensionForces[WheelIndex], 0.0f, ForceControlDeltaSeconds, FMath::Max(0.1f, SuspensionForceInterpSpeed));
            WheelLateralForces[WheelIndex] = FMath::FInterpTo(WheelLateralForces[WheelIndex], 0.0f, ForceControlDeltaSeconds, FMath::Max(0.1f, TireForceInterpSpeed));
            continue;
        }

        const float RawSpringLength = FMath::Clamp(UnclampedSpringLength, 0.0f, MaxSpringLength);
        const float PhysicalSpringLength = bUseStableGroundRideHeight
            ? TargetRideSpringLengthForWheel
            : FMath::Clamp(RawSpringLength, MinSpringLength, MaxSpringLength);

        // Support force uses the measured trace length so bottom-out force is generated before the
        // chassis clips the ramp. Visual wheel travel is handled separately in UpdateWheelVisuals().
        const float ForceSpringLength = bUseStableGroundRideHeight ? TargetRideSpringLengthForWheel : RawSpringLength;
        const float Compression = FMath::Max(0.0f, MaxSpringLength - ForceSpringLength);
        const FVector SuspensionPointVelocity = Body->GetPhysicsLinearVelocityAtPoint(MountWorld);
        const float UpwardSpeedAtMount = FVector::DotProduct(SuspensionPointVelocity, Up);
        const FVector HorizontalSuspensionPointVelocity(SuspensionPointVelocity.X, SuspensionPointVelocity.Y, 0.0f);
        const float HitNormalZForRebound = FMath::Clamp(HitNormal.Z, 0.20f, 1.0f);
        const float WheelPlaneFollowUpSpeed = FMath::Max(
            0.0f,
            -(HitNormal.X * HorizontalSuspensionPointVelocity.X + HitNormal.Y * HorizontalSuspensionPointVelocity.Y) / HitNormalZForRebound);
        const float WheelSlopeAllowanceAlpha = FMath::Clamp((1.0f - HitNormalZForRebound) / 0.22f, 0.0f, 1.0f);
        const float WheelReboundThreshold = FMath::Max(
            FMath::Max(0.0f, GroundedReboundSpeedThreshold),
            WheelPlaneFollowUpSpeed + FMath::Lerp(20.0f, 54.0f, WheelSlopeAllowanceAlpha));
        const float SuspensionVelocityLimit = FMath::Max(60.0f, MaxSuspensionVelocity);
        // Uphill driving naturally gives the wheel mount a positive world-up velocity. Do not treat that
        // plane-following motion as rebound; only the excess upward velocity above the ramp-follow speed
        // should soften the damper.
        const float SuspensionRelativeUpSpeed = UpwardSpeedAtMount - WheelPlaneFollowUpSpeed;
        const float BodyCompressionVelocity = bUseStableGroundRideHeight
            ? 0.0f
            : FMath::Clamp(-SuspensionRelativeUpSpeed, -SuspensionVelocityLimit, SuspensionVelocityLimit);
        const float TraceCompressionVelocity = (!bUseStableGroundRideHeight && bWasGrounded)
            ? FMath::Clamp(
                (PreviousSpringLength - ForceSpringLength) / FMath::Max(0.001f, ForceControlDeltaSeconds),
                -SuspensionVelocityLimit * 0.65f,
                SuspensionVelocityLimit)
            : 0.0f;
        float CompressionVelocity = BodyCompressionVelocity;
        if (TraceCompressionVelocity > CompressionVelocity)
        {
            // A rising bump or ramp shortens the wheel trace before the chassis velocity has changed.
            // Blend that early compression signal in, but never use it to amplify rebound.
            CompressionVelocity = FMath::Lerp(
                CompressionVelocity,
                TraceCompressionVelocity,
                FMath::Clamp(SuspensionTraceCompressionVelocityBlend, 0.0f, 1.0f));
        }
        const bool bSuspensionCompressingIntoGround = CompressionVelocity > 8.0f || ForceSpringLength < PreviousSpringLength - 0.35f;
        bAnyWheelCompressingIntoGround = bAnyWheelCompressingIntoGround || bSuspensionCompressingIntoGround;

        const float TargetRideSpringLength = TargetRideSpringLengthForWheel;
        const float NeutralCompression = FMath::Max(1.0f, MaxSpringLength - TargetRideSpringLength);
        const float RideHeightError = TargetRideSpringLength - ForceSpringLength;
        const float GroundNormalZ = FMath::Clamp(FVector::DotProduct(HitNormal, FVector::UpVector), 0.35f, 1.0f);
        const float SlopeSupportMultiplier = FMath::Clamp(1.0f / GroundNormalZ, 1.0f, 1.36f);
        const float DeepCompressionAlpha = FMath::Clamp(
            (TargetRideSpringLength - ForceSpringLength) / FMath::Max(1.0f, TargetRideSpringLength),
            0.0f,
            1.0f);
        const float WheelGroundBottomOutDepth = !bUseStableGroundRideHeight
            ? FMath::Max(0.0f, MinSpringLength - ForceSpringLength)
            : 0.0f;
        const float StepCompressionDepth = !bUseStableGroundRideHeight
            ? FMath::Max(0.0f, RideHeightError - 0.75f)
            : 0.0f;
        const bool bDeepStepCompression = StepCompressionDepth > KINDA_SMALL_NUMBER
            && (bSuspensionCompressingIntoGround || !bWasGrounded || DeepCompressionAlpha > 0.05f);
        const float NeutralSpringRate = RequiredSupportForcePerWheel / NeutralCompression;
        const float RequestedSpringRate = FMath::Max(1.0f, SuspensionStrength) * MassScale;
        const float EffectiveSpringRate = FMath::Clamp(
            RequestedSpringRate,
            NeutralSpringRate * 0.86f,
            NeutralSpringRate * 1.18f);
        const float ProgressiveSpringMultiplier = FMath::Lerp(0.92f, 1.24f, DeepCompressionAlpha);
        const float SpringForce = Compression * EffectiveSpringRate * ProgressiveSpringMultiplier * SlopeSupportMultiplier;
        const float DamperResponseMultiplier = CompressionVelocity >= 0.0f ? 1.18f : 0.50f;
        const float DamperCorrection = CompressionVelocity * SuspensionDamping * DamperResponseMultiplier * MassScale;
        const float SuspensionForceLimit = FMath::Max(
            RequiredSupportForcePerWheel * 0.42f,
            FMath::Min(
                FMath::Max(1.0f, MaxSuspensionForcePerWheel) * MassScale,
                RequiredSupportForcePerWheel * FMath::Clamp(MaxSuspensionSupportMultiplier, 1.0f, 2.25f)));
        const float GroundContactGuardForceLimit = FMath::Max(0.0f, MaxWheelGroundContactGuardForce) * MassScale;
        const float StepFollowForceLimit = FMath::Max(0.0f, MaxStepBodyFollowAssistForce) * MassScale;
        const float EffectiveSuspensionForceLimit = (WheelGroundBottomOutDepth > KINDA_SMALL_NUMBER || bDeepStepCompression)
            ? FMath::Max(
                SuspensionForceLimit,
                FMath::Min(
                    SuspensionForceLimit + GroundContactGuardForceLimit + StepFollowForceLimit,
                    RequiredSupportForcePerWheel * 3.65f))
            : SuspensionForceLimit;

        // Physical raycast suspension: full droop has no fake static lift; static ride height
        // is carried by spring compression, and dampers react to relative body/wheel speed.
        // This keeps slope contact realistic without gluing or hovering the chassis.
        float TargetSuspensionForce = FMath::Clamp(
            (SpringForce + DamperCorrection) * FMath::Clamp(SuspensionForceScale, 0.0f, 1.0f),
            0.0f,
            EffectiveSuspensionForceLimit);

        if (!bUseStableGroundRideHeight && WheelGroundBottomOutDepth > KINDA_SMALL_NUMBER)
        {
            // The tire is at/inside the road according to the wheel trace. Add a short hard-stop
            // force so ramp entries cannot swallow the visual wheel before the chassis catches up.
            const float GroundGuardForce = FMath::Clamp(
                WheelGroundBottomOutDepth * FMath::Max(0.0f, WheelGroundContactGuardStrength) * MassScale
                    + FMath::Max(0.0f, CompressionVelocity) * FMath::Max(0.0f, WheelGroundContactGuardDamping) * MassScale,
                0.0f,
                GroundContactGuardForceLimit);
            TargetSuspensionForce = FMath::Clamp(TargetSuspensionForce + GroundGuardForce, 0.0f, EffectiveSuspensionForceLimit);
        }

        if (!bUseStableGroundRideHeight && bDeepStepCompression)
        {
            // Curb/step compression should move the chassis with the tire. This assist is proportional
            // to the wheel's ride-height error and compression speed, and is capped separately so it
            // behaves like a short bump-stop rather than a constant hover force.
            const float StepFollowForce = FMath::Clamp(
                StepCompressionDepth * FMath::Max(0.0f, StepBodyFollowAssistStrength) * MassScale
                    + FMath::Max(0.0f, CompressionVelocity) * FMath::Max(0.0f, StepBodyFollowAssistDamping) * MassScale,
                0.0f,
                StepFollowForceLimit);
            TargetSuspensionForce = FMath::Clamp(TargetSuspensionForce + StepFollowForce, 0.0f, EffectiveSuspensionForceLimit);
        }

        if (!bUseStableGroundRideHeight && !bWasGrounded)
        {
            // New contact should support the 1000 kg chassis quickly when compressing into the road,
            // but still ease in when the chassis is rebounding upward.
            const bool bReboundingOnNewContact = UpwardSpeedAtMount > WheelReboundThreshold && !bSuspensionCompressingIntoGround;
            const bool bLoadingSuspensionOnNewContact = bSuspensionCompressingIntoGround || RideHeightError > 2.0f || bDeepStepCompression;
            const float NewContactSupportMultiplier = bReboundingOnNewContact ? 0.66f : (bDeepStepCompression ? 1.72f : (bLoadingSuspensionOnNewContact ? 1.46f : 1.04f));
            TargetSuspensionForce = FMath::Min(
                TargetSuspensionForce,
                RequiredSupportForcePerWheel * NewContactSupportMultiplier);
        }

        if (!bUseStableGroundRideHeight)
        {
            // Progressive bump stop starts before the spring is fully collapsed. That extra travel support
            // stops the chassis from digging into a ramp/uphill face, but it is still capped by the same
            // suspension limit and rebound damping so it behaves like a rubber bump stop, not a launcher.
            const float BumpStopStartLength = FMath::Clamp(
                MinSpringLength + FMath::Max(1.0f, MaxWheelCompressionTravel) * 0.42f,
                MinSpringLength + 0.5f,
                FMath::Max(MinSpringLength + 0.5f, TargetRideSpringLength));
            if (ForceSpringLength < BumpStopStartLength)
            {
                const float BumpStopAlpha = FMath::Clamp((BumpStopStartLength - ForceSpringLength) / FMath::Max(1.0f, BumpStopStartLength), 0.0f, 1.0f);
                const float BumpStopCompressionSpeed = FMath::Max(0.0f, CompressionVelocity);
                const float BumpStopForce = BumpStopAlpha * BumpStopAlpha * RequiredSupportForcePerWheel * 1.05f
                    + (BumpStopStartLength - ForceSpringLength) * SuspensionStrength * MassScale * FMath::Lerp(0.32f, 0.62f, DeepCompressionAlpha)
                    + BumpStopCompressionSpeed * SuspensionDamping * MassScale * 0.24f * BumpStopAlpha;
                TargetSuspensionForce = FMath::Clamp(TargetSuspensionForce + BumpStopForce, 0.0f, EffectiveSuspensionForceLimit);
            }
        }

        const float PreviousSuspensionForce = WheelSuspensionForces.IsValidIndex(WheelIndex) ? WheelSuspensionForces[WheelIndex] : 0.0f;
        const bool bSuspensionForceIncreasing = TargetSuspensionForce > PreviousSuspensionForce;
        const bool bCompressingSuspension = CompressionVelocity > 0.0f || ForceSpringLength < PreviousSpringLength - 0.5f;
        float ContactBlendSpeed = bWasGrounded
            ? FMath::Max(0.1f, SuspensionForceInterpSpeed)
            : FMath::Max(0.1f, SuspensionForceInterpSpeed * 0.78f);
        ContactBlendSpeed *= bSuspensionForceIncreasing
            ? (bCompressingSuspension ? FMath::Max(0.1f, SuspensionCompressionRiseMultiplier) : 1.35f)
            : FMath::Max(0.1f, SuspensionReboundReleaseMultiplier);
        if (WheelGroundBottomOutDepth > KINDA_SMALL_NUMBER)
        {
            ContactBlendSpeed *= 2.25f;
        }
        const float SmoothedSuspensionForce = FMath::FInterpTo(PreviousSuspensionForce, TargetSuspensionForce, ForceControlDeltaSeconds, ContactBlendSpeed);
        float ForceChangeRateMultiplier = bSuspensionForceIncreasing
            ? (bCompressingSuspension ? FMath::Max(0.1f, SuspensionCompressionRiseMultiplier) : 1.35f)
            : FMath::Max(0.1f, SuspensionReboundReleaseMultiplier);
        if (WheelGroundBottomOutDepth > KINDA_SMALL_NUMBER)
        {
            ForceChangeRateMultiplier *= 2.25f;
        }
        const float SuspensionForceStep = FMath::Max(
            FMath::Max(1000.0f, MaxSuspensionForceChangePerSecond) * MassScale * ForceChangeRateMultiplier,
            RequiredSupportForcePerWheel * (WheelGroundBottomOutDepth > KINDA_SMALL_NUMBER ? 58.0f : (bSuspensionForceIncreasing ? 15.0f : 4.0f))) * ForceControlDeltaSeconds;
        float SuspensionForce = FMath::Clamp(SmoothedSuspensionForce, PreviousSuspensionForce - SuspensionForceStep, PreviousSuspensionForce + SuspensionForceStep);

        // If the chassis is already rebounding upward, do not keep pushing it up at full strength.
        // This keeps curbs and re-contact frames from becoming jumps while preserving normal spring support.
        if (!bUseStableGroundRideHeight && UpwardSpeedAtMount > WheelReboundThreshold && !bSuspensionCompressingIntoGround)
        {
            const float ReboundAlpha = FMath::Clamp(
                (UpwardSpeedAtMount - WheelReboundThreshold) / FMath::Max(1.0f, WheelReboundThreshold * 2.0f),
                0.0f,
                1.0f);
            SuspensionForce *= FMath::Lerp(1.0f, 0.34f, ReboundAlpha);
        }

        if (!bUseStableGroundRideHeight && !bWasGrounded)
        {
            // First contact remains capped to prevent hop, but not so low that the one-ton chassis
            // visibly waits before the suspension starts carrying the load.
            const bool bReboundingOnNewContact = UpwardSpeedAtMount > WheelReboundThreshold && !bSuspensionCompressingIntoGround;
            const bool bLoadingSuspensionOnNewContact = bSuspensionCompressingIntoGround || bCompressingSuspension || ForceSpringLength < TargetRideSpringLength - 1.0f || bDeepStepCompression;
            const float NewContactSupportMultiplier = bReboundingOnNewContact ? 0.66f : (bDeepStepCompression ? 1.68f : (bLoadingSuspensionOnNewContact ? 1.42f : 1.00f));
            SuspensionForce = FMath::Min(
                SuspensionForce,
                RequiredSupportForcePerWheel * NewContactSupportMultiplier);
        }

        const float TireNormalForce = bUseStableGroundRideHeight
            ? RequiredSupportForcePerWheel
            : SuspensionForce;
        WheelSuspensionForces[WheelIndex] = TireNormalForce;

        if (!bUseStableGroundRideHeight && SuspensionForce > KINDA_SMALL_NUMBER)
        {
            // Keep most of the load on the suspension, but apply it close enough to the tire contact
            // patch to create a real pitch-up moment on ramps. This prevents nose/chassis digging
            // without snapping the whole vehicle to the ground plane.
            const float SlopeAmount = FMath::Clamp(1.0f - HitNormal.Z, 0.0f, 1.0f);
            const float UpBlend = FMath::Lerp(0.42f, 0.24f, SlopeAmount);
            const FVector SuspensionAxis = FMath::Lerp(HitNormal, Up, UpBlend).GetSafeNormal();
            const FVector SuspensionForceLocation = FMath::Lerp(Hit.ImpactPoint, MountWorld, 0.34f);
            AddVehicleForceAtLocation(SuspensionAxis * SuspensionForce, SuspensionForceLocation);
        }

        float WheelSteerAngle = 0.0f;
        if (WheelState.bFront)
        {
            WheelSteerAngle = BaseSteeringAngle;
            if (!FMath::IsNearlyZero(WheelSteerAngle, 0.001f) && AckermannStrength > 0.0f)
            {
                const float TurnSign = FMath::Sign(WheelSteerAngle);
                const float HalfTrack = FMath::Max(20.0f, FMath::Abs(WheelState.LocalOffset.Y));
                const float BaseTurnRadius = Wheelbase / FMath::Max(0.05f, FMath::Tan(FMath::Abs(WheelSteerAngle)));
                const bool bInnerWheel = TurnSign * WheelState.LocalOffset.Y > 0.0f;
                const float AdjustedRadius = FMath::Max(50.0f, BaseTurnRadius + (bInnerWheel ? -HalfTrack : HalfTrack));
                const float AckermannAngle = TurnSign * FMath::Atan(Wheelbase / AdjustedRadius);
                WheelSteerAngle = FMath::Lerp(WheelSteerAngle, AckermannAngle, FMath::Clamp(AckermannStrength, 0.0f, 1.0f));
            }
        }

        const FQuat SteerQuat(Up, WheelSteerAngle);
        FVector WheelForward = FVector::VectorPlaneProject(SteerQuat.RotateVector(Forward), HitNormal).GetSafeNormal();
        if (WheelForward.IsNearlyZero())
        {
            WheelForward = SteerQuat.RotateVector(Forward).GetSafeNormal();
        }
        FVector WheelRight = FVector::CrossProduct(HitNormal, WheelForward).GetSafeNormal();
        if (WheelRight.IsNearlyZero())
        {
            WheelRight = FVector::CrossProduct(Up, WheelForward).GetSafeNormal();
        }
        const FVector ContactVelocity = Body->GetPhysicsLinearVelocityAtPoint(Hit.ImpactPoint);

        WheelState.MountWorld = MountWorld;
        WheelState.ContactWorld = Hit.ImpactPoint;
        WheelState.WheelForward = WheelForward;
        WheelState.WheelRight = WheelRight;
        WheelState.ImpactNormal = HitNormal;
        WheelState.bGrounded = true;
        WheelState.SpringLength = PhysicalSpringLength;
        WheelState.Compression = Compression;
        WheelState.NormalForce = TireNormalForce;
        WheelState.ForwardSpeed = FVector::DotProduct(ContactVelocity, WheelForward);
        WheelState.LateralSpeed = FVector::DotProduct(ContactVelocity, WheelRight);

        WheelGrounded[WheelIndex] = true;
        WheelSpringLengths[WheelIndex] = PhysicalSpringLength;
        ++GroundedWheels;
        if (WheelState.bFront)
        {
            ++GroundedFrontWheels;
        }
        else
        {
            ++GroundedRearWheels;
        }
    }

    auto ApplyAntiRollForAxle = [&](bool bFrontAxle)
    {
        FVehicleWheelState* LeftWheel = nullptr;
        FVehicleWheelState* RightWheel = nullptr;
        for (FVehicleWheelState& WheelState : WheelStates)
        {
            if (!WheelState.bGrounded || WheelState.bFront != bFrontAxle)
            {
                continue;
            }

            if (WheelState.bRightSide)
            {
                RightWheel = &WheelState;
            }
            else
            {
                LeftWheel = &WheelState;
            }
        }

        if (!LeftWheel || !RightWheel)
        {
            return;
        }

        const float CompressionDifference = RightWheel->Compression - LeftWheel->Compression;
        const float AntiRollForce = FMath::Clamp(CompressionDifference * AntiRollBarStiffness * MassScale, -MaxAntiRollForce * MassScale, MaxAntiRollForce * MassScale);
        if (!FMath::IsNearlyZero(AntiRollForce, 1.0f))
        {
            const FVector AntiRollAxis = FMath::Lerp(FVector::UpVector, Up, 0.35f).GetSafeNormal();
            AddVehicleForceAtLocation(AntiRollAxis * AntiRollForce, RightWheel->MountWorld);
            AddVehicleForceAtLocation(-AntiRollAxis * AntiRollForce, LeftWheel->MountWorld);
        }
    };

    ApplyAntiRollForAxle(true);
    ApplyAntiRollForAxle(false);

    const float FrontDriveShare = FMath::Clamp(DrivenFrontTorqueShare, 0.0f, 1.0f);
    const float RearDriveShare = 1.0f - FrontDriveShare;
    const float AbsThrottle = FMath::Abs(SmoothedThrottleInput);
    const float SafeSpeedLimit = FMath::Max(500.0f, MaxSpeedForward);
    const float SpeedLimitAlpha = FMath::Clamp((AbsBodyForwardSpeed - SafeSpeedLimit * 0.96f) / FMath::Max(1.0f, SafeSpeedLimit * 0.04f), 0.0f, 1.0f);
    const bool bAcceleratingTowardLimit = !FMath::IsNearlyZero(SmoothedThrottleInput, 0.01f) && FMath::Sign(SmoothedThrottleInput) == FMath::Sign(BodyForwardSpeed);
    const float SpeedLimiter = bAcceleratingTowardLimit ? (1.0f - SpeedLimitAlpha) : 1.0f;

    for (const FVehicleWheelState& WheelState : WheelStates)
    {
        if (!WheelState.bGrounded || WheelState.NormalForce <= KINDA_SMALL_NUMBER)
        {
            continue;
        }

        const int32 GroundedAxleCount = WheelState.bFront ? FMath::Max(1, GroundedFrontWheels) : FMath::Max(1, GroundedRearWheels);
        const float AxleDriveShare = WheelState.bFront ? FrontDriveShare : RearDriveShare;
        const bool bThrottleIsBrake = AbsThrottle > 0.05f && FMath::Abs(WheelState.ForwardSpeed) > 120.0f && FMath::Sign(SmoothedThrottleInput) != FMath::Sign(WheelState.ForwardSpeed);

        float LongitudinalDemand = 0.0f;
        if (bThrottleIsBrake)
        {
            LongitudinalDemand = -FMath::Sign(WheelState.ForwardSpeed) * BrakeForce * MassScale * AbsThrottle / static_cast<float>(FMath::Max(1, GroundedWheels));
        }
        else if (AbsThrottle > 0.02f)
        {
            const float DriveMagnitude = (SmoothedThrottleInput >= 0.0f ? EngineForce : ReverseForce) * MassScale;
            LongitudinalDemand = SmoothedThrottleInput * DriveMagnitude * AxleDriveShare * SpeedLimiter / static_cast<float>(GroundedAxleCount);
        }
        else if (FMath::Abs(WheelState.ForwardSpeed) > 25.0f)
        {
            LongitudinalDemand = -FMath::Sign(WheelState.ForwardSpeed) * EngineBrakingForce * MassScale / static_cast<float>(FMath::Max(1, GroundedWheels));
        }

        if (FMath::Abs(WheelState.ForwardSpeed) > 15.0f && RollingResistance > 0.0f)
        {
            LongitudinalDemand += -FMath::Sign(WheelState.ForwardSpeed) * WheelState.NormalForce * RollingResistance;
        }

        const float UphillForwardAmount = FMath::Clamp(
            FVector::DotProduct(WheelState.WheelForward, FVector::UpVector) * FMath::Sign(SmoothedThrottleInput),
            0.0f,
            0.35f);
        const float SlopeTractionAssist = FMath::Clamp(UphillForwardAmount / 0.22f, 0.0f, 1.0f);
        const float LongitudinalLimit = FMath::Max(
            1.0f,
            WheelState.NormalForce * TireLongitudinalFriction * FMath::Lerp(1.0f, 1.22f, SlopeTractionAssist));
        float LongitudinalForce = FMath::Clamp(LongitudinalDemand, -LongitudinalLimit, LongitudinalLimit);

        // Keep enough combined-slip budget for steering at speed instead of allowing throttle to
        // consume the complete tire friction circle and collapse lateral grip to zero.
        const float SteeringReserveActivity = FMath::Clamp(FMath::Abs(SmoothedSteeringInput), 0.0f, 1.0f)
            * FMath::Lerp(0.25f, 1.0f, HighSpeedGripAlpha);
        const float AxleReserveScale = WheelState.bFront ? 1.0f : 0.72f;
        const float LateralGripReserve = FMath::Clamp(
            SteeringLateralGripReserve * FMath::Lerp(1.0f, HighSpeedSteeringAuthority, HighSpeedGripAlpha)
                * SteeringReserveActivity * AxleReserveScale,
            0.0f,
            0.90f);
        const float MaxLongitudinalUsageForSteering = FMath::Sqrt(FMath::Max(
            0.0f,
            1.0f - LateralGripReserve * LateralGripReserve));
        const float SteeringLongitudinalLimit = LongitudinalLimit * MaxLongitudinalUsageForSteering;
        LongitudinalForce = FMath::Clamp(
            LongitudinalForce,
            -SteeringLongitudinalLimit,
            SteeringLongitudinalLimit);

        // Tire side force is generated from slip angle, not by directly rotating the chassis.
        // The small reference speed keeps low-speed steering responsive without creating a snap turn.
        const float SlipReferenceSpeed = FMath::Max(1.0f, TireSlipReferenceSpeed + FMath::Abs(WheelState.ForwardSpeed) * 0.05f);
        const float EffectiveLongitudinalSpeed = FMath::Max(SlipReferenceSpeed, FMath::Abs(WheelState.ForwardSpeed));
        const float SlipAngle = FMath::Atan2(WheelState.LateralSpeed, EffectiveLongitudinalSpeed);
        const float SteeringGripMultiplier = WheelState.bFront ? FMath::Max(0.1f, FrontSteeringGripMultiplier) : FMath::Max(0.1f, RearSteeringGripMultiplier);
        const float SteeringActivity = FMath::Clamp(FMath::Abs(SmoothedSteeringInput), 0.0f, 1.0f);
        const float EffectiveHighSpeedFrontGripBoost = FMath::Clamp(
            1.0f + (FMath::Clamp(HighSpeedFrontGripBoost, 1.0f, 2.0f) - 1.0f) * HighSpeedSteeringAuthority,
            1.0f,
            2.0f);
        const float FrontGripBoost = WheelState.bFront
            ? FMath::Lerp(1.0f, EffectiveHighSpeedFrontGripBoost, HighSpeedGripAlpha * SteeringActivity)
            : 1.0f;
        // HighSpeedLateralGripScale limits capacity only once. Applying it to both stiffness and
        // capacity compounded the loss and made fast vehicles ignore steering input.
        const float CorneringStiffness = FMath::Max(0.1f, TireCorneringStiffness)
            * FMath::Max(0.1f, LateralGrip) * SteeringGripMultiplier * FrontGripBoost;
        const float LateralDemand = -SlipAngle * CorneringStiffness * WheelState.NormalForce;
        const float LateralCapacityBoost = WheelState.bFront ? FMath::Lerp(1.0f, FrontGripBoost, 0.35f) : 1.0f;
        const float LateralLimitBase = FMath::Min(WheelState.NormalForce * TireLateralFriction, MaxLateralGripForce * MassScale)
            * SpeedLateralGripScale * LateralCapacityBoost;
        const float LongitudinalUsage = FMath::Clamp(FMath::Abs(LongitudinalForce) / LongitudinalLimit, 0.0f, 1.0f);
        const float LateralLimit = LateralLimitBase * FMath::Sqrt(FMath::Max(0.0f, 1.0f - LongitudinalUsage * LongitudinalUsage));
        const float TargetLateralForce = FMath::Clamp(LateralDemand, -LateralLimit, LateralLimit) * FMath::Clamp(TireLateralForceScale, 0.0f, 1.0f);
        const float PreviousLateralForce = WheelLateralForces.IsValidIndex(WheelState.Index) ? WheelLateralForces[WheelState.Index] : 0.0f;
        const float SmoothedLateralForce = FMath::FInterpTo(PreviousLateralForce, TargetLateralForce, ForceControlDeltaSeconds, FMath::Max(0.1f, TireForceInterpSpeed));
        const float LateralForceStep = FMath::Max(1000.0f, MaxLateralForceChangePerSecond) * MassScale * ForceControlDeltaSeconds;
        const float LateralForce = FMath::Clamp(SmoothedLateralForce, PreviousLateralForce - LateralForceStep, PreviousLateralForce + LateralForceStep);
        if (WheelLateralForces.IsValidIndex(WheelState.Index))
        {
            WheelLateralForces[WheelState.Index] = LateralForce;
        }

        const FVector CenterOfMassWorld = Body->GetCenterOfMass();
        const float HeightToCenter = FVector::DotProduct(CenterOfMassWorld - WheelState.ContactWorld, Up);
        const FVector CenterHeightLocation = WheelState.ContactWorld + Up * HeightToCenter;
        const FVector LongitudinalForceLocation = FMath::Lerp(WheelState.ContactWorld, CenterHeightLocation, FMath::Clamp(DriveForceCenterOfMassHeightBlend, 0.0f, 1.0f));
        const FVector LateralForceLocation = FMath::Lerp(WheelState.ContactWorld, CenterHeightLocation, FMath::Clamp(LateralForceCenterOfMassHeightBlend, 0.0f, 1.0f));

        if (!FMath::IsNearlyZero(LongitudinalForce, 1.0f))
        {
            if (bUseStableGroundRideHeight)
            {
                AddVehicleForce(WheelState.WheelForward * LongitudinalForce);
            }
            else
            {
                AddVehicleForceAtLocation(WheelState.WheelForward * LongitudinalForce, LongitudinalForceLocation);
            }
        }
        if (!FMath::IsNearlyZero(LateralForce, 1.0f))
        {
            if (bUseStableGroundRideHeight)
            {
                AddVehicleForce(WheelState.WheelRight * LateralForce);
            }
            else
            {
                AddVehicleForceAtLocation(WheelState.WheelRight * LateralForce, LateralForceLocation);
            }
        }
    }

    if (!bUseStableGroundRideHeight && GroundedWheels > 0)
    {
        const float GroundedRatioForAttitude = static_cast<float>(GroundedWheels) / static_cast<float>(FMath::Max(1, WheelOffsets.Num()));
        const FVector CurrentAngularVelocity = Body->GetPhysicsAngularVelocityInRadians();
        const FVector BodyForwardAxis = Body->GetForwardVector().GetSafeNormal();
        const FVector BodyRightAxis = Body->GetRightVector().GetSafeNormal();
        const float PitchRate = FVector::DotProduct(CurrentAngularVelocity, BodyRightAxis);
        const float RollRate = FVector::DotProduct(CurrentAngularVelocity, BodyForwardAxis);
        const FVector PitchRollAngularVelocity = BodyRightAxis * PitchRate + BodyForwardAxis * RollRate;

        if (GroundedPitchRollDampingTorque > 0.0f)
        {
            const FVector PitchRollDampingTorque = (-PitchRollAngularVelocity * GroundedPitchRollDampingTorque * MassScale * FMath::Clamp(GroundedRatioForAttitude, 0.25f, 1.0f))
                .GetClampedToMaxSize(FMath::Max(1.0f, MaxGroundedPitchRollDampingTorque * MassScale));
            if (!PitchRollDampingTorque.IsNearlyZero(1.0f))
            {
                AddVehicleTorqueInRadians(PitchRollDampingTorque);
            }
        }

        if (TerrainAttitudeTorqueStrength > 0.0f && GroundedWheels >= 1)
        {
            FVector FrontContactSum = FVector::ZeroVector;
            FVector RearContactSum = FVector::ZeroVector;
            FVector RightContactSum = FVector::ZeroVector;
            FVector LeftContactSum = FVector::ZeroVector;
            FVector WeightedNormalSum = FVector::ZeroVector;
            float FrontContactWeight = 0.0f;
            float RearContactWeight = 0.0f;
            float RightContactWeight = 0.0f;
            float LeftContactWeight = 0.0f;

            for (const FVehicleWheelState& WheelState : WheelStates)
            {
                if (!WheelState.bGrounded && !WheelState.bHasRoadTrace)
                {
                    continue;
                }

                const float TraceWeight = WheelState.bGrounded
                    ? FMath::Max(1.0f, WheelState.NormalForce)
                    : RequiredSupportForcePerWheel * FMath::Clamp(UngroundedRoadTraceAttitudeWeight, 0.0f, 1.0f);
                if (TraceWeight <= KINDA_SMALL_NUMBER)
                {
                    continue;
                }

                WeightedNormalSum += WheelState.ImpactNormal.GetSafeNormal() * TraceWeight;

                if (WheelState.bFront)
                {
                    FrontContactSum += WheelState.ContactWorld * TraceWeight;
                    FrontContactWeight += TraceWeight;
                }
                else
                {
                    RearContactSum += WheelState.ContactWorld * TraceWeight;
                    RearContactWeight += TraceWeight;
                }

                if (WheelState.bRightSide)
                {
                    RightContactSum += WheelState.ContactWorld * TraceWeight;
                    RightContactWeight += TraceWeight;
                }
                else
                {
                    LeftContactSum += WheelState.ContactWorld * TraceWeight;
                    LeftContactWeight += TraceWeight;
                }
            }

            FVector TargetRoadUp = WeightedNormalSum.GetSafeNormal();
            if (FrontContactWeight > KINDA_SMALL_NUMBER
                && RearContactWeight > KINDA_SMALL_NUMBER
                && RightContactWeight > KINDA_SMALL_NUMBER
                && LeftContactWeight > KINDA_SMALL_NUMBER)
            {
                const FVector RoadForward = ((FrontContactSum / FrontContactWeight) - (RearContactSum / RearContactWeight)).GetSafeNormal();
                const FVector RoadRight = ((RightContactSum / RightContactWeight) - (LeftContactSum / LeftContactWeight)).GetSafeNormal();
                const FVector PlaneUp = FVector::CrossProduct(RoadForward, RoadRight).GetSafeNormal();
                if (!PlaneUp.IsNearlyZero())
                {
                    TargetRoadUp = (TargetRoadUp + PlaneUp * 1.45f).GetSafeNormal();
                }
            }

            if (TargetRoadUp.IsNearlyZero())
            {
                TargetRoadUp = FVector::UpVector;
            }
            if (FVector::DotProduct(TargetRoadUp, FVector::UpVector) < 0.0f)
            {
                TargetRoadUp *= -1.0f;
            }

            const FVector CurrentUp = Body->GetUpVector().GetSafeNormal();
            const FVector AttitudeErrorAxis = FVector::CrossProduct(CurrentUp, TargetRoadUp);
            const FVector AttitudeDamping = -PitchRollAngularVelocity * TerrainAttitudeTorqueDamping * MassScale;
            const FVector AttitudeTorque = (AttitudeErrorAxis * TerrainAttitudeTorqueStrength * MassScale + AttitudeDamping)
                .GetClampedToMaxSize(FMath::Max(1.0f, MaxTerrainAttitudeTorque * MassScale));
            if (!AttitudeTorque.IsNearlyZero(1.0f))
            {
                AddVehicleTorqueInRadians(AttitudeTorque * FMath::Clamp(GroundedRatioForAttitude, 0.25f, 1.0f));
            }
        }

        const float MaxPitchRollRate = FMath::Max(0.1f, MaxGroundedPitchRollRateRadians);
        if (FMath::Abs(PitchRate) > MaxPitchRollRate || FMath::Abs(RollRate) > MaxPitchRollRate)
        {
            FVector ClampedAngularVelocity = CurrentAngularVelocity;
            ClampedAngularVelocity += BodyRightAxis * (FMath::Clamp(PitchRate, -MaxPitchRollRate, MaxPitchRollRate) - PitchRate);
            ClampedAngularVelocity += BodyForwardAxis * (FMath::Clamp(RollRate, -MaxPitchRollRate, MaxPitchRollRate) - RollRate);
            Body->SetPhysicsAngularVelocityInRadians(ClampedAngularVelocity);
        }

        if (GroundedWheels >= 3
            && FMath::Abs(SmoothedThrottleInput) < 0.04f
            && FMath::Abs(SmoothedSteeringInput) < 0.04f
            && Body->GetPhysicsLinearVelocity().SizeSquared2D() < FMath::Square(120.0f))
        {
            const float KillAlpha = FMath::Clamp(ForceControlDeltaSeconds * FMath::Max(0.0f, LowSpeedPitchRollAngularDamping), 0.0f, 0.85f);
            if (KillAlpha > 0.0f)
            {
                const FVector IdleAngularVelocity = Body->GetPhysicsAngularVelocityInRadians();
                const float IdlePitchRate = FVector::DotProduct(IdleAngularVelocity, BodyRightAxis);
                const float IdleRollRate = FVector::DotProduct(IdleAngularVelocity, BodyForwardAxis);
                const FVector IdlePitchRollAngularVelocity = BodyRightAxis * IdlePitchRate + BodyForwardAxis * IdleRollRate;
                Body->SetPhysicsAngularVelocityInRadians(IdleAngularVelocity - IdlePitchRollAngularVelocity * KillAlpha);
            }
        }
    }

    if (!bUseStableGroundRideHeight && GroundedWheels > 0)
    {
        const float GroundedRatioForRebound = static_cast<float>(GroundedWheels) / static_cast<float>(FMath::Max(1, WheelOffsets.Num()));
        FVector CurrentVelocity = Body->GetPhysicsLinearVelocity();
        const float WorldUpSpeed = FVector::DotProduct(CurrentVelocity, FVector::UpVector);
        const float ReboundThreshold = FMath::Max(0.0f, GroundedReboundSpeedThreshold);

        // Rebound damping should kill launch energy, but it must not cap the vertical velocity required
        // to follow a real ramp. Estimate the Z speed needed to stay on the average contact plane and
        // use that as the damping threshold while the wheels are on a drivable slope.
        FVector WeightedGroundNormal = FVector::ZeroVector;
        float WeightedGroundNormalWeight = 0.0f;
        for (const FVehicleWheelState& WheelState : WheelStates)
        {
            if (!WheelState.bGrounded)
            {
                continue;
            }

            const float WheelWeight = FMath::Max(1.0f, WheelState.NormalForce);
            WeightedGroundNormal += WheelState.ImpactNormal.GetSafeNormal() * WheelWeight;
            WeightedGroundNormalWeight += WheelWeight;
        }

        float EffectiveReboundThreshold = ReboundThreshold;
        if (WeightedGroundNormalWeight > 0.0f)
        {
            const FVector AverageGroundNormal = (WeightedGroundNormal / WeightedGroundNormalWeight).GetSafeNormal();
            if (!AverageGroundNormal.IsNearlyZero() && AverageGroundNormal.Z > 0.20f)
            {
                const FVector HorizontalVelocity(CurrentVelocity.X, CurrentVelocity.Y, 0.0f);
                const float PlaneFollowUpSpeed = FMath::Max(
                    0.0f,
                    -(AverageGroundNormal.X * HorizontalVelocity.X + AverageGroundNormal.Y * HorizontalVelocity.Y) / AverageGroundNormal.Z);
                const float SlopeAllowanceAlpha = FMath::Clamp((1.0f - AverageGroundNormal.Z) / 0.22f, 0.0f, 1.0f);
                EffectiveReboundThreshold = FMath::Max(
                    EffectiveReboundThreshold,
                    PlaneFollowUpSpeed + FMath::Lerp(24.0f, 58.0f, SlopeAllowanceAlpha));
            }
        }

        if (WorldUpSpeed > EffectiveReboundThreshold && !bAnyWheelCompressingIntoGround)
        {
            const float ReboundDampingForce = FMath::Clamp(
                (WorldUpSpeed - EffectiveReboundThreshold) * FMath::Max(0.0f, GroundedReboundDamping) * MassScale * FMath::Clamp(GroundedRatioForRebound, 0.25f, 1.0f),
                0.0f,
                FMath::Max(1.0f, MaxGroundedReboundDampingForce) * MassScale);
            if (ReboundDampingForce > KINDA_SMALL_NUMBER)
            {
                AddVehicleForce(-FVector::UpVector * ReboundDampingForce);
            }

            // A real damper removes rebound energy. The cap is ramp-aware, and it is skipped
            // while any wheel is actively compressing so uphill ramps can lift the car naturally.
            const float MaxRetainedUpSpeed = FMath::Max(EffectiveReboundThreshold * 1.18f, EffectiveReboundThreshold + 22.0f);
            if (WorldUpSpeed > MaxRetainedUpSpeed)
            {
                CurrentVelocity -= FVector::UpVector * (WorldUpSpeed - MaxRetainedUpSpeed);
                Body->SetPhysicsLinearVelocity(CurrentVelocity);
            }
        }
    }

    if (!bUseStableGroundRideHeight && GroundedWheels >= 2)
    {
        FVector WeightedGroundNormal = FVector::ZeroVector;
        float WeightedGroundNormalWeight = 0.0f;
        for (const FVehicleWheelState& WheelState : WheelStates)
        {
            if (!WheelState.bGrounded)
            {
                continue;
            }

            const float WheelWeight = FMath::Max(1.0f, WheelState.NormalForce);
            WeightedGroundNormal += WheelState.ImpactNormal.GetSafeNormal() * WheelWeight;
            WeightedGroundNormalWeight += WheelWeight;
        }

        const FVector AverageGroundNormal = WeightedGroundNormalWeight > KINDA_SMALL_NUMBER
            ? (WeightedGroundNormal / WeightedGroundNormalWeight).GetSafeNormal()
            : FVector::UpVector;
        const float SlopeJitterAlpha = AverageGroundNormal.IsNearlyZero()
            ? 0.0f
            : FMath::Clamp((1.0f - AverageGroundNormal.Z) / 0.18f, 0.0f, 1.0f);
        const float SpeedAlpha = FMath::Clamp(AbsBodyForwardSpeed / VehicleLowSpeedSlopeDampingMaxSpeed, 0.0f, 1.0f);
        const float ControlAlpha = FMath::Clamp(
            FMath::Max(FMath::Abs(SmoothedThrottleInput), FMath::Abs(SmoothedSteeringInput)),
            0.0f,
            1.0f);
        const float GroundedRatioForVelocity = static_cast<float>(GroundedWheels) / static_cast<float>(FMath::Max(1, WheelOffsets.Num()));
        const float DampingAlpha = FMath::Clamp(
            ForceControlDeltaSeconds * VehicleLowSpeedSlopeDampingRate * (1.0f - SpeedAlpha)
                * FMath::Lerp(0.32f, 1.0f, SlopeJitterAlpha)
                * FMath::Lerp(1.0f, 0.55f, ControlAlpha)
                * FMath::Clamp(GroundedRatioForVelocity, 0.25f, 1.0f),
            0.0f,
            VehicleLowSpeedSlopeDampingMaxAlpha);

        if (DampingAlpha > KINDA_SMALL_NUMBER)
        {
            // Low-speed side slip on slopes is usually the visible source of left/right jitter.
            // Remove only a small amount of horizontal body-right velocity; steering and tire forces still own turning.
            FVector CurrentVelocity = Body->GetPhysicsLinearVelocity();
            const FVector FlatRight = FVector::VectorPlaneProject(Body->GetRightVector(), FVector::UpVector).GetSafeNormal();
            if (!FlatRight.IsNearlyZero())
            {
                const float SideSpeed = FVector::DotProduct(CurrentVelocity, FlatRight);
                if (!FMath::IsNearlyZero(SideSpeed, 1.0f))
                {
                    CurrentVelocity -= FlatRight * SideSpeed * DampingAlpha;
                    Body->SetPhysicsLinearVelocity(CurrentVelocity);
                }
            }
        }
    }

    if (!bUseStableGroundRideHeight)
    {
        ApplyAeroDownforce(GroundedWheels);
        ApplyGroundedPitchRollDamping(GroundedWheels, ForceControlDeltaSeconds);
    }

    if (GroundedWheels > 0)
    {
        const float GroundedRatio = static_cast<float>(GroundedWheels) / static_cast<float>(FMath::Max(1, WheelOffsets.Num()));
        const float YawRate = FVector::DotProduct(Body->GetPhysicsAngularVelocityInRadians(), Up);
        const float YawDirectionSign = AbsBodyForwardSpeed > 35.0f
            ? FMath::Sign(BodyForwardSpeed)
            : (FMath::Abs(SmoothedThrottleInput) > 0.05f ? FMath::Sign(SmoothedThrottleInput) : 1.0f);

        // Bicycle-model target used only as a mild yaw-rate damper/assist. The actual turn moment still
        // comes from steered front-tire lateral forces at the wheel contact patches, so the car follows
        // a radius while moving instead of pivoting around its center.
        const float SteeringAmountForYaw = FMath::Clamp(FMath::Abs(SmoothedSteeringInput), 0.0f, 1.0f);
        const float ThrottleTurnActivity = FMath::Clamp(FMath::Abs(SmoothedThrottleInput), 0.0f, 1.0f);
        const float LowSpeedAssistSpeed = FMath::Max(0.0f, LowSpeedSteeringYawAssistSpeed) * SteeringAmountForYaw * ThrottleTurnActivity;
        const float EffectiveYawSpeed = FMath::Max(AbsBodyForwardSpeed, LowSpeedAssistSpeed);
        const float BicycleYawRate = YawDirectionSign * EffectiveYawSpeed * FMath::Tan(BaseSteeringAngle) / FMath::Max(80.0f, Wheelbase);
        const float PhysicalYawRateLimit = FMath::Max(0.75f, MaxAngularVelocityRadians);
        const float DesiredYawRate = FMath::Clamp(
            BicycleYawRate,
            -PhysicalYawRateLimit,
            PhysicalYawRateLimit);

        if (bUseStableGroundRideHeight)
        {
            // Compatibility fallback for legacy stable mode. Keep direct yaw very conservative;
            // the preferred path is force-based tire steering with bUseStableGroundRideHeight=false.
            const float TurnActivity = FMath::Clamp(
                FMath::Max(
                    FMath::Max(FMath::Abs(SmoothedThrottleInput), FMath::Abs(SmoothedSteeringInput) * 0.35f),
                    AbsBodyForwardSpeed / 320.0f),
                0.0f,
                1.0f);
            const float DirectionSign = AbsBodyForwardSpeed > 25.0f
                ? FMath::Sign(BodyForwardSpeed)
                : (FMath::Abs(SmoothedThrottleInput) > 0.05f ? FMath::Sign(SmoothedThrottleInput) : 1.0f);
            const float EffectiveTurnSpeed = FMath::Max(AbsBodyForwardSpeed, FMath::Max(0.0f, StableMinimumTurnSpeed) * TurnActivity);
            const float StableDesiredYawRate = FMath::Clamp(
                DirectionSign * EffectiveTurnSpeed * FMath::Tan(BaseSteeringAngle) / FMath::Max(80.0f, Wheelbase),
                -FMath::Max(0.1f, StableMaxYawRateRadians),
                FMath::Max(0.1f, StableMaxYawRateRadians));

            SmoothedStableYawRate = FMath::FInterpTo(SmoothedStableYawRate, StableDesiredYawRate, DeltaSeconds, FMath::Max(0.1f, StableYawResponse));

            const FRotator CurrentRotation = GetActorRotation();
            const float DeltaYawRadians = SmoothedStableYawRate * DeltaSeconds;
            const float DeltaYawDegrees = FMath::RadiansToDegrees(DeltaYawRadians);
            if (!FMath::IsNearlyZero(DeltaYawDegrees, 0.0001f)
                || !FMath::IsNearlyZero(CurrentRotation.Pitch, 0.01f)
                || !FMath::IsNearlyZero(CurrentRotation.Roll, 0.01f))
            {
                const FRotator NewRotation(0.0f, CurrentRotation.Yaw + DeltaYawDegrees, 0.0f);
                SetActorLocationAndRotation(GetActorLocation(), NewRotation, false, nullptr, ETeleportType::TeleportPhysics);
                Body->SetWorldLocationAndRotation(Body->GetComponentLocation(), NewRotation, false, nullptr, ETeleportType::TeleportPhysics);
            }

            Body->SetPhysicsAngularVelocityInRadians(FVector::UpVector * SmoothedStableYawRate);

            FVector FlatVelocity = Body->GetPhysicsLinearVelocity();
            FlatVelocity.Z = 0.0f;
            if (!FMath::IsNearlyZero(DeltaYawRadians, 0.00001f) && FlatVelocity.SizeSquared2D() > 1.0f)
            {
                const FQuat YawDeltaQuat(FVector::UpVector, DeltaYawRadians);
                const FVector RotatedVelocity = YawDeltaQuat.RotateVector(FlatVelocity);
                const float FollowAlpha = FMath::Clamp(DeltaSeconds * FMath::Max(0.0f, StableVelocityYawFollowSpeed), 0.0f, 1.0f);
                FlatVelocity = FMath::Lerp(FlatVelocity, RotatedVelocity, FollowAlpha);
            }

            const FVector StableForward = GetActorForwardVector().GetSafeNormal();
            const FVector StableRight = GetActorRightVector().GetSafeNormal();
            const float StableForwardSpeed = FVector::DotProduct(FlatVelocity, StableForward);
            const float StableLateralSpeed = FVector::DotProduct(FlatVelocity, StableRight);
            const float DampedLateralSpeed = FMath::FInterpTo(StableLateralSpeed, 0.0f, DeltaSeconds, FMath::Max(0.1f, StableVelocityYawFollowSpeed));
            Body->SetPhysicsLinearVelocity(StableForward * StableForwardSpeed + StableRight * DampedLateralSpeed);
        }
        else
        {
            const float SteeringAmount = FMath::Clamp(FMath::Abs(SmoothedSteeringInput), 0.0f, 1.0f);
            const float DesiredYawAbs = FMath::Abs(DesiredYawRate);
            const float YawRateAbs = FMath::Abs(YawRate);
            const float YawRateError = DesiredYawRate - YawRate;

            // Tire forces remain the primary steering mechanism. The direct yaw controller is limited to
            // measured understeer, uses front-wheel contact as authority, and blends from the existing
            // low-speed helper into a bounded high-speed helper instead of disappearing at speed.
            int32 FrontWheelCount = 0;
            for (const FVehicleWheelState& WheelState : WheelStates)
            {
                FrontWheelCount += WheelState.bFront ? 1 : 0;
            }
            const float FrontGroundedRatio = static_cast<float>(GroundedFrontWheels)
                / static_cast<float>(FMath::Max(1, FrontWheelCount));
            const float LowSpeedAssistAlpha = FMath::Clamp(1.0f - AbsBodyForwardSpeed / 1450.0f, 0.0f, 1.0f);
            const float HighSpeedAssistStart = FMath::Max(100.0f, HighSpeedYawAssistStartSpeed);
            const float HighSpeedAssistFullSpeed = FMath::Max(
                HighSpeedAssistStart + 600.0f,
                FMath::Min(FMath::Max(HighSpeedAssistStart + 1.0f, SteeringSpeedForFullAssist), HighSpeedAssistStart + 1800.0f));
            const float HighSpeedAssistRange = FMath::Max(300.0f, HighSpeedAssistFullSpeed - HighSpeedAssistStart);
            const float HighSpeedAssistAlpha = FMath::Clamp(
                (AbsBodyForwardSpeed - HighSpeedAssistStart) / HighSpeedAssistRange,
                0.0f,
                1.0f);
            const float DesiredYawDirection = FMath::Sign(DesiredYawRate);
            const float DirectedCurrentYawRate = DesiredYawDirection == 0.0f
                ? 0.0f
                : YawRate * DesiredYawDirection;
            const float UndersteerAlpha = SteeringAmount > 0.02f
                ? FMath::Clamp((DesiredYawAbs - FMath::Max(0.0f, DirectedCurrentYawRate)) / FMath::Max(0.12f, DesiredYawAbs), 0.0f, 1.0f)
                : 0.0f;
            const float AssistBlend = FMath::Max(LowSpeedAssistAlpha * 1.10f, HighSpeedAssistAlpha);
            const float AssistStrength = FMath::Lerp(
                SteeringYawRateAssist,
                FMath::Max(SteeringYawRateAssist, HighSpeedYawAssistStrength) * HighSpeedSteeringAuthority,
                HighSpeedAssistAlpha);
            const float YawAssistAlpha = SteeringAmount * UndersteerAlpha * AssistBlend
                * FMath::Clamp(FrontGroundedRatio, 0.0f, 1.0f);
            const float YawAssistTorque = YawRateError * AssistStrength * MassScale * GroundedRatio * YawAssistAlpha;

            // Stability damping is deliberately conditional. It catches excessive spin or idle yaw, but
            // does not fight the tire-generated turn while the car is following a reasonable bicycle-model yaw rate.
            const float AllowedYawRate = FMath::Max(0.18f, DesiredYawAbs * 1.45f + 0.12f);
            const float OvershootAlpha = FMath::Clamp((YawRateAbs - AllowedYawRate) / FMath::Max(0.1f, AllowedYawRate), 0.0f, 1.0f);
            const float IdleYawDampingAlpha = SteeringAmount < 0.04f ? 1.0f : 0.0f;
            const float DampingTargetYawRate = SteeringAmount > 0.04f ? DesiredYawRate : 0.0f;
            const float DampingError = DampingTargetYawRate - YawRate;
            const float YawDampingTorque = DampingError * SteeringYawDamping * MassScale * GroundedRatio
                * FMath::Max(IdleYawDampingAlpha * 0.30f, OvershootAlpha * 0.48f);

            const float EffectiveSteeringTorqueLimit = MaxSteeringAssistTorque * MassScale
                * FMath::Lerp(1.0f, HighSpeedSteeringAuthority, HighSpeedAssistAlpha);
            const float SteeringTotalTorque = FMath::Clamp(
                YawAssistTorque + YawDampingTorque,
                -EffectiveSteeringTorqueLimit,
                EffectiveSteeringTorqueLimit);
            if (!FMath::IsNearlyZero(SteeringTotalTorque, 1.0f))
            {
                AddVehicleTorqueInRadians(Up * SteeringTotalTorque);
            }

            const FVector CurrentAngularVelocity = Body->GetPhysicsAngularVelocityInRadians();
            const FVector BodyYawAngularVelocity = Up * FVector::DotProduct(CurrentAngularVelocity, Up);
            const FVector NonYawAngularVelocity = CurrentAngularVelocity - BodyYawAngularVelocity;
            const FVector PitchRollDampingTorque = (-NonYawAngularVelocity * UprightTorqueDamping * MassScale * 0.18f)
                .GetClampedToMaxSize(FMath::Max(1.0f, UprightTorqueDamping * MassScale * 0.35f));
            if (!PitchRollDampingTorque.IsNearlyZero(1.0f))
            {
                AddVehicleTorqueInRadians(PitchRollDampingTorque);
            }
        }
    }
    else if (!bUseStableGroundRideHeight)
    {
        const FVector AirDampingTorque = (-AngularVelocity * FMath::Max(0.0f, AirborneAngularDampingTorque) * MassScale)
            .GetClampedToMaxSize(FMath::Max(1.0f, MaxAirborneAngularDampingTorque * MassScale));
        AddVehicleTorqueInRadians(AirDampingTorque);
    }
    else
    {
        SmoothedStableYawRate = FMath::FInterpTo(SmoothedStableYawRate, 0.0f, DeltaSeconds, FMath::Max(0.1f, StableYawResponse));
        Body->SetPhysicsAngularVelocityInRadians(FVector::UpVector * SmoothedStableYawRate);
    }
}

void AVehiclePawn::ApplyAeroDownforce(int32 GroundedWheels)
{
    if (!IsValid(Body) || !Body->IsSimulatingPhysics())
    {
        return;
    }

    const FVector LinearVelocity = Body->GetPhysicsLinearVelocity();
    const float Speed = LinearVelocity.Size();
    const bool bGrounded = GroundedWheels > 0;
    const float ClearanceScale = GetDownforceClearanceScale();
    const float MassScale = GetVehicleMassScale();

    if (Speed > 50.0f && MaxAerodynamicDrag > 0.0f && AerodynamicDragCoefficient > 0.0f)
    {
        const float DragMagnitude = FMath::Clamp(Speed * Speed * FMath::Max(0.0f, AerodynamicDragCoefficient) * MassScale, 0.0f, MaxAerodynamicDrag * MassScale);
        AddVehicleForce(-LinearVelocity.GetSafeNormal() * DragMagnitude);
    }

    const float DownforceSpeedAlpha = ComputeVehicleAeroSpeedAlpha(Speed, MinimumDownforceSpeed);
    if (DownforceSpeedAlpha > 0.0f)
    {
        const float Coefficient = bGrounded ? GroundedDownforceCoefficient : AirborneDownforceCoefficient;
        const float MaxForce = (bGrounded ? MaxGroundedDownforce : MaxAirborneDownforce) * MassScale;
        if (Coefficient > 0.0f && MaxForce > 0.0f)
        {
            const FVector BodyUp = Body->GetUpVector();
            const float UprightAlpha = FMath::Clamp(FVector::DotProduct(BodyUp, FVector::UpVector), 0.0f, 1.0f);
            const FVector ChassisDown = (-BodyUp).GetSafeNormal();
            const FVector DownDirection = FMath::Lerp(-FVector::UpVector, ChassisDown, UprightAlpha).GetSafeNormal();
            const float Downforce = FMath::Clamp(Speed * Speed * Coefficient * MassScale, 0.0f, MaxForce) * ClearanceScale * DownforceSpeedAlpha;

            if (Downforce > KINDA_SMALL_NUMBER)
            {
                AddVehicleForce(DownDirection * Downforce);
            }
        }
    }

    if (bGrounded && DownforceSpeedAlpha > 0.0f && SmoothedThrottleInput > 0.02f && ThrottleFrontDownforce > 0.0f)
    {
        // A small center downforce under acceleration counteracts suspension rebound/pitch lift.
        const float ThrottleCenterDownforce = FMath::Clamp(SmoothedThrottleInput * ThrottleFrontDownforce * 0.65f * MassScale, 0.0f, MaxGroundedDownforce * 0.45f * MassScale) * DownforceSpeedAlpha;
        AddVehicleForce(-FVector::UpVector * ThrottleCenterDownforce);
    }

    if (bGrounded && DownforceSpeedAlpha > 0.0f && MaxFrontDownforce > 0.0f)
    {
        const float ForwardSpeed = FMath::Abs(FVector::DotProduct(LinearVelocity, Body->GetForwardVector()));
        const float ForwardSpeedAlpha = ComputeVehicleAeroSpeedAlpha(ForwardSpeed, MinimumDownforceSpeed);
        const float SpeedFrontForce = ForwardSpeed * ForwardSpeed * FMath::Max(0.0f, FrontDownforceCoefficient) * MassScale * ForwardSpeedAlpha;
        const float ThrottleFrontForce = FMath::Max(0.0f, SmoothedThrottleInput) * FMath::Max(0.0f, ThrottleFrontDownforce) * MassScale * DownforceSpeedAlpha;
        const float GroundedRatio = static_cast<float>(GroundedWheels) / static_cast<float>(FMath::Max(1, WheelOffsets.Num()));
        const float FrontForce = FMath::Clamp((SpeedFrontForce + ThrottleFrontForce) * FMath::Clamp(GroundedRatio, 0.25f, 1.0f), 0.0f, MaxFrontDownforce * MassScale) * ClearanceScale;

        if (FrontForce > KINDA_SMALL_NUMBER)
        {
            AddVehicleForceAtLocation(-FVector::UpVector * FrontForce, GetFrontAxleForceLocation());
        }
    }
}

FVector AVehiclePawn::GetFrontAxleForceLocation() const
{
    if (!IsValid(Body))
    {
        return GetActorLocation() + GetActorForwardVector() * BodyExtent.X * 0.75f;
    }

    FVector LocalFrontOffset(BodyExtent.X * 0.75f, 0.0f, 0.0f);
    float FrontX = 0.0f;
    bool bHasWheelOffset = false;
    for (const FVector& Offset : WheelOffsets)
    {
        FrontX = bHasWheelOffset ? FMath::Max(FrontX, Offset.X) : Offset.X;
        bHasWheelOffset = true;
    }

    if (bHasWheelOffset)
    {
        FVector Sum = FVector::ZeroVector;
        int32 Count = 0;
        const float FrontBand = FMath::Max(8.0f, BodyExtent.X * 0.15f);
        for (const FVector& Offset : WheelOffsets)
        {
            if (Offset.X >= FrontX - FrontBand)
            {
                Sum += Offset;
                ++Count;
            }
        }

        if (Count > 0)
        {
            LocalFrontOffset = Sum / static_cast<float>(Count);
            LocalFrontOffset.Z = 0.0f;
        }
    }

    return Body->GetComponentTransform().TransformPosition(LocalFrontOffset);
}

void AVehiclePawn::ApplyPitchStabilization(int32 GroundedWheels)
{
    if (!IsValid(Body) || !Body->IsSimulatingPhysics() || GroundedWheels <= 0 || MaxPitchStabilizationTorque <= 0.0f)
    {
        return;
    }

    const FVector Forward = Body->GetForwardVector();
    const FVector Right = Body->GetRightVector();
    const FVector AngularVelocity = Body->GetPhysicsAngularVelocityInRadians();
    const float NoseUpAmount = FVector::DotProduct(Forward, FVector::UpVector);
    const float PitchRate = FVector::DotProduct(AngularVelocity, Right);
    const float GroundedRatio = static_cast<float>(GroundedWheels) / static_cast<float>(FMath::Max(1, WheelOffsets.Num()));
    const float MassScale = GetVehicleMassScale();
    const float TorqueMagnitude = FMath::Clamp(
        NoseUpAmount * FMath::Max(0.0f, PitchStabilizationTorqueStrength) * MassScale - PitchRate * FMath::Max(0.0f, PitchStabilizationTorqueDamping) * MassScale,
        -MaxPitchStabilizationTorque * MassScale,
        MaxPitchStabilizationTorque * MassScale) * FMath::Clamp(GroundedRatio, 0.25f, 1.0f);

    if (!FMath::IsNearlyZero(TorqueMagnitude, 1.0f))
    {
        AddVehicleTorqueInRadians(Right * TorqueMagnitude);
    }
}

void AVehiclePawn::ApplyRollStabilization(int32 GroundedWheels)
{
    if (!IsValid(Body) || !Body->IsSimulatingPhysics() || GroundedWheels <= 0 || MaxRollStabilizationTorque <= 0.0f)
    {
        return;
    }

    const FVector Forward = Body->GetForwardVector();
    const FVector Right = Body->GetRightVector();
    const FVector AngularVelocity = Body->GetPhysicsAngularVelocityInRadians();
    const float RollError = FVector::DotProduct(Right, FVector::UpVector);
    const float RollRate = FVector::DotProduct(AngularVelocity, Forward);
    const float GroundedRatio = static_cast<float>(GroundedWheels) / static_cast<float>(FMath::Max(1, WheelOffsets.Num()));
    const float MassScale = GetVehicleMassScale();
    const float TorqueMagnitude = FMath::Clamp(
        -RollError * FMath::Max(0.0f, RollStabilizationTorqueStrength) * MassScale - RollRate * FMath::Max(0.0f, RollStabilizationTorqueDamping) * MassScale,
        -MaxRollStabilizationTorque * MassScale,
        MaxRollStabilizationTorque * MassScale) * FMath::Clamp(GroundedRatio, 0.25f, 1.0f);

    if (!FMath::IsNearlyZero(TorqueMagnitude, 1.0f))
    {
        AddVehicleTorqueInRadians(Forward * TorqueMagnitude);
    }
}

void AVehiclePawn::ApplyGroundedPitchRollDamping(int32 GroundedWheels, float DeltaSeconds)
{
    if (!IsValid(Body) || !Body->IsSimulatingPhysics() || GroundedWheels <= 0 || DeltaSeconds <= 0.0f)
    {
        return;
    }

    const float GroundedRatio = static_cast<float>(GroundedWheels) / static_cast<float>(FMath::Max(1, WheelOffsets.Num()));
    const float DampingAlpha = FMath::Clamp(
        1.0f - FMath::Exp(-FMath::Max(0.0f, GroundedPitchRollAngularDamping) * FMath::Clamp(GroundedRatio, 0.25f, 1.0f) * DeltaSeconds),
        0.0f,
        1.0f);

    FVector AngularVelocity = Body->GetPhysicsAngularVelocityInRadians();
    FVector YawAxis = Body->GetUpVector().GetSafeNormal();
    if (YawAxis.IsNearlyZero())
    {
        YawAxis = FVector::UpVector;
    }

    const FVector YawAngularVelocity = YawAxis * FVector::DotProduct(AngularVelocity, YawAxis);
    FVector PitchRollAngularVelocity = AngularVelocity - YawAngularVelocity;

    // Velocity-only damping: removes the loose left/right and nose-up/down sway, but does not force
    // the chassis flat, so ramps and uneven ground still tilt the body through the suspension.
    PitchRollAngularVelocity *= FMath::Clamp(1.0f - DampingAlpha, 0.0f, 1.0f);
    PitchRollAngularVelocity = PitchRollAngularVelocity.GetClampedToMaxSize(FMath::Max(0.05f, MaxGroundedPitchRollAngularVelocity));

    Body->SetPhysicsAngularVelocityInRadians(YawAngularVelocity + PitchRollAngularVelocity);
}

float AVehiclePawn::GetDesiredCenterHeightAboveGround() const
{
    const float SafeRideHeightOffset = FMath::Clamp(RideHeightOffset, -30.0f, 30.0f);

    // The tire bottoms are the ride-height authority. Use a robust median rather than the maximum:
    // one wheel mesh with an oversized AABB/pivot must not raise the entire chassis and leave the
    // other tires in the air. Each rendered wheel is subsequently aligned to its own traced contact.
    TArray<float, TInlineAllocator<16>> RequiredCenterHeights;
    RequiredCenterHeights.Reserve(WheelOffsets.Num());
    for (int32 WheelIndex = 0; WheelIndex < WheelOffsets.Num(); ++WheelIndex)
    {
        const float LocalWheelBottomZ = WheelOffsets[WheelIndex].Z
            - GetTargetWheelSpringLength(WheelIndex)
            - GetEffectiveWheelRadius(WheelIndex);
        const float RequiredHeight = -LocalWheelBottomZ
            + LoadedWheelGroundContactBuffer
            + SafeRideHeightOffset;
        if (FMath::IsFinite(RequiredHeight) && RequiredHeight > 0.0f)
        {
            RequiredCenterHeights.Add(RequiredHeight);
        }
    }

    if (RequiredCenterHeights.Num() > 0)
    {
        RequiredCenterHeights.Sort();
        const int32 UpperMiddleIndex = RequiredCenterHeights.Num() / 2;
        const int32 LowerMiddleIndex = (RequiredCenterHeights.Num() - 1) / 2;
        const float DesiredHeight = 0.5f * (
            RequiredCenterHeights[LowerMiddleIndex] + RequiredCenterHeights[UpperMiddleIndex]);
        if (FMath::IsFinite(DesiredHeight))
        {
            return FMath::Max(1.0f, DesiredHeight);
        }
    }

    // Malformed/no-wheel fallback only. It is never combined with a valid authored tire pose.
    const float FallbackHeight = (LoadedBodyVisualBounds.IsValid
        ? -LoadedBodyVisualBounds.Min.Z + LoadedVisualBodyGroundClearance
        : BodyExtent.Z + GetPhysicsBodyGroundClearance()) + SafeRideHeightOffset;
    return FMath::Max(1.0f, FallbackHeight);
}

float AVehiclePawn::GetDownforceClearanceScale() const
{
    if (!IsValid(Body))
    {
        return 1.0f;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return 1.0f;
    }

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(VehicleDownforceClearanceTrace), false, this);
    QueryParams.AddIgnoredActor(this);

    const FVector BodyLocation = Body->GetComponentLocation();
    const float DesiredCenterHeight = GetDesiredCenterHeightAboveGround();
    const float TraceUp = FMath::Max(80.0f, BodyExtent.Z + 80.0f);
    const FVector Start = BodyLocation + FVector::UpVector * TraceUp;
    const FVector End = BodyLocation - FVector::UpVector * (DesiredCenterHeight + TraceUp + 160.0f);

    FHitResult Hit;
    if (!FPhysicsHelper::Raycast(this, Start, End, QueryParams, Hit))
    {
        return 1.0f;
    }

    const float CurrentCenterHeight = BodyLocation.Z - Hit.ImpactPoint.Z;
    const float FadeRange = FMath::Max(12.0f, GetPhysicsBodyGroundClearance());
    const float LowHeight = DesiredCenterHeight - FadeRange;
    return FMath::Clamp((CurrentCenterHeight - LowHeight) / FadeRange, 0.0f, 1.0f);
}

void AVehiclePawn::RestoreStoredPawnCamera(APlayerController* PlayerController, APawn* PawnToRestore) const
{
    if (!bResetCharacterCameraOnExit || !IsValid(PlayerController) || !IsValid(PawnToRestore))
    {
        return;
    }

    // Do not keep the vehicle camera's spring-arm pitch/roll after repossessing the character.
    // Align yaw to the restored character and reset pitch/roll to the normal character camera state.
    FRotator CleanCharacterRotation = PawnToRestore->GetActorRotation();
    CleanCharacterRotation.Pitch = 0.0f;
    CleanCharacterRotation.Roll = 0.0f;

    PlayerController->SetViewTarget(PawnToRestore);
    PlayerController->SetControlRotation(CleanCharacterRotation);
    PlayerController->ClientSetRotation(CleanCharacterRotation, true);
}

void AVehiclePawn::UpdateWheelVisuals(float DeltaSeconds)
{
    const FTransform BodyTransform = Body ? Body->GetComponentTransform() : GetActorTransform();
    const FVector Forward = Body ? Body->GetForwardVector() : GetActorForwardVector();
    const FVector VisualLinearVelocity = bUseStableGroundRideHeight
        ? StablePhysicsLinearVelocity
        : (Body ? Body->GetPhysicsLinearVelocity() : FVector::ZeroVector);
    const FVector VisualAngularVelocity = bUseStableGroundRideHeight
        ? StablePhysicsAngularVelocity
        : (Body ? Body->GetPhysicsAngularVelocityInRadians() : FVector::ZeroVector);
    const float AbsForwardSpeed = FMath::Abs(FVector::DotProduct(VisualLinearVelocity, Forward));
    const float SteerAlphaRaw = FMath::Clamp(AbsForwardSpeed / FMath::Max(1.0f, SteeringSpeedForFullAssist), 0.0f, 1.0f);
    const float SteerAlpha = FMath::Pow(SteerAlphaRaw, 1.65f);
    const float VisualHighSpeedAuthority = FMath::Clamp(
        HighSpeedSteeringAuthorityScale * FMath::Lerp(1.0f, 1.15f, SteerAlpha),
        1.0f,
        2.0f);
    const float VisualHighSpeedSteeringDegrees = FMath::Clamp(
        HighSpeedSteeringAngleDegrees * VisualHighSpeedAuthority,
        1.0f,
        FMath::Max(1.0f, MaxSteeringAngleDegrees));
    const float SteeringDegrees = SmoothedSteeringInput * FMath::Lerp(
        MaxSteeringAngleDegrees,
        VisualHighSpeedSteeringDegrees,
        SteerAlpha);
    WheelSpinDegrees.SetNum(WheelOffsets.Num());
    WheelVisualSpringLengths.SetNum(WheelOffsets.Num());

    UWorld* World = GetWorld();
    FCollisionQueryParams VisualQueryParams(SCENE_QUERY_STAT(VehicleWheelVisualGroundClampTrace), false, this);
    VisualQueryParams.AddIgnoredActor(this);

    auto CalculateWheelVisual = [&](int32 WheelIndex, FVector& OutLocalCenter, FRotator& OutRelativeRotation)
    {
        const float SafeWheelRadius = GetEffectiveWheelRadius(WheelIndex);
        const FVector MountWorld = BodyTransform.TransformPosition(WheelOffsets[WheelIndex]);
        const float MinSpringLength = GetMinimumWheelSpringLength(WheelIndex);
        const float MaxSpringLength = GetEffectiveSuspensionRestLength(WheelIndex);
        const float TargetRideSpringLength = GetTargetWheelSpringLength(WheelIndex);
        const float PhysicalSpringLength = WheelSpringLengths.IsValidIndex(WheelIndex)
            ? FMath::Clamp(WheelSpringLengths[WheelIndex], MinSpringLength, MaxSpringLength)
            : TargetRideSpringLength;
        const float VisualTravelScale = FMath::Clamp(WheelVisualSuspensionTravelScale, 0.1f, 1.0f);
        float TargetVisualSpringLength = FMath::Clamp(
            TargetRideSpringLength + (PhysicalSpringLength - TargetRideSpringLength) * VisualTravelScale,
            FMath::Min(TargetRideSpringLength, MinSpringLength),
            FMath::Max(TargetRideSpringLength, MaxSpringLength));

        const FVector WheelUp = BodyTransform.GetUnitAxis(EAxis::Z).GetSafeNormal();
        if (bPreventWheelVisualGroundPenetration && World && !WheelUp.IsNearlyZero())
        {
            const FVector VisualTraceAxis = FMath::Lerp(WheelUp, FVector::UpVector, 0.32f).GetSafeNormal();
            const FVector SafeVisualTraceAxis = VisualTraceAxis.IsNearlyZero() ? WheelUp : VisualTraceAxis;
            const FVector VisualTraceStart = MountWorld + SafeVisualTraceAxis * FMath::Max(4.0f, SafeWheelRadius * 0.28f);
            const FVector VisualTraceEnd = MountWorld - SafeVisualTraceAxis * (MaxSpringLength + SuspensionTraceExtra + SafeWheelRadius + 16.0f);
            FHitResult VisualHit;
            bool bHasVisualGround = false;
            if (bUseSuspensionSweep)
            {
                const float VisualSweepRadius = FMath::Clamp(
                    SafeWheelRadius * FMath::Clamp(WheelVisualGroundSweepRadiusScale, 0.1f, 1.0f),
                    2.0f,
                    SafeWheelRadius * 0.98f);
                bHasVisualGround = World->SweepSingleByChannel(
                    VisualHit,
                    VisualTraceStart,
                    VisualTraceEnd,
                    FQuat::Identity,
                    ECC_Visibility,
                    FCollisionShape::MakeSphere(VisualSweepRadius),
                    VisualQueryParams);
            }
            if (!bHasVisualGround)
            {
                bHasVisualGround = FPhysicsHelper::Raycast(this, VisualTraceStart, VisualTraceEnd, VisualQueryParams, VisualHit);
            }

            if (bHasVisualGround
                && VisualHit.bBlockingHit
                && !VisualHit.bStartPenetrating
                && FMath::Max(
                    FVector::DotProduct(VisualHit.ImpactNormal.GetSafeNormal(), WheelUp),
                    FVector::DotProduct(VisualHit.ImpactNormal.GetSafeNormal(), FVector::UpVector)) >= FMath::Clamp(MinSuspensionHitNormalDot, 0.0f, 1.0f))
            {
                const float MountToGround = FVector::DotProduct(MountWorld - VisualHit.ImpactPoint, WheelUp);
                const float GroundContactSpringLength = FMath::Clamp(
                    MountToGround - SafeWheelRadius - FMath::Max(0.0f, WheelVisualGroundContactBuffer),
                    0.0f,
                    MaxSpringLength);
                if (FMath::IsFinite(GroundContactSpringLength))
                {
                    // Align the tire bottom to the traced surface in both directions. The previous
                    // code only shortened the spring to prevent penetration; it never extended a
                    // hovering wheel down to the road, so a valid contact could still render a gap.
                    TargetVisualSpringLength = GroundContactSpringLength;
                }
            }
        }

        float& VisualSpringLength = WheelVisualSpringLengths[WheelIndex];
        if (VisualSpringLength <= 0.0f || DeltaSeconds <= 0.0f || VisualSpringLength > TargetVisualSpringLength)
        {
            // Upward correction is immediate so the tire never renders below the traced road surface.
            VisualSpringLength = TargetVisualSpringLength;
        }
        else
        {
            VisualSpringLength = FMath::FInterpTo(
                VisualSpringLength,
                TargetVisualSpringLength,
                DeltaSeconds,
                FMath::Max(0.1f, WheelVisualSuspensionInterpSpeed));
        }
        const FVector WheelCenterWorld = MountWorld - WheelUp * VisualSpringLength;
        OutLocalCenter = BodyTransform.InverseTransformPosition(WheelCenterWorld);
        const FVector Velocity = bUseStableGroundRideHeight
            ? (VisualLinearVelocity + FVector::CrossProduct(VisualAngularVelocity, WheelCenterWorld - BodyTransform.GetLocation()))
            : (Body ? Body->GetPhysicsLinearVelocityAtPoint(WheelCenterWorld) : FVector::ZeroVector);
        const float ForwardSpeed = FVector::DotProduct(Velocity, Forward);
        WheelSpinDegrees[WheelIndex] += FMath::RadiansToDegrees((ForwardSpeed / SafeWheelRadius) * DeltaSeconds);

        const bool bFrontWheel = WheelOffsets[WheelIndex].X > 0.0f;
        OutRelativeRotation = FRotator(WheelSpinDegrees[WheelIndex], bFrontWheel ? SteeringDegrees : 0.0f, 0.0f);
    };

    UInstancedEntitySubsystem* InstancedEntities = UInstancedEntitySubsystem::Get(this);
    if (!InstancedEntities || InstancedRenderRegistrationId == INDEX_NONE)
    {
        return;
    }

    for (int32 WheelIndex = 0;
         WheelIndex < LoadedWheelRenderPartIndices.Num() && WheelIndex < WheelOffsets.Num();
         ++WheelIndex)
    {
        FVector LocalWheelCenter;
        FRotator RelativeRotation;
        CalculateWheelVisual(WheelIndex, LocalWheelCenter, RelativeRotation);

        const FQuat BaseRotation = LoadedWheelBaseRotations.IsValidIndex(WheelIndex)
            ? LoadedWheelBaseRotations[WheelIndex]
            : FQuat::Identity;
        const FVector BaseScale = LoadedWheelBaseScales.IsValidIndex(WheelIndex)
            ? LoadedWheelBaseScales[WheelIndex]
            : FVector::OneVector;
        const FVector MeshCenterOffset = LoadedWheelVisualCenterOffsets.IsValidIndex(WheelIndex)
            ? LoadedWheelVisualCenterOffsets[WheelIndex]
            : FVector::ZeroVector;
        const FQuat VisualRotation = (RelativeRotation.Quaternion() * BaseRotation).GetNormalized();
        const FVector RelativeLocation = LocalWheelCenter - VisualRotation.RotateVector(MeshCenterOffset * BaseScale);
        const FTransform WheelLocalTransform(VisualRotation, RelativeLocation, BaseScale);

        InstancedEntities->UpdateEntityPartLocalTransform(
            InstancedRenderRegistrationId,
            LoadedWheelRenderPartIndices[WheelIndex],
            WheelLocalTransform);
    }
}


