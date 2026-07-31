// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "World/LightRotation.h"
#include "SkyUpdateAsyncAction.generated.h"

class UWorldData;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FWorldAsyncCompleteDelegate, FLightRotation, Result);

UCLASS()
class GLTFSIMULATOR_API USkyUpdateAsyncAction : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()
public:
    /** Creates the Blueprint-compatible action wrapper. */
    UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"), Category = "World|Async")
    static USkyUpdateAsyncAction *SkyUpdateAsync(UObject *WorldContextObject, UWorldData *Data);

    /** Calculates both light rotations without allocating an action object. */
    static FLightRotation CalculateLightRotation(const UWorldData* World);

    /** Output after the Blueprint action is completed. */
    UPROPERTY(BlueprintAssignable)
    FWorldAsyncCompleteDelegate OnCompleted;

    virtual void Activate() override;

private:
    UPROPERTY()
    TObjectPtr<UObject> WorldContextObject;

    UPROPERTY()
    TObjectPtr<UWorldData> Data;

    /** Implements the numerically guarded solar-angle calculation. */
    static FRotator CalculateSunRotation(const UWorldData* World);
};
