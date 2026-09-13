// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file StringHelper.cpp
 * 역할: 문자열의 공통 변환 기능을 제공합니다.
 * 핵심 기능: 문자열 결합·분할·부분 추출.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "System/StringHelper.h"

FString FStringHelper::GetTextBeforeChar(const FString &Input, char Delim)
{
    int32 Index = 0;
    if (Input.FindChar(Delim, Index))
    {
        return Input.Left(Index);
    }
    return Input; // Return the full string when the delimiter is missing.
}

FString FStringHelper::GetTextAfterChar(const FString &Input, char Delim)
{
    int32 Index = 0;
    if (Input.FindChar(Delim, Index))
    {
        return Input.Mid(Index + 1); // Return the substring after Delim.
    }
    return FString(); // Return an empty string when the delimiter is missing.
}

FString FStringHelper::FindFirstStringWithPrefix(const TArray<FString> &KeyArray, const FString &Prefix)
{
    for (const FString &Key : KeyArray)
    {
        if (Key.StartsWith(Prefix))
        {
            return Key;
        }
    }
    return FString("");
}

FString FStringHelper::ToString(bool InValue)
{
    return InValue ? TEXT("true") : TEXT("false");
}

FString FStringHelper::Append(const TArray<FString> &Strings)
{
    FString Result;
    for (const FString &Str : Strings)
    {
        Result.Append(Str);
    }
    return Result;
}