// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WeatherActor.generated.h"

class UGameManagerSubSystem;
class USceneCaptureComponent2D;
class UNiagaraComponent;

UCLASS()
class GLTFSIMULATOR_API AWeatherActor : public AActor
{
    GENERATED_BODY()
public:
    AWeatherActor();

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<USceneCaptureComponent2D> SceneCapture;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UNiagaraComponent> Niagara;

private:

    FVector Location;
    FVector LocationOffset;
    void AsyncTick();

    // 비동기 작업 완료 시 호출될 콜백 함수
    UFUNCTION()
    void OnRainUpdateCompleted();

    TObjectPtr<UGameManagerSubSystem> SubSystem;
    double MaxDistance;
    FName Param;
};