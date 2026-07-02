// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Gameplay/PlacementTypes.h"
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

    UFUNCTION(BlueprintPure, Category="Vehicle")
    APawn* GetStoredPawn() const { return StoredPawn.Get(); }

    UFUNCTION(BlueprintCallable, Category="Vehicle|Model")
    bool LoadVehicleModel(const FString& InFilePath, const FString& InObjectName);

    UFUNCTION(BlueprintCallable, Category="Vehicle|Physics")
    void ResetVehiclePoseAboveGround();

    UFUNCTION(BlueprintCallable, Category="Vehicle|Save")
    FPlacedObjectRecord ToPlacementRecord(int32 VehicleRecordIndex = 0) const;

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
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

    UPROPERTY(Transient)
    TObjectPtr<UPhysicalMaterial> LowFrictionPhysicalMaterial;

    UPROPERTY(EditAnywhere, Category="Vehicle", meta=(ClampMin="1.0"))
    float VehicleMassKg = 1000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float SuspensionRestLength = 54.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float SuspensionTraceExtra = 42.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float SuspensionStrength = 8500.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float SuspensionDamping = 4200.0f;

    // Limits spring speed used by the damper so the suspension cannot inject a large impulse after a sharp contact.
    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.0"))
    float MaxSuspensionVelocity = 180.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.0"))
    float SuspensionContactSmoothingSpeed = 6.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension")
    bool bUseSuspensionSweep = true;

    UPROPERTY(EditAnywhere, Category="Vehicle|Suspension", meta=(ClampMin="0.05", ClampMax="1.0"))
    float SuspensionSweepRadiusScale = 0.42f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float MaxSuspensionForcePerWheel = 95000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float UprightTorqueDamping = 90000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float LoadedWheelVisualRestLengthRatio = 0.74f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float GroundedDownforceCoefficient = 0.006f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float MaxGroundedDownforce = 180000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float AirborneDownforceCoefficient = 0.004f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float MaxAirborneDownforce = 120000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float MinimumDownforceSpeed = 650.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float FrontDownforceCoefficient = 0.0025f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float MaxFrontDownforce = 45000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float ThrottleFrontDownforce = 15000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Pitch", meta=(ClampMin="0.0"))
    float PitchStabilizationTorqueStrength = 85000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Pitch", meta=(ClampMin="0.0"))
    float PitchStabilizationTorqueDamping = 65000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Pitch", meta=(ClampMin="0.0"))
    float MaxPitchStabilizationTorque = 140000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Pitch", meta=(ClampMin="0.0", ClampMax="1.0"))
    float DriveForceCenterOfMassHeightBlend = 0.25f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Roll", meta=(ClampMin="0.0", ClampMax="1.0"))
    float LateralForceCenterOfMassHeightBlend = 0.18f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Roll", meta=(ClampMin="0.0"))
    float RollStabilizationTorqueStrength = 60000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Roll", meta=(ClampMin="0.0"))
    float RollStabilizationTorqueDamping = 50000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Roll", meta=(ClampMin="0.0"))
    float MaxRollStabilizationTorque = 120000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float EngineForce = 620000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float ReverseForce = 260000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float BrakeForce = 1250000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.0"))
    float SteeringTorque = 180000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="1.0", ClampMax="55.0"))
    float MaxSteeringAngleDegrees = 28.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.0"))
    float MinSteeringSpeedFactor = 0.24f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.0"))
    float MaxSteeringSpeedFactor = 1.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="1.0"))
    float SteeringSpeedForFullAssist = 2600.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.0"))
    float SteeringYawRateAssist = 65000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.0"))
    float SteeringYawDamping = 90000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.0"))
    float MaxSteeringAssistTorque = 120000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.1"))
    float FrontSteeringGripMultiplier = 0.90f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.1"))
    float RearSteeringGripMultiplier = 1.05f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering")
    float LateralGrip = 0.38f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering")
    float MaxLateralGripForce = 160000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float RollingResistance = 0.018f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float UprightTorqueStrength = 145000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float WheelRadius = 33.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float WheelWidth = 24.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float MaxSpeedForward = 6200.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="0.1"))
    float TireLongitudinalFriction = 1.18f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="0.1"))
    float TireLateralFriction = 0.62f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="0.1"))
    float TireCorneringStiffness = 2.2f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="1.0"))
    float TireSlipReferenceSpeed = 260.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="0.1", ClampMax="1.0"))
    float HighSpeedLateralGripScale = 0.42f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="100.0"))
    float HighSpeedLateralGripSpeed = 2600.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="0.0", ClampMax="1.0"))
    float DrivenFrontTorqueShare = 0.38f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="0.0"))
    float EngineBrakingForce = 90000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="1.0", ClampMax="45.0"))
    float HighSpeedSteeringAngleDegrees = 5.5f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.0", ClampMax="1.0"))
    float AckermannStrength = 0.65f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float AerodynamicDragCoefficient = 0.018f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float MaxAerodynamicDrag = 680000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Roll", meta=(ClampMin="0.0"))
    float AntiRollBarStiffness = 30000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Roll", meta=(ClampMin="0.0"))
    float MaxAntiRollForce = 65000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Air", meta=(ClampMin="0.0"))
    float AirborneAngularDampingTorque = 60000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Air", meta=(ClampMin="0.0"))
    float MaxAirborneAngularDampingTorque = 140000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Air", meta=(ClampMin="0.5"))
    float MaxAngularVelocityRadians = 3.2f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Input", meta=(ClampMin="0.1"))
    float ThrottleInputInterpSpeed = 4.3f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Input", meta=(ClampMin="0.1"))
    float SteeringInputInterpSpeed = 3.2f;

    // Deprecated tuning placeholders kept for serialized Blueprint defaults.
    // Forces are now applied once per tick because manual force-loop substepping caused suspension jitter.
    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.004", ClampMax="0.05"))
    float MaxVehiclePhysicsSubstepSeconds = 0.0166667f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="1", ClampMax="8"))
    int32 MaxVehiclePhysicsSubsteps = 4;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0", ClampMax="1.0"))
    float MinSuspensionHitNormalDot = 0.30f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0", ClampMax="1.0"))
    float SuspensionForceScale = 0.42f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.1"))
    float SuspensionForceInterpSpeed = 4.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="1000.0"))
    float MaxSuspensionForceChangePerSecond = 70000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.0", ClampMax="1.0"))
    float TireLateralForceScale = 0.42f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="0.1"))
    float TireForceInterpSpeed = 5.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Stability", meta=(ClampMin="1000.0"))
    float MaxLateralForceChangePerSecond = 110000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    FVector BodyExtent = FVector(160.0f, 78.0f, 42.0f);

    UPROPERTY(EditAnywhere, Category="Vehicle")
    TArray<FVector> WheelOffsets;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float MinimumBodyGroundClearance = 48.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float ChassisAntiGroundStickStrength = 3500.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float ChassisAntiGroundStickDamping = 2200.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float MaxChassisAntiGroundStickForce = 28000.0f;

    UPROPERTY()
    TObjectPtr<UglTFRuntimeAsset> GltfAsset;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> LoadedBodyMeshComponents;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> LoadedWheelMeshComponents;

    UPROPERTY()
    TMap<int32, TObjectPtr<UStaticMesh>> MeshCache;

    UPROPERTY()
    FString SourceFilePath;

    UPROPERTY()
    FString ObjectName = TEXT("Vehicle");

    UPROPERTY()
    FString BaseName = TEXT("Vehicle");

    TArray<FQuat> LoadedWheelBaseRotations;

    TArray<float> WheelSpinDegrees;
    TArray<float> WheelSpringLengths;
    TArray<float> WheelSuspensionForces;
    TArray<float> WheelLateralForces;
    TArray<bool> WheelGrounded;

    float ThrottleInput = 0.0f;
    float SteeringInput = 0.0f;
    float SmoothedThrottleInput = 0.0f;
    float SmoothedSteeringInput = 0.0f;


    void ApplySuspensionAndDrive(float DeltaSeconds);
    void ApplyAeroDownforce(int32 GroundedWheels);
    void ApplyAerodynamicDrag();
    float GetSteeringSpeedScale(float AbsForwardSpeed) const;
    void ApplyPitchStabilization(int32 GroundedWheels);
    void ApplyRollStabilization(int32 GroundedWheels);
    void ApplyChassisClearanceProtection(UWorld* World, const FTransform& BodyTransform, const FCollisionQueryParams& QueryParams);
    void ApplyVehicleBodyPhysicsSettings();
    float GetVehicleMassScale() const;
    FVector GetFrontAxleForceLocation() const;
    void UpdateWheelVisuals(float DeltaSeconds);
    void BuildBodyMesh();
    void BuildWheelMeshes();
    void BuildWheelMesh(UProceduralMeshComponent* MeshComponent) const;
    void ClearLoadedVehicleModel();
    UStaticMesh* LoadMeshByIndex(int32 MeshIndex);
    void HideProceduralDefaultVisuals(bool bHide);
    FVector GetExitLocation() const;
    bool FindSafeExitTransform(APawn* PawnToExit, FVector& OutLocation, FRotator& OutRotation) const;
    float GetDesiredCenterHeightAboveGround() const;
    float GetDownforceClearanceScale() const;
    void RestoreStoredPawnCamera(APlayerController* PlayerController, APawn* PawnToRestore) const;
};
