// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file JsonHelper.cpp
 * 역할: JSON과 Unreal 기본 타입 사이의 변환을 제공합니다.
 * 핵심 기능: enum·벡터·회전 등 공통 값 직렬화.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "System/JsonHelper.h"

void FJsonHelper::SetVector(const TSharedRef<FJsonObject> &Json, const FVector &Vector, const FString &KeyPrefix)
{
    // Follows the original X/Y/Z naming rule while allowing an optional prefix such as "Center".
    FString PX = KeyPrefix.IsEmpty() ? TEXT("X") : KeyPrefix + TEXT("X");
    FString PY = KeyPrefix.IsEmpty() ? TEXT("Y") : KeyPrefix + TEXT("Y");
    FString PZ = KeyPrefix.IsEmpty() ? TEXT("Z") : KeyPrefix + TEXT("Z");

    Json->SetNumberField(PX, Vector.X);
    Json->SetNumberField(PY, Vector.Y);
    Json->SetNumberField(PZ, Vector.Z);
}

// 2. Read an FVector from a JSON object.
void FJsonHelper::TryGetVector(const TSharedPtr<FJsonObject> &Json, FVector &OutVector, const FString &KeyPrefix)
{
    if (!Json.IsValid()) return;

    FString PX = KeyPrefix.IsEmpty() ? TEXT("X") : KeyPrefix + TEXT("X");
    FString PY = KeyPrefix.IsEmpty() ? TEXT("Y") : KeyPrefix + TEXT("Y");
    FString PZ = KeyPrefix.IsEmpty() ? TEXT("Z") : KeyPrefix + TEXT("Z");

    Json->TryGetNumberField(PX, OutVector.X);
    Json->TryGetNumberField(PY, OutVector.Y);
    Json->TryGetNumberField(PZ, OutVector.Z);
}

// Returns every top-level key from an FJsonObject.
TArray<FString> FJsonHelper::GetAllKeysFromJsonObject(const TSharedPtr<FJsonObject> &JsonObject)
{
    TArray<FString> Keys;
    if (JsonObject.IsValid())
    {
        Keys.Reserve(JsonObject->Values.Num());
        for (const auto& Pair : JsonObject->Values)
        {
            Keys.Add(FString(Pair.Key));
        }
    }
    return Keys;
}
