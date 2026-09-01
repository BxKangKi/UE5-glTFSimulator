// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TimerManager.h"
#include "WeatherSubsystem.generated.h"

class AActor;
class USceneComponent;
class UWorldData;

/**
 * Runtime owner for weather state and the editor-assigned weather effect actor.
 *
 * Design goals:
 * - level.json owns deterministic weather tick/range settings;
 * - the subsystem owns no permanent world actor reference across level travel;
 * - clear weather destroys the spawned effect actor, releasing Niagara/material resources;
 * - all UObject work is game-thread only;
 * - console and future chat commands use the same ApplyWeather entry point.
 */
UCLASS()
class GLTFSIMULATOR_API UWeatherSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Deinitialize() override;

    void ConfigureWeatherActorClass(TSubclassOf<AActor> InWeatherActorClass);
    void ConfigureFromWorldData(UWorldData* InWorldData);
    void SetWeatherCamera(USceneComponent* InCamera);

    /** Applies a state immediately. DurationSeconds < 0 uses level tick-duration rules. */
    bool ApplyWeather(const FString& Preset, float Intensity, bool bEnabled, float DurationSeconds = -1.0f);

    /** Explicitly releases the effect actor and timer. Safe to call repeatedly during world teardown. */
    void StopWeather();

    UFUNCTION(BlueprintPure, Category="Weather")
    FString GetCurrentWeatherPreset() const { return CurrentPreset; }

    UFUNCTION(BlueprintPure, Category="Weather")
    bool IsWeatherActive() const { return bWeatherSystemEnabled; }

private:
    UPROPERTY(Transient)
    TSubclassOf<AActor> WeatherActorClass;

    UPROPERTY(Transient)
    TObjectPtr<UWorldData> WorldData;

    UPROPERTY(Transient)
    TWeakObjectPtr<USceneComponent> WeatherCamera;

    UPROPERTY(Transient)
    TWeakObjectPtr<AActor> ActiveWeatherActor;

    FTimerHandle WeatherTickHandle;
    FString CurrentPreset = TEXT("clear");
    float CurrentIntensity = 1.0f;
    int32 RemainingWeatherTicks = 0;
    bool bWeatherSystemEnabled = false;
    bool bCommandOverrideActive = false;

    void RestartTickTimer();
    void WeatherTick();
    void ChooseNextAutomaticWeather();
    void ApplyEffectActorState();
    void DestroyEffectActor();
    int32 ResolveRandomDurationTicks() const;
    float GetTickIntervalSeconds() const;
    static FString NormalizePreset(const FString& Preset);
};
