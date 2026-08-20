#include "Animation/CharacterTurnBody.h"
#include "Rigs/RigHierarchy.h"

// 일반적인 Execute() 대신 UHT가 생성한 Execute 매크로를 사용합니다.
// 내부적으로 StaticExecute 시그니처로 치환되며, ExecuteContext, BoneName 등의 입력값이 인자로 자동 전달됩니다.
FRigUnit_CharacterTurnBody_Execute()
{
    // Hierarchy 컨텍스트 유효성 검사
    URigHierarchy* Hierarchy = ExecuteContext.Hierarchy;
    if (Hierarchy == nullptr)
    {
        return;
    }

    FRigElementKey BoneKey(BoneName, ERigElementType::Bone);
    if (!Hierarchy->Contains(BoneKey))
    {
        return;
    }

    // 1. 회전 속도를 기반으로 보간 비율 계산 (-1.0 ~ 1.0)
    float SafeMaxSpeed = FMath::Max(KINDA_SMALL_NUMBER, MaxRotationSpeed);
    float Alpha = FMath::Clamp(RotationSpeed / SafeMaxSpeed, -1.0f, 1.0f);
    float TargetAngle = Alpha * MaxLeanAngle;

    // 2. 현재 로컬 트랜스폼 획득
    FTransform LocalTransform = Hierarchy->GetLocalTransform(BoneKey);

    // 3. 선택된 축에 회전 적용 (Additive 연산)
    FRotator DeltaRot = FRotator::ZeroRotator;
    switch (LeanAxis)
    {
        case EAxis::X: 
            DeltaRot.Roll = TargetAngle; 
            break;
        case EAxis::Y: 
            DeltaRot.Pitch = TargetAngle; 
            break;
        case EAxis::Z: 
            DeltaRot.Yaw = TargetAngle; 
            break;
    }

    // 4. 새로운 로컬 트랜스폼 연산 및 세팅 (쿼터니언 곱을 사용해 짐벌락 방지)
    FQuat NewRotation = LocalTransform.GetRotation() * DeltaRot.Quaternion();
    LocalTransform.SetRotation(NewRotation);
    
    Hierarchy->SetLocalTransform(BoneKey, LocalTransform);
}