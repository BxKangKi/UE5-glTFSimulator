// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "ComputeFileHashAsyncAction.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnHashComputed, FString, Hash);

UCLASS()
class GLTFSIMULATOR_API UComputeFileHashAsyncAction : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    // 블루프린트에서 호출할 정적 함수
    UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true"), Category = "File|Hash")
    static UComputeFileHashAsyncAction *ComputeFileHashAsync(UObject *WorldContextObject, const FString &FilePath);

    // 블루프린트 이벤트 바인딩
    UPROPERTY(BlueprintAssignable)
    FOnHashComputed OnCompleted;

    // 비동기 실행 함수 오버라이드
    virtual void Activate() override;

private:
    FString TargetFilePath;

    UPROPERTY()
    TObjectPtr<UObject> WorldContextObject;
};