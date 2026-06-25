// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "WaterInteract.generated.h"

// 엔진 내부용 클래스 (수정 X)
UINTERFACE(MinimalAPI, Blueprintable)
class UWaterInteract : public UInterface
{
    GENERATED_BODY()
};

// 실제 인터페이스 클래스
class GLTFSIMULATOR_API IWaterInteract
{
    GENERATED_BODY()

public:
    // 블루프린트와 C++ 모두에서 구현/호출 가능하도록 설정
    virtual void EnterWater(const float Level = 0.0f) = 0;

    // 블루프린트와 C++ 모두에서 구현/호출 가능하도록 설정
    virtual void ExitWater(const float Level = 0.0f) = 0;
};