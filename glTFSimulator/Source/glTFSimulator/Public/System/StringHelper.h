// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file StringHelper.h
 * 역할: 문자열의 공통 변환 기능을 제공합니다.
 * 핵심 기능: 문자열 결합·분할·부분 추출.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"

struct FStringHelper
{
    static FString GetTextBeforeChar(const FString &Input, char Delim);
    static FString GetTextAfterChar(const FString &Input, char Delim);
    static FString FindFirstStringWithPrefix(const TArray<FString> &KeyArray, const FString &Prefix);
    static FString ToString(bool Value);
    static FString Append(const TArray<FString> &Strings);
};