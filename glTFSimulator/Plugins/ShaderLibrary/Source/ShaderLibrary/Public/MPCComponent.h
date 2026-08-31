// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MPCComponent.generated.h"

class UMaterialParameterCollection;

/**
 * Lightweight runtime controller for the weather Material Parameter Collection.
 *
 * WeatherMode is a continuous value:
 *   0.0 = Rain
 *   0.5 = Rain/Snow blend
 *   1.0 = Snow
 *
 * Precipitation is the intensity:
 *   0.0 = Clear (forces precipitation to zero in the MPC)
 *   1.0 = Full intensity
 */
UCLASS(ClassGroup=(Rendering), meta=(BlueprintSpawnableComponent))
class SHADERLIBRARY_API UMPCComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UMPCComponent();

    /** Material Parameter Collection used by the weather post process material. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Weather")
    TObjectPtr<UMaterialParameterCollection> MPC;

    /**
     * Continuous weather mode.
     * 0 = Rain, 1 = Snow, values between them are blended.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Weather", meta=(ClampMin="0.0", ClampMax="1.0"))
    float WeatherMode = 0.0f;

    /** Precipitation intensity. Zero means Clear regardless of WeatherMode. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Weather", meta=(ClampMin="0.0", ClampMax="1.0"))
    float Precipitation = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="CelShading", meta=(ClampMin="0.0", ClampMax="1.0"))
    float CelShadingMode = 0.0f;

    /** Apply the editor/default values when the component begins play. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Weather")
    bool bApplyDefaultsOnBeginPlay = true;

    /** Set weather values at runtime. */
    UFUNCTION(BlueprintCallable, Category="Weather")
    void SetWeather(float InWeatherMode, float InPrecipitation);

    /** Set a clear weather state. */
    UFUNCTION(BlueprintCallable, Category="CelShading")
    void SetWeatherClear();

    /** Apply current properties to the world's MPC instance. */
    UFUNCTION(BlueprintCallable, Category="CelShadingPrecipitationParameterName")
    bool ApplyWeather();

    UFUNCTION(BlueprintCallable, Category="Weather")
    void SetCelShadingMode(float InCelShadingMode);

    UFUNCTION(BlueprintCallable, Category="Weather")
    bool ApplyCelShadingMode();

protected:
    virtual void BeginPlay() override;

private:
    bool ApplyWeatherInternal();
    bool ApplyCelShadingModeInternal();
    UMaterialParameterCollectionInstance* GetMPC();
    static const FName WeatherModeParameterName;
    static const FName PrecipitationParameterName;
    static const FName CelShadingModeParameterName;
};
