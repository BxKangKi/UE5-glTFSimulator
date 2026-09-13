// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file ComputeFileHashAsyncAction.h
 * 역할: 파일 해시 계산을 비동기로 수행합니다.
 * 핵심 기능: 파일 읽기·해시 계산, Blueprint 완료 통지와 취소.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

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
    // Static function callable from Blueprint.
    UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true"), Category = "File|Hash")
    static UComputeFileHashAsyncAction *ComputeFileHashAsync(UObject *WorldContextObject, const FString &FilePath);

    // Blueprint event binding.
    UPROPERTY(BlueprintAssignable)
    FOnHashComputed OnCompleted;

    // Asynchronous execution override.
    virtual void Activate() override;

private:
    FString TargetFilePath;

    UPROPERTY()
    TObjectPtr<UObject> WorldContextObject;
};