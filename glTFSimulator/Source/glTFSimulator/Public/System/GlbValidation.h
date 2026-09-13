// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file GlbValidation.h
 * 역할: 외부 GLB 파일의 구조와 읽기 범위를 검사합니다.
 * 핵심 기능: 헤더·청크·크기 검증, 경로 정규화, 잘못된 입력 거부.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"

/** Lightweight validation helpers for untrusted binary glTF files. */
namespace GlbValidation
{
    /** Converts a supplied file path to a normalized absolute path. */
    GLTFSIMULATOR_API FString NormalizePath(const FString& FilePath);

    /** Validates a binary glTF container without parsing mesh data. */
    GLTFSIMULATOR_API bool ValidateFile(const FString& FilePath, FString& OutReason);

    /**
     * Performs the expensive authoring preflight on a worker before the build-only glTFRuntime
     * parser is called. JSON types, buffer ranges, accessors, primitives and allocation estimates
     * are bounded so malformed source data cannot request impossible native allocations.
     */
    GLTFSIMULATOR_API bool ValidateBuildSourceFile(const FString& FilePath, FString& OutReason);
}
