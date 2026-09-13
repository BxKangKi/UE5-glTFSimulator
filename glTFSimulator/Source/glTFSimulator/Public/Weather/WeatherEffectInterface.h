// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WeatherEffectInterface.h
 * 역할: 에디터에서 지정한 날씨 액터의 콜백 인터페이스입니다.
 * 핵심 기능: 날씨 preset·강도 알림.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

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
