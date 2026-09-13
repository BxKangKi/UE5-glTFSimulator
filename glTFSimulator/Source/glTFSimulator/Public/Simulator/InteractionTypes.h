/**
 * @file InteractionTypes.h
 * 역할: 장비·객체 상호작용 공통 타입을 정의합니다.
 * 핵심 기능: 부착 위치·상호작용 상태 표현.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "InteractionTypes.generated.h"

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
struct GLTFSIMULATOR_API FSimulatorGripPoint
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bEnabled = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) ESimulatorHand Hand = ESimulatorHand::Right;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) ESimulatorGripRole Role = ESimulatorGripRole::Primary;

    /** Character socket/bone used when the object is physically attached. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName CharacterSocket = NAME_None;

    /** Object transform relative to the character socket while directly attached. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FTransform AttachmentOffset = FTransform::Identity;
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FSimulatorCharacterInteractionConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 SchemaVersion = 1;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) ESimulatorHand DominantHand = ESimulatorHand::Right;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName RightHandSocket = TEXT("hand_r_socket");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FName LeftHandSocket = TEXT("hand_l_socket");
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

    /** Longest dimension for a held Static miniature, in Unreal centimeters. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin="1", ClampMax="100")) float HeldPreviewLongestDimensionCm = 18.0f;

    ESimulatorHand ResolvePrimaryHand(ESimulatorHand CharacterDominantHand) const;
    const FSimulatorGripPoint* GetGrip(ESimulatorHand Hand) const;
    void Sanitize();
};
