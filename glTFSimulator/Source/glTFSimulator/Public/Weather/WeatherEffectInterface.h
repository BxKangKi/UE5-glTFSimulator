// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "WeatherEffectInterface.generated.h"

class USceneComponent;

/**
 * Optional Blueprint hook for editor-assigned weather actors (for example BP_Rain).
 *
 * The runtime weather subsystem does not require this interface: an actor that does not implement
 * it is still spawned, attached to the active camera, and destroyed when weather clears. Implement
 * the interface only when the Blueprint needs explicit preset/intensity/activation callbacks.
 */
UINTERFACE(BlueprintType)
class GLTFSIMULATOR_API UWeatherEffectInterface : public UInterface
{
    GENERATED_BODY()
};

class GLTFSIMULATOR_API IWeatherEffectInterface
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Weather")
    void SetWeatherCamera(USceneComponent* CameraComponent);

    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Weather")
    void SetWeatherPreset(const FString& Preset);

    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Weather")
    void SetWeatherIntensity(float Intensity);

    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Weather")
    void SetWeatherActive(bool bActive);
};
