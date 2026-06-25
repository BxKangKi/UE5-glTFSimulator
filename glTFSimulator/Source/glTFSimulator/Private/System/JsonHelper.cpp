// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "System/JsonHelper.h"

void FJsonHelper::SetVector(const TSharedRef<FJsonObject> &Json, const FVector &Vector, const FString &KeyPrefix)
{
    // 원본 코드의 규칙(X, Y, Z 단독 명시)을 따르되, 필요 시 Prefix(예: "Center")를 붙일 수 있도록 설계
    FString PX = KeyPrefix.IsEmpty() ? TEXT("X") : KeyPrefix + TEXT("X");
    FString PY = KeyPrefix.IsEmpty() ? TEXT("Y") : KeyPrefix + TEXT("Y");
    FString PZ = KeyPrefix.IsEmpty() ? TEXT("Z") : KeyPrefix + TEXT("Z");

    Json->SetNumberField(PX, Vector.X);
    Json->SetNumberField(PY, Vector.Y);
    Json->SetNumberField(PZ, Vector.Z);
}

// 2. JSON Object -> FVector 추출
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

// FJsonObject에서 모든 최상위 키를 가져오는 함수
TArray<FString> FJsonHelper::GetAllKeysFromJsonObject(const TSharedPtr<FJsonObject> &JsonObject)
{
    TArray<FString> Keys;
    if (JsonObject.IsValid())
    {
        JsonObject->Values.GetKeys(Keys);
    }
    return Keys;
}
