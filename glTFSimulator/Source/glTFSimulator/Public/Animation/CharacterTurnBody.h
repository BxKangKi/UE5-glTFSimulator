#pragma once

#include "CoreMinimal.h"
#include "Units/RigUnit.h"
#include "ControlRigDefines.h"
#include "CharacterTurnBody.generated.h"

// 카테고리와 노드 색상을 지정하여 Control Rig 에디터에서 쉽게 찾을 수 있도록 설정합니다.
USTRUCT(meta = (DisplayName = "Character Turn Body", Category = "Character", NodeColor = "0.1 0.8 0.1"))
struct GLTFSIMULATOR_API FRigUnit_CharacterTurnBody : public FRigUnitMutable
{
    GENERATED_BODY()

    FRigUnit_CharacterTurnBody()
        : BoneName(NAME_None)
        , RotationSpeed(0.f)
        , MaxRotationSpeed(100.f)
        , MaxLeanAngle(30.f)
        , LeanAxis(EAxis::X)
    {}

    // RigVM 실행 매크로
    RIGVM_METHOD()
    virtual void Execute() override;

    // -------------------------------------------------------------------------
    // Properties
    // -------------------------------------------------------------------------

    // Control Rig 실행 컨텍스트 (필수)
    UPROPERTY(meta=(Input, Output))
    FControlRigExecuteContext ExecuteContext;

    // 회전을 적용할 뼈 이름 (에디터 내 콤보박스 위젯 활성화)
    UPROPERTY(meta=(Input, CustomWidget="BoneName"))
    FName BoneName;

    // 현재 회전 속도 (입력 핀)
    UPROPERTY(meta=(Input))
    float RotationSpeed;

    // 기준이 되는 최대 회전 속도 (정규화를 위해 사용)
    UPROPERTY(meta=(Input))
    float MaxRotationSpeed;

    // 최대 속도일 때 도달할 최대 기울기 각도 (도 단위)
    UPROPERTY(meta=(Input))
    float MaxLeanAngle;

    // 뼈 구조에 따라 기울어질 축 설정 (일반적으로 상하체 롤(Roll)은 X축)
    UPROPERTY(meta=(Input))
    TEnumAsByte<EAxis::Type> LeanAxis;
};