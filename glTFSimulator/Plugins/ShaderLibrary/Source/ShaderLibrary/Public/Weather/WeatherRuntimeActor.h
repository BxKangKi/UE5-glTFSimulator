#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WeatherRuntimeActor.generated.h"

class UMaterialInstanceDynamic;
class UNiagaraComponent;
class USceneCaptureComponent2D;
class UPostProcessComponent;
class USceneComponent;

UCLASS(BlueprintType)
class SHADERLIBRARY_API AWeatherRuntimeActor : public AActor
{
    GENERATED_BODY()

public:
    AWeatherRuntimeActor();

    UFUNCTION(BlueprintCallable, Category="Weather")
    void ConfigureWeather(const FString& InPreset, float InIntensity, USceneComponent* InCamera);

    UFUNCTION(BlueprintCallable, Category="Weather")
    void SetWeatherCamera(USceneComponent* InCamera);

    UFUNCTION(BlueprintPure, Category="Weather")
    float GetSurfaceCoverage() const { return SurfaceCoverage; }

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Tick(float DeltaSeconds) override;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Weather|Capture")
    TObjectPtr<USceneCaptureComponent2D> SceneCapture;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Weather|Niagara")
    TObjectPtr<UNiagaraComponent> Niagara;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Weather|PostProcess")
    TObjectPtr<UPostProcessComponent> PostProcess;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weather|Capture", meta=(ClampMin="100.0", Units="cm"))
    float CaptureOrthoWidth = 12100.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weather|Capture", meta=(ClampMin="0.0", Units="cm"))
    float CaptureMaxViewDistance = 1048576.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weather|Capture", meta=(Units="cm"))
    float CaptureHeightOffset = 1500.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weather|Surface", meta=(ClampMin="0.0"))
    float WetnessRiseRate = 0.42f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weather|Surface", meta=(ClampMin="0.0"))
    float WetnessDryRate = 0.06f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weather|Surface", meta=(ClampMin="0.0"))
    float SnowAccumulationRate = 0.16f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weather|Surface", meta=(ClampMin="0.0"))
    float SnowMeltRate = 0.018f;

private:
    void UpdateRainCaptureParameters();
    void UpdateWeatherPostProcess();
    TWeakObjectPtr<USceneComponent> WeatherCamera;
    TObjectPtr<UMaterialInstanceDynamic> WeatherPostProcessMID;

    FString WeatherPreset = TEXT("Rain");
    float WeatherIntensity = 0.0f;
    float SurfaceCoverage = 0.0f;
};
