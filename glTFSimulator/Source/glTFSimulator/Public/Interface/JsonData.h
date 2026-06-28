// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "Dom/JsonObject.h"
#include "JsonData.generated.h"

// 엔진 내부용 클래스 (수정 X)
UINTERFACE(MinimalAPI, Blueprintable)
class UJsonData : public UInterface
{
    GENERATED_BODY()
};

// 실제 인터페이스 클래스
class GLTFSIMULATOR_API IJsonData
{
    GENERATED_BODY()

public:
    virtual TSharedRef<FJsonObject> Serialization() = 0;
    virtual bool Deserialization(TSharedPtr<FJsonObject> Json) = 0;
};