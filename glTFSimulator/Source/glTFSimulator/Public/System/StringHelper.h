// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

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