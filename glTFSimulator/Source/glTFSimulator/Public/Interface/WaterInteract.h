// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file WaterInteract.h
 * 역할: 수면과 상호작용하는 객체의 인터페이스를 정의합니다.
 * 핵심 기능: 물 상호작용 이벤트 계약.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "WaterInteract.generated.h"

// Engine-internal class; do not edit manually.
UINTERFACE(MinimalAPI, Blueprintable)
class UWaterInteract : public UInterface
{
    GENERATED_BODY()
};

// Actual interface class exposed to gameplay code.
class GLTFSIMULATOR_API IWaterInteract
{
    GENERATED_BODY()

public:
    // Exposes the interface so both Blueprint and C++ can implement or call it.
    virtual void EnterWater(const float Level = 0.0f) = 0;

    // Exposes the interface so both Blueprint and C++ can implement or call it.
    virtual void ExitWater(const float Level = 0.0f) = 0;
};