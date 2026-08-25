// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

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
