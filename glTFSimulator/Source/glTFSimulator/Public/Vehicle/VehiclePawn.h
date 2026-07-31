// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeCounter.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Model/glTFMaterialAssetReferences.h"
#include "World/PlacementTypes.h"
#include "VehiclePawn.generated.h"

class UBoxComponent;
class UPhysicalMaterial;
class UProceduralMeshComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UglTFRuntimeAsset;
class UBuoyancyComponent;
class USpringArmComponent;
class UCameraComponent;
class APlayerController;
class UInputComponent;
class UWorld;
struct FCollisionQueryParams;
class FJsonObject;

UCLASS(BlueprintType)
class GLTFSIMULATOR_API AVehiclePawn : public APawn
{
    GENERATED_BODY()

public:
    AVehiclePawn();

    UFUNCTION(BlueprintCallable, Category="Vehicle")
    bool EnterVehicle(APlayerController* PlayerController, APawn* PreviousPawn);

    UFUNCTION(BlueprintCallable, Category="Vehicle")
    void ExitVehicle();

    UFUNCTION(BlueprintPure, Category="Vehicle")
    bool IsOccupied() const { return IsValid(OccupyingController); }

    UFUNCTION(BlueprintCallable, Category="Vehicle|Input")
    void SetDriveInput(float Throttle, float Steering);

    UFUNCTION(BlueprintCallable, Category="Vehicle|Input")
    void SetThrottleInput(float Throttle);

    UFUNCTION(BlueprintCallable, Category="Vehicle|Input")
    void SetSteeringInput(float Steering);

    UFUNCTION(BlueprintCallable, Category="Vehicle|Input")
    void ClearDriveInput();

    UFUNCTION(Server, Unreliable)
    void ServerSetDriveInput(float Throttle, float Steering);

    UFUNCTION(BlueprintPure, Category="Vehicle")
    APawn* GetStoredPawn() const { return StoredPawn.Get(); }

    UFUNCTION(BlueprintCallable, Category="Vehicle|Model")
    bool LoadVehicleModel(const FString& InFilePath, const FString& InObjectName);

    // Loads per-model driving tune values from a JSON file next to the vehicle model.
    UFUNCTION(BlueprintCallable, Category="Vehicle|Tuning")
    bool LoadVehicleTuningJson(const FString& JsonPath);

    // Writes a documented tuning template. Existing files are not overwritten by LoadVehicleModel().
    UFUNCTION(BlueprintCallable, Category="Vehicle|Tuning")
    bool SaveVehicleTuningJsonTemplate(const FString& JsonPath) const;

    UFUNCTION(BlueprintPure, Category="Vehicle|Tuning")
    FString GetVehicleTuningJsonPath() const;

    UFUNCTION(BlueprintCallable, Category="Vehicle|Physics")
    void ResetVehiclePoseAboveGround();

    UFUNCTION(BlueprintCallable, Category="Vehicle|Save")
    FPlacedObjectRecord ToPlacementRecord(int32 VehicleRecordIndex = 0) const;

    struct FVehicleParallelControlInput
    {
        float DeltaSeconds = 0.0f;
        float ThrottleInput = 0.0f;
        float SteeringInput = 0.0f;
        float SmoothedThrottleInput = 0.0f;
        float SmoothedSteeringInput = 0.0f;
        float ThrottleInputInterpSpeed = 5.0f;
        float SteeringInputRiseRate = 2.5f;
        float SteeringInputReturnRate = 10.5f;
        float SteeringInputSpeedDamping = 0.0f;
        float SteeringInputCurveExponent = 1.0f;
        float SteeringSpeedForFullAssist = 4300.0f;
        FVector BodyForward = FVector::ForwardVector;
        FVector BodyVelocity = FVector::ZeroVector;
    };

    struct FVehicleParallelControlOutput
    {
        float SmoothedThrottleInput = 0.0f;
        float SmoothedSteeringInput = 0.0f;
        bool bValid = false;
    };

    FVehicleParallelControlInput BuildParallelControlInput(float DeltaSeconds) const;
    static FVehicleParallelControlOutput CalculateParallelControlOutput(const FVehicleParallelControlInput& Input);
    void ApplyParallelControlOutput(const FVehicleParallelControlOutput& Output);
    void UpdateVehicleFromSubSystem(float DeltaSeconds);

protected:
    virtual void BeginPlay() override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Destroyed() override;
    virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

private:
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UBoxComponent> Body;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UProceduralMeshComponent> BodyMesh;

    UPROPERTY(VisibleAnywhere)
    TArray<TObjectPtr<UProceduralMeshComponent>> WheelMeshes;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UBuoyancyComponent> BuoyancyComponent;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<USpringArmComponent> SpringArm;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UCameraComponent> Camera;

    UPROPERTY()
    TObjectPtr<APlayerController> OccupyingController;

    UPROPERTY()
    TObjectPtr<APawn> StoredPawn;

    FRotator StoredControlRotation = FRotator::ZeroRotator;
    FTransform StoredPawnTransformBeforeEnter = FTransform::Identity;
    bool bHasStoredControlRotation = false;
    bool bHasStoredPawnTransform = false;

    UPROPERTY(EditAnywhere, Category="Vehicle|Camera")
    bool bResetCharacterCameraOnExit = true;

    /** glTFRuntime material assets assigned directly in the owning Blueprint/class defaults. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Vehicle|Assets", meta=(AllowPrivateAccess="true"))
    FglTFMaterialAssetReferences MaterialAssets;

    UPROPERTY(Transient)
    TObjectPtr<UPhysicalMaterial> LowFrictionPhysicalMaterial;

    // Default chassis mass is one metric ton. ApplyVehicleBodyPhysicsSettings() pushes this
    // into Chaos so suspension, tire load, drive force, and damping are tuned around a 1000 kg car.
    UPROPERTY(EditAnywhere, Category="Vehicle", meta=(ClampMin="1.0"))
    float VehicleMassKg = 1000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float SuspensionRestLength = 56.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float SuspensionTraceExtra = 82.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float SuspensionStrength = 16000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float SuspensionDamping = 19000.0f;

    // Limits spring speed used by the damper. Compression is allowed to react quickly for a one-ton chassis,
    // while rebound is still damped in code so the vehicle does not hop after sharp contacts.
    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.0"))
    float MaxSuspensionVelocity = 270.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.0"))
    float SuspensionContactSmoothingSpeed = 112.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension")
    bool bUseSuspensionSweep = true;

    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.05", ClampMax="1.0"))
    float SuspensionSweepRadiusScale = 0.82f;

    // Near-miss wheel traces can help the chassis pitch/roll toward a ramp before full tire load returns.
    // This restores contact on slopes without adding fake support force or snapping to the floor.
    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.0", ClampMax="1.0"))
    float UngroundedRoadTraceAttitudeWeight = 0.42f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float MaxSuspensionForcePerWheel = 820000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float UprightTorqueDamping = 150000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float LoadedWheelVisualRestLengthRatio = 0.68f;

    // Visual-only wheel travel scale. Physics still uses the full suspension trace, but the rendered tire
    // moves a smaller amount around the authored ride pose so the gap changes like a real passenger car.
    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.1", ClampMax="1.0"))
    float WheelVisualSuspensionTravelScale = 0.42f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.1"))
    float WheelVisualSuspensionInterpSpeed = 34.0f;

    // Maximum upward compression from the authored/static wheel pose.
    // Keep this short so tires cannot travel deep into the chassis.
    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="2.0", ClampMax="30.0"))
    float MaxWheelCompressionTravel = 4.5f;

    // Downward droop from the authored/static wheel pose. This is separate from compression:
    // the wheel does not need to move far into the body, but it does need enough extension
    // to stay in contact over slopes, crests, and uneven ground.
    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="4.0", ClampMax="80.0"))
    float MaxWheelDroopTravel = 28.0f;

    // Required fallback gap between the top of the tire and the lower chassis collision box
    // when no authored wheel-rest length is available. Runtime glTF wheels preserve their
    // authored rest pose and use MaxWheelCompressionTravel for the upward limit.
    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.0", ClampMax="30.0"))
    float MinimumWheelBodyClearance = 8.0f;

    // Legacy deterministic ground solver. Disabled by default because the normal runtime path now uses
    // force-based wheel suspension, tire friction, gravity, anti-roll, and chassis inertia without
    // snapping the vehicle to traced ground.
    UPROPERTY(EditAnywhere, Category="Vehicle|Stability")
    bool bUseStableGroundRideHeight = false;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability")
    bool bLockBodyPitchAndRoll = false;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float StableRideHeightGroundBuffer = 0.75f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float StableGroundTraceUp = 260.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float StableGroundTraceDown = 520.0f;

    // Stable ride mode now follows wheel-contact terrain pitch/roll, while yaw response is deliberately damped
    // so steering does not snap the car sideways at low speed.
    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.1"))
    float StableYawResponse = 5.2f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.1"))
    float StableMaxYawRateRadians = 2.40f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float StableMinimumTurnSpeed = 125.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.1"))
    float StableVelocityYawFollowSpeed = 5.0f;

    // Prevents the deterministic wheel solver from tunneling through walls at low FPS.
    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.008", ClampMax="0.05"))
    float StableMaxSimulationStepSeconds = 0.0125f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="1", ClampMax="64"))
    int32 StableMaxSimulationSubsteps = 64;

    // A vertical ledge higher than this is treated as an obstacle instead of a ramp.
    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float StableMaxStepHeight = 28.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="1.0", ClampMax="60.0"))
    float StableMaxSlopeDegrees = 32.0f;

    // Caps how quickly the chassis may rise so trace hits cannot pop the car onto a tall block.
    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float StableMaxClimbRate = 180.0f;

    // Extra pitch/roll damping for the deterministic wheel-physics solver. It damps oscillation
    // without locking the chassis flat, so ramps and uneven terrain still tilt the car naturally.
    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float StablePitchRollDamping = 68000.0f;

    // A soft terrain-attitude torque nudges the body toward the wheel-supported ground plane.
    // It is torque-based rather than teleporting/snap-aligning the body to the floor.
    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float StableTerrainAttitudeStrength = 6200.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.1"))
    float StableMaxPitchRollRateRadians = 0.38f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.1"))
    float StableMaxVerticalSpeed = 480.0f;

    // Extra vertical damping and rise-speed limiting for the deterministic wheel solver.
    // These stop raycast suspension contacts or collision depenetration from becoming a retained jump velocity.
    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float StableGroundedVerticalDamping = 190000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float StableMaxGroundedUpSpeed = 70.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="1.0"))
    float StableSuspensionForceLimitMultiplier = 1.45f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float GroundedDownforceCoefficient = 0.00055f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float MaxGroundedDownforce = 22000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float AirborneDownforceCoefficient = 0.00115f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float MaxAirborneDownforce = 56000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float MinimumDownforceSpeed = 280.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float FrontDownforceCoefficient = 0.00035f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float MaxFrontDownforce = 11000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float ThrottleFrontDownforce = 3200.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Pitch", meta=(ClampMin="0.0"))
    float PitchStabilizationTorqueStrength = 0.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Pitch", meta=(ClampMin="0.0"))
    float PitchStabilizationTorqueDamping = 105000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Pitch", meta=(ClampMin="0.0"))
    float MaxPitchStabilizationTorque = 125000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Pitch", meta=(ClampMin="0.0", ClampMax="1.0"))
    float DriveForceCenterOfMassHeightBlend = 0.92f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Roll", meta=(ClampMin="0.0", ClampMax="1.0"))
    float LateralForceCenterOfMassHeightBlend = 0.96f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Roll", meta=(ClampMin="0.0"))
    float RollStabilizationTorqueStrength = 0.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Roll", meta=(ClampMin="0.0"))
    float RollStabilizationTorqueDamping = 115000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Roll", meta=(ClampMin="0.0"))
    float MaxRollStabilizationTorque = 135000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float EngineForce = 430000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float ReverseForce = 140000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float BrakeForce = 620000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.0"))
    float SteeringTorque = 0.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="1.0", ClampMax="55.0"))
    float MaxSteeringAngleDegrees = 34.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.0"))
    float MinSteeringSpeedFactor = 0.30f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.0"))
    float MaxSteeringSpeedFactor = 1.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="1.0"))
    float SteeringSpeedForFullAssist = 4300.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.0"))
    float SteeringYawRateAssist = 32000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.0"))
    float SteeringYawDamping = 118000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.0"))
    float MaxSteeringAssistTorque = 85000.0f;

    // Small low-speed yaw helper used only as an anti-understeer assist. Normal turning now comes
    // from Ackermann front-wheel angles and tire lateral forces, not from a forced yaw-rate controller.
    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.0"))
    float LowSpeedSteeringYawAssistSpeed = 145.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.1"))
    float FrontSteeringGripMultiplier = 1.20f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.1"))
    float RearSteeringGripMultiplier = 1.30f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering")
    float LateralGrip = 1.02f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering")
    float MaxLateralGripForce = 285000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float RollingResistance = 0.012f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float UprightTorqueStrength = 0.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float WheelRadius = 33.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float WheelWidth = 24.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float MaxSpeedForward = 4200.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="0.1"))
    float TireLongitudinalFriction = 1.48f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="0.1"))
    float TireLateralFriction = 1.14f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="0.1"))
    float TireCorneringStiffness = 6.4f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="1.0"))
    float TireSlipReferenceSpeed = 120.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="0.1", ClampMax="1.0"))
    float HighSpeedLateralGripScale = 0.64f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="100.0"))
    float HighSpeedLateralGripSpeed = 3400.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="0.0", ClampMax="1.0"))
    float DrivenFrontTorqueShare = 0.32f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="0.0"))
    float EngineBrakingForce = 72000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="1.0", ClampMax="45.0"))
    float HighSpeedSteeringAngleDegrees = 9.5f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.0", ClampMax="1.0"))
    float AckermannStrength = 1.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float AerodynamicDragCoefficient = 0.010f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float MaxAerodynamicDrag = 480000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Roll", meta=(ClampMin="0.0"))
    float AntiRollBarStiffness = 94000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Roll", meta=(ClampMin="0.0"))
    float MaxAntiRollForce = 250000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Air", meta=(ClampMin="0.0"))
    float AirborneAngularDampingTorque = 150000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Air", meta=(ClampMin="0.0"))
    float MaxAirborneAngularDampingTorque = 260000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Air", meta=(ClampMin="0.5"))
    float MaxAngularVelocityRadians = 2.40f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Input", meta=(ClampMin="0.1"))
    float ThrottleInputInterpSpeed = 5.2f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Input", meta=(ClampMin="0.1"))
    float SteeringInputInterpSpeed = 9.0f;

    // Rate-limits steering input so keyboard/gamepad input does not snap the chassis into a yaw impulse.
    UPROPERTY(EditAnywhere, Category="Vehicle|Input", meta=(ClampMin="0.1"))
    float SteeringInputRiseRate = 2.65f;

    // Steering returns to center slightly faster than it moves toward full lock.
    UPROPERTY(EditAnywhere, Category="Vehicle|Input", meta=(ClampMin="0.1"))
    float SteeringInputReturnRate = 10.5f;

    // High speed reduces steering input rate, matching the slower hand-wheel motion of a real car.
    UPROPERTY(EditAnywhere, Category="Vehicle|Input", meta=(ClampMin="0.0", ClampMax="1.0"))
    float SteeringInputSpeedDamping = 0.36f;

    // Curves low analog values for softer initial turn-in while still allowing full lock.
    UPROPERTY(EditAnywhere, Category="Vehicle|Input", meta=(ClampMin="1.0", ClampMax="3.0"))
    float SteeringInputCurveExponent = 1.35f;

    // Vehicle control is integrated in small deterministic steps. Every force/torque is applied as
    // Force * StepDelta impulse, so one second of input produces the same total impulse in normal
    // Tick and in Chaos async physics tick.
    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.004", ClampMax="0.05"))
    float MaxVehiclePhysicsSubstepSeconds = 0.0166667f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="1", ClampMax="64"))
    int32 MaxVehiclePhysicsSubsteps = 60;

    // The subsystem update path is the authority. Mixing Actor async physics tick with normal tick
    // caused duplicate/flickering integration on projects where Chaos async tick availability changes.
    UPROPERTY(EditAnywhere, Category="Vehicle|Physics")
    bool bUseAsyncVehiclePhysicsTick = false;

    UPROPERTY(EditAnywhere, Category="Vehicle|Physics")
    bool bRunVehicleForcesInAsyncPhysicsTick = false;

    // Chassis clearance is intentionally lower than the tire diameter. A normal car body should sit
    // roughly half a wheel radius above the road, while wheels occupy the wheel wells.
    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.10", ClampMax="1.00"))
    float BodyGroundClearanceWheelRadiusRatio = 0.50f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension")
    bool bPreventWheelVisualGroundPenetration = true;

    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.0", ClampMax="6.0"))
    float WheelVisualGroundContactBuffer = 0.8f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.1", ClampMax="1.0"))
    float WheelVisualGroundSweepRadiusScale = 0.86f;

    // Hard bottom-out support used only when a wheel trace says the tire would be pushed into the road.
    // It prevents uphill ramp entry from swallowing the tire without acting as a constant ride-height lift.
    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.0"))
    float WheelGroundContactGuardStrength = 340000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.0"))
    float WheelGroundContactGuardDamping = 56000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.0"))
    float MaxWheelGroundContactGuardForce = 1050000.0f;

    // Extra compression support for curbs/steps. It lifts the chassis with the wheel instead of
    // letting the visual tire consume the whole step by itself.
    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.0"))
    float StepBodyFollowAssistStrength = 52000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.0"))
    float StepBodyFollowAssistDamping = 26000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.0"))
    float MaxStepBodyFollowAssistForce = 360000.0f;

    // Force smoothing must be independent from long game-thread frames. This keeps the
    // suspension/tire controller stable at low FPS and with async Chaos physics.
    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.004", ClampMax="0.05"))
    float MaxWheelForceControlDeltaSeconds = 0.0166667f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0", ClampMax="1.0"))
    float MinSuspensionHitNormalDot = 0.66f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0", ClampMax="1.0"))
    float SuspensionForceScale = 1.0f;

    // Hard cap for raycast suspension support. The cap allows the one-ton chassis to stay above the wheels
    // on uphill transitions and deep compression; asymmetric rebound damping still prevents hop.
    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="1.0", ClampMax="2.5"))
    float MaxSuspensionSupportMultiplier = 1.92f;

    // Downward damping applied only while grounded and moving upward quickly; this removes rebound pops
    // without teleporting or gluing the chassis to uneven terrain.
    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float GroundedReboundDamping = 36000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float MaxGroundedReboundDampingForce = 500000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float GroundedReboundSpeedThreshold = 48.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.1"))
    float SuspensionForceInterpSpeed = 155.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="1000.0"))
    float MaxSuspensionForceChangePerSecond = 14500000.0f;

    // Extra force-response speed used only when the suspension is compressing into the ground.
    // This compensates for the one-ton chassis without making rebound launch the car upward.
    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.1", ClampMax="5.0"))
    float SuspensionCompressionRiseMultiplier = 3.85f;

    // Rebound/release remains slower than compression so the chassis stays stable after bumps.
    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.1", ClampMax="5.0"))
    float SuspensionReboundReleaseMultiplier = 0.70f;

    // Uses measured wheel trace-length change as an early bump/compression signal. Body velocity alone
    // can react late when a rising curb or uneven ground reaches the tire before the chassis starts moving.
    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.0", ClampMax="1.0"))
    float SuspensionTraceCompressionVelocityBlend = 1.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0", ClampMax="1.0"))
    float TireLateralForceScale = 0.96f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.1"))
    float TireForceInterpSpeed = 22.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="1000.0"))
    float MaxLateralForceChangePerSecond = 480000.0f;

    // Direct pitch/roll angular damping used only while wheels are grounded.
    // This removes slow chassis wallow without locking yaw or snapping the car flat.
    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float GroundedPitchRollAngularDamping = 28.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.05"))
    float MaxGroundedPitchRollAngularVelocity = 0.40f;

    // Pitch/roll-only chassis stabilizer. It damps the loose body sway without locking yaw.
    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float GroundedPitchRollDampingTorque = 360000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float MaxGroundedPitchRollDampingTorque = 680000.0f;

    // Aligns the chassis toward the wheel-supported road plane, not flat world-up.
    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float TerrainAttitudeTorqueStrength = 176000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float TerrainAttitudeTorqueDamping = 440000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float MaxTerrainAttitudeTorque = 760000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.1"))
    float MaxGroundedPitchRollRateRadians = 0.58f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0"))
    float LowSpeedPitchRollAngularDamping = 28.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    FVector BodyExtent = FVector(160.0f, 78.0f, 42.0f);

    UPROPERTY(EditAnywhere, Category="Vehicle")
    TArray<FVector> WheelOffsets;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float MinimumBodyGroundClearance = 16.5f;

    // Soft skid-plate clearance guard. It only adds force when the chassis underside is already
    // below the intended clearance, so it prevents uphill ground digging without snapping to ground.
    UPROPERTY(EditAnywhere, Category="Vehicle")
    float ChassisAntiGroundStickStrength = 68000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float ChassisAntiGroundStickDamping = 24000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float MaxChassisAntiGroundStickForce = 460000.0f;

    UPROPERTY()
    TObjectPtr<UglTFRuntimeAsset> GltfAsset;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> LoadedBodyMeshComponents;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> LoadedWheelMeshComponents;

    UPROPERTY()
    TMap<int32, TObjectPtr<UStaticMesh>> MeshCache;


    UPROPERTY(ReplicatedUsing=OnRep_VehicleModelReplicationData)
    FString ReplicatedSourceFilePath;

    UPROPERTY(ReplicatedUsing=OnRep_VehicleModelReplicationData)
    FString ReplicatedObjectName;

    UFUNCTION()
    void OnRep_VehicleModelReplicationData();

    UPROPERTY()
    FString SourceFilePath;

    UPROPERTY()
    FString ObjectName = TEXT("Vehicle");

    UPROPERTY()
    FString BaseName = TEXT("Vehicle");

    TArray<FQuat> LoadedWheelBaseRotations;
    TArray<FVector> LoadedWheelVisualCenterOffsets;
    TArray<float> WheelTargetSpringLengths;
    FBox LoadedBodyVisualBounds = FBox(ForceInit);
    FBox LoadedWheelVisualRestBounds = FBox(ForceInit);

    float RuntimeWheelRadius = 0.0f;

    TArray<float> WheelSpinDegrees;
    TArray<float> WheelSpringLengths;
    TArray<float> WheelVisualSpringLengths;
    TArray<float> WheelSuspensionForces;
    TArray<float> WheelLateralForces;
    TArray<bool> WheelGrounded;

    float ThrottleInput = 0.0f;
    float SteeringInput = 0.0f;
    float SmoothedThrottleInput = 0.0f;
    float SmoothedSteeringInput = 0.0f;
    float SmoothedStableYawRate = 0.0f;
    FVector StablePlanarVelocity = FVector::ZeroVector;
    float StableVerticalVelocity = 0.0f;
    bool bStablePlanarVelocityInitialized = false;
    FVector StablePhysicsLinearVelocity = FVector::ZeroVector;
    FVector StablePhysicsAngularVelocity = FVector::ZeroVector;
    bool bStablePhysicsStateInitialized = false;

    FVector SmoothedRoadUp = FVector::UpVector;
    bool bHasSmoothedRoadUp = false;

    FThreadSafeCounter AsyncVehiclePhysicsStepCounter;
    int32 LastObservedAsyncVehiclePhysicsStepCounter = 0;
    bool bHasObservedAsyncVehiclePhysicsStep = false;
    bool bApplyingAsyncVehiclePhysicsStep = false;
    bool bSkipVehicleInputSmoothingForCurrentRun = false;
    float CurrentVehiclePhysicsStepSeconds = 0.0f;

    void RunVehiclePhysicsSteps(float DeltaSeconds, bool bFromAsyncPhysicsTick);
    void StepVehiclePhysics(float DeltaSeconds, bool bFromAsyncPhysicsTick);
    void UpdateVehicleInputSmoothing(float DeltaSeconds);
    void ResetVehicleTuningToClassDefaults();
    bool ApplyVehicleTuningJsonObject(const TSharedPtr<FJsonObject>& JsonObject);
    void AddVehicleForce(const FVector& Force);
    void AddVehicleForceAtLocation(const FVector& Force, const FVector& Location);
    void AddVehicleTorqueInRadians(const FVector& Torque);
    void UpdateStableWheelVehicle(float DeltaSeconds);
    void ApplySuspensionAndDrive(float DeltaSeconds);
    void ApplyAeroDownforce(int32 GroundedWheels);
    void ApplyAerodynamicDrag();
    float GetSteeringSpeedScale(float AbsForwardSpeed) const;
    void ApplyPitchStabilization(int32 GroundedWheels);
    void ApplyRollStabilization(int32 GroundedWheels);
    void ApplyGroundedPitchRollDamping(int32 GroundedWheels, float DeltaSeconds);
    void ApplyChassisClearanceProtection(UWorld* World, const FTransform& BodyTransform, const FCollisionQueryParams& QueryParams);
    void ApplyVehicleBodyPhysicsSettings();
    float GetVehicleMassScale() const;
    float GetEffectiveWheelRadius() const;
    float GetPhysicsBodyGroundClearance() const;
    float GetMinimumWheelSpringLength(int32 WheelIndex = INDEX_NONE) const;
    float GetEffectiveSuspensionRestLength(int32 WheelIndex = INDEX_NONE) const;
    float GetTargetWheelSpringLength(int32 WheelIndex = INDEX_NONE) const;
    float GetStableWheelVisualSpringLength() const;
    void ApplyStableVehicleGrounding(float DeltaSeconds);
    FVector GetFrontAxleForceLocation() const;
    void UpdateWheelVisuals(float DeltaSeconds);
    void BuildBodyMesh();
    void BuildWheelMeshes();
    void BuildWheelMesh(UProceduralMeshComponent* MeshComponent) const;
    void ClearLoadedVehicleModel();
    void ReleaseRuntimeResources();
    UStaticMesh* LoadMeshByIndex(int32 MeshIndex);
    FString ResolveVehicleTuningJsonPath(const FString& ModelPath) const;
    void HideProceduralDefaultVisuals(bool bHide);
    FVector GetExitLocation() const;
    bool FindSafeExitTransform(APawn* PawnToExit, FVector& OutLocation, FRotator& OutRotation) const;
    float GetDesiredCenterHeightAboveGround() const;
    float GetDownforceClearanceScale() const;
    void RestoreStoredPawnCamera(APlayerController* PlayerController, APawn* PawnToRestore) const;
};
