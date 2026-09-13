// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file StringHelper.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
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