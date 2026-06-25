// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "UpdateRainAsync.generated.h"

class USceneCaptureComponent2D;
class UNiagaraComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnUpdateRainCompleted);

UCLASS()
class GLTFSIMULATOR_API UUpdateRainAsync : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintAssignable, Category = "Rain|Async")
    FOnUpdateRainCompleted Completed;

    UFUNCTION(BlueprintCallable,
              Category = "Rain|Async",
              meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
    static UUpdateRainAsync *UpdateRainAsync(
        UObject *WorldContextObject,
        USceneCaptureComponent2D *ViewComp,
        UNiagaraComponent *NiagaraComp,
        const float InMaxViewDist,
        const FName &InParam);

    virtual void Activate() override;

private:
    // Stored inputs
    UPROPERTY()
    TObjectPtr<UObject> WorldContextObject;
    UPROPERTY()
    TWeakObjectPtr<USceneCaptureComponent2D> ViewCompPtr;
    UPROPERTY()
    TWeakObjectPtr<UNiagaraComponent> NiagaraCompPtr;
    float OrthoWidth = 0.f;
    float NearPlane = 0.f;
    float MaxViewDist = 0.f;
    FName Param;
};