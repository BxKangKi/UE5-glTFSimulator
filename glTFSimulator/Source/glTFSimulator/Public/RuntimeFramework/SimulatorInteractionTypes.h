#pragma once

#include "CoreMinimal.h"
#include "SimulatorInteractionTypes.generated.h"

UENUM(BlueprintType)
enum class ESimulatorHand : uint8
{
    Right UMETA(DisplayName="Right"),
    Left UMETA(DisplayName="Left")
};

UENUM(BlueprintType)
enum class ESimulatorGripRole : uint8
{
    Disabled,
    Primary,
    Secondary,
    Support
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FSimulatorFingerPose
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0", ClampMax="1")) float ThumbCurl = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0", ClampMax="1")) float IndexCurl = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0", ClampMax="1")) float MiddleCurl = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0", ClampMax="1")) float RingCurl = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0", ClampMax="1")) float LittleCurl = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="-1", ClampMax="1")) float Spread = 0.0f;

    void Clamp();
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FSimulatorGripPoint
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bEnabled = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) ESimulatorHand Hand = ESimulatorHand::Right;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) ESimulatorGripRole Role = ESimulatorGripRole::Primary;

    /** Character socket/bone used when the object is physically attached. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName CharacterSocket = NAME_None;

    /** Optional equipment scene-component tag. If absent, the equipment root is used. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName EquipmentAnchorTag = NAME_None;

    /** Transform from the equipment anchor to the desired hand IK target. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FTransform LocalTarget = FTransform::Identity;

    /** Object transform relative to the character socket while directly attached. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FTransform AttachmentOffset = FTransform::Identity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0", ClampMax="1")) float IKAlpha = 1.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FSimulatorFingerPose Fingers;
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FSimulatorFullBodyIKConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bEnabled = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0.001")) float TransformInterpSpeed = 18.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0.001")) float AlphaInterpSpeed = 12.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0", ClampMax="2")) float MaxArmStretchRatio = 1.05f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0", ClampMax="90")) float MaxSpineYawDegrees = 28.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0", ClampMax="90")) float MaxSpinePitchDegrees = 18.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0", ClampMax="45")) float MaxTurnLeanDegrees = 8.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0", ClampMax="45")) float MaxMoveLeanDegrees = 6.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0", ClampMax="1")) float PelvisWeight = 0.15f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0", ClampMax="1")) float SpineWeight = 0.55f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0", ClampMax="1")) float ShoulderWeight = 0.30f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="0")) float MaxYawRateForFullLean = 180.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName PelvisBone = TEXT("pelvis");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName SpineBone = TEXT("spine_03");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName LeftClavicleBone = TEXT("clavicle_l");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName RightClavicleBone = TEXT("clavicle_r");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName LeftUpperArmBone = TEXT("upperarm_l");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName RightUpperArmBone = TEXT("upperarm_r");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName LeftLowerArmBone = TEXT("lowerarm_l");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName RightLowerArmBone = TEXT("lowerarm_r");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName LeftHandBone = TEXT("hand_l");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName RightHandBone = TEXT("hand_r");
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FSimulatorCharacterInteractionConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 SchemaVersion = 1;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) ESimulatorHand DominantHand = ESimulatorHand::Right;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName RightHandSocket = TEXT("hand_r_socket");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName LeftHandSocket = TEXT("hand_l_socket");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FSimulatorFullBodyIKConfig FullBodyIK;
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FSimulatorEquipmentInteractionConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 SchemaVersion = 1;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bOverridePrimaryHand = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) ESimulatorHand PrimaryHandOverride = ESimulatorHand::Right;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FSimulatorGripPoint RightGrip;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FSimulatorGripPoint LeftGrip;

    /** Longest dimension for a held prefab miniature, in Unreal centimeters. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="1", ClampMax="100")) float HeldPreviewLongestDimensionCm = 18.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite) FSimulatorFullBodyIKConfig FullBodyIK;

    ESimulatorHand ResolvePrimaryHand(ESimulatorHand CharacterDominantHand) const;
    const FSimulatorGripPoint* GetGrip(ESimulatorHand Hand) const;
    void Sanitize();
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FSimulatorResolvedHandIK
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) bool bValid = false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FTransform TargetWorld = FTransform::Identity;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FTransform TargetComponentSpace = FTransform::Identity;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) float Alpha = 0.0f;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FSimulatorFingerPose Fingers;
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FSimulatorResolvedFullBodyIK
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FSimulatorResolvedHandIK LeftHand;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FSimulatorResolvedHandIK RightHand;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FSimulatorResolvedHandIK PrimaryHand;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FSimulatorResolvedHandIK SecondaryHand;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FRotator PelvisRotation = FRotator::ZeroRotator;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FRotator SpineRotation = FRotator::ZeroRotator;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FRotator ShoulderRotation = FRotator::ZeroRotator;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) float BodyAlpha = 0.0f;
};
