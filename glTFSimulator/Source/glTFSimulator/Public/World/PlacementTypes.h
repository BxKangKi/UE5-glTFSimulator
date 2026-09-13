// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Editor-facing object categories used only for stable generated names.

/**
 * @file PlacementTypes.h
 * 역할: 배치 도구에서 사용하는 공통 타입을 정의합니다.
 * 핵심 기능: 배치 모드·도구 구분.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "PlacementTypes.generated.h"

UENUM(BlueprintType)
enum class EPlacedObjectKind : uint8
{
    Static = 0 UMETA(DisplayName = "Static"),
    Vehicle = 1 UMETA(DisplayName = "Vehicle")
};
