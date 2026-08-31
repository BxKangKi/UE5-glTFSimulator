#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "WeatherRuntimeSubsystem.generated.h"

class AWeatherRuntimeActor;
class USceneComponent;

UCLASS()
class SHADERLIBRARY_API UWeatherRuntimeSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintCallable, Category="Weather")
    void SetWeatherCamera(USceneComponent* InCamera);

    UFUNCTION(BlueprintCallable, Category="Weather")
    void ApplyWeather(const FString& InPreset, float InIntensity, bool bEnabled);

    UFUNCTION(BlueprintCallable, Category="Weather")
    void ClearWeather();

    UFUNCTION(BlueprintPure, Category="Weather")
    AWeatherRuntimeActor* GetActiveWeatherActor() const { return ActiveWeatherActor; }

private:
    void EnsureActorForCurrentWorld();
    void DestroyActor();
    void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);

    UPROPERTY(Transient)
    TObjectPtr<AWeatherRuntimeActor> ActiveWeatherActor;

    TWeakObjectPtr<USceneComponent> WeatherCamera;
    FString PendingPreset = TEXT("Rain");
    float PendingIntensity = 0.0f;
    bool bPendingEnabled = false;
    TWeakObjectPtr<UWorld> ActiveWorld;
    FDelegateHandle WorldCleanupHandle;
};
