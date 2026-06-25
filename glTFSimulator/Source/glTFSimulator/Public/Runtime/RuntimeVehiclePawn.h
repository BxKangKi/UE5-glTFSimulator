// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Runtime/RuntimePlacementTypes.h"
#include "RuntimeVehiclePawn.generated.h"

class UBoxComponent;
class UPhysicalMaterial;
class UProceduralMeshComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UglTFRuntimeAsset;
class URuntimeBuoyancyComponent;
class USpringArmComponent;
class UCameraComponent;
class APlayerController;
class UInputComponent;
class UWorld;
struct FCollisionQueryParams;

UCLASS(BlueprintType)
class GLTFSIMULATOR_API ARuntimeVehiclePawn : public APawn
{
    GENERATED_BODY()

public:
    ARuntimeVehiclePawn();

    UFUNCTION(BlueprintCallable, Category="Runtime Vehicle")
    bool EnterVehicle(APlayerController* PlayerController, APawn* PreviousPawn);

    UFUNCTION(BlueprintCallable, Category="Runtime Vehicle")
    void ExitVehicle();

    UFUNCTION(BlueprintPure, Category="Runtime Vehicle")
    bool IsOccupied() const { return IsValid(OccupyingController); }

    UFUNCTION(BlueprintCallable, Category="Runtime Vehicle|Input")
    void SetDriveInput(float Throttle, float Steering);

    UFUNCTION(BlueprintCallable, Category="Runtime Vehicle|Input")
    void SetThrottleInput(float Throttle);

    UFUNCTION(BlueprintCallable, Category="Runtime Vehicle|Input")
    void SetSteeringInput(float Steering);

    UFUNCTION(BlueprintCallable, Category="Runtime Vehicle|Input")
    void ClearDriveInput();

    UFUNCTION(BlueprintPure, Category="Runtime Vehicle")
    APawn* GetStoredPawn() const { return StoredPawn.Get(); }

    UFUNCTION(BlueprintCallable, Category="Runtime Vehicle|Model")
    bool LoadVehicleModel(const FString& InFilePath, const FString& InRuntimeName);

    UFUNCTION(BlueprintCallable, Category="Runtime Vehicle|Physics")
    void ResetVehiclePoseAboveGround();

    UFUNCTION(BlueprintCallable, Category="Runtime Vehicle|Save")
    FRuntimePlacedObjectRecord ToPlacementRecord(int32 VehicleRecordIndex = 0) const;

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
    TObjectPtr<URuntimeBuoyancyComponent> BuoyancyComponent;

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
    float SuspensionStrength = 29000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float SuspensionDamping = 10400.0f;


    UPROPERTY(EditAnywhere, Category="Vehicle")
    float MaxSuspensionForcePerWheel = 460000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float UprightTorqueDamping = 210000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float LoadedWheelVisualRestLengthRatio = 0.74f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float GroundedDownforceCoefficient = 0.024f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float MaxGroundedDownforce = 820000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float AirborneDownforceCoefficient = 0.020f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float MaxAirborneDownforce = 620000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float MinimumDownforceSpeed = 360.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float FrontDownforceCoefficient = 0.013f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float MaxFrontDownforce = 280000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Aero", meta=(ClampMin="0.0"))
    float ThrottleFrontDownforce = 85000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Pitch", meta=(ClampMin="0.0"))
    float PitchStabilizationTorqueStrength = 420000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Pitch", meta=(ClampMin="0.0"))
    float PitchStabilizationTorqueDamping = 210000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Pitch", meta=(ClampMin="0.0"))
    float MaxPitchStabilizationTorque = 720000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Pitch", meta=(ClampMin="0.0", ClampMax="1.0"))
    float DriveForceCenterOfMassHeightBlend = 0.68f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Roll", meta=(ClampMin="0.0", ClampMax="1.0"))
    float LateralForceCenterOfMassHeightBlend = 0.55f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Roll", meta=(ClampMin="0.0"))
    float RollStabilizationTorqueStrength = 380000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Roll", meta=(ClampMin="0.0"))
    float RollStabilizationTorqueDamping = 190000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Roll", meta=(ClampMin="0.0"))
    float MaxRollStabilizationTorque = 680000.0f;

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
    float SteeringYawRateAssist = 220000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.0"))
    float SteeringYawDamping = 150000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.0"))
    float MaxSteeringAssistTorque = 340000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.1"))
    float FrontSteeringGripMultiplier = 0.90f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering", meta=(ClampMin="0.1"))
    float RearSteeringGripMultiplier = 1.05f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering")
    float LateralGrip = 0.86f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Steering")
    float MaxLateralGripForce = 640000.0f;

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
    float TireLateralFriction = 1.08f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="0.1"))
    float TireCorneringStiffness = 5.4f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="1.0"))
    float TireSlipReferenceSpeed = 260.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Tires", meta=(ClampMin="0.1", ClampMax="1.0"))
    float HighSpeedLateralGripScale = 0.68f;

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
    float AntiRollBarStiffness = 420000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Roll", meta=(ClampMin="0.0"))
    float MaxAntiRollForce = 360000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Air", meta=(ClampMin="0.0"))
    float AirborneAngularDampingTorque = 115000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Air", meta=(ClampMin="0.0"))
    float MaxAirborneAngularDampingTorque = 300000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Air", meta=(ClampMin="0.5"))
    float MaxAngularVelocityRadians = 4.8f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Input", meta=(ClampMin="0.1"))
    float ThrottleInputInterpSpeed = 4.3f;

    UPROPERTY(EditAnywhere, Category="Vehicle|Input", meta=(ClampMin="0.1"))
    float SteeringInputInterpSpeed = 5.2f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    FVector BodyExtent = FVector(160.0f, 78.0f, 42.0f);

    UPROPERTY(EditAnywhere, Category="Vehicle")
    TArray<FVector> WheelOffsets;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float MinimumBodyGroundClearance = 48.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float ChassisAntiGroundStickStrength = 26000.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float ChassisAntiGroundStickDamping = 9200.0f;

    UPROPERTY(EditAnywhere, Category="Vehicle")
    float MaxChassisAntiGroundStickForce = 260000.0f;

    UPROPERTY()
    TObjectPtr<UglTFRuntimeAsset> RuntimeAsset;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> LoadedBodyMeshComponents;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> LoadedWheelMeshComponents;

    UPROPERTY()
    TMap<int32, TObjectPtr<UStaticMesh>> MeshCache;

    UPROPERTY()
    FString SourceFilePath;

    UPROPERTY()
    FString RuntimeName = TEXT("Vehicle");

    UPROPERTY()
    FString BaseName = TEXT("Vehicle");

    TArray<FQuat> LoadedWheelBaseRotations;

    TArray<float> WheelSpinDegrees;
    TArray<float> WheelSpringLengths;
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
