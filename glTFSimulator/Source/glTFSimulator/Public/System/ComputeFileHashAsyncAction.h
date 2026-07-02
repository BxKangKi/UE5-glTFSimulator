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