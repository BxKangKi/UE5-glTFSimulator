// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WeatherActor.generated.h"

class UGameManagerSubSystem;
class USceneCaptureComponent2D;
class UNiagaraComponent;
class UGameUpdateSubSystem;

UCLASS()
class GLTFSIMULATOR_API AWeatherActor : public AActor
{
    GENERATED_BODY()
public:
    AWeatherActor();

    UFUNCTION(BlueprintCallable, Category="Weather")
    void ConfigureWeather(const FString& InPreset, float InIntensity);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weather")
    FString WeatherPreset = TEXT("Rain");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weather", meta=(ClampMin="0.0"))
    float WeatherIntensity = 1.0f;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<USceneCaptureComponent2D> SceneCapture;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UNiagaraComponent> Niagara;

private:

    FVector Location;
    FVector LocationOffset;
    int32 GameUpdateTickHandle = INDEX_NONE;
    bool bRainUpdateInFlight = false;
    void UpdateFromGameUpdate(float DeltaSeconds);
    void StartRainAsyncUpdate();

    // Callback invoked when the asynchronous task completes.
    UFUNCTION()
    void OnRainUpdateCompleted();

    TObjectPtr<UGameManagerSubSystem> SubSystem;
    double MaxDistance;
    FName Param;
};