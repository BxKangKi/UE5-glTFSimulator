// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WeatherSubsystem.h
 * 역할: 월드 날씨 상태와 날씨 효과 액터를 관리합니다.
 * 핵심 기능: 날씨 시계·전환, 비·눈 액터 생성·제거, 카메라 연동.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
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
 * - config.json owns deterministic weather tick/range settings;
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
    FTSTicker::FDelegateHandle CameraFollowTickerHandle;
    FString CurrentPreset = TEXT("clear");
    float CurrentIntensity = 1.0f;
    int32 RemainingWeatherTicks = 0;
    bool bWeatherSystemEnabled = false;
    bool bCommandOverrideActive = false;

    void RestartTickTimer();
    void WeatherTick();
    void ChooseNextAutomaticWeather();
    void ApplyEffectActorState();
    void StartCameraFollowTicker();
    void StopCameraFollowTicker();
    bool TickWeatherActorFollow(float DeltaTime);
    void DestroyEffectActor();
    int32 ResolveRandomDurationTicks() const;
    float GetTickIntervalSeconds() const;
    static FString NormalizePreset(const FString& Preset);
};
