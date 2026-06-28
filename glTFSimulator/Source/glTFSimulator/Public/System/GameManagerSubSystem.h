// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "GameManagerSubSystem.generated.h"

class UGameSettings;
class UWorldData;
class UPostProcessComponent;

UCLASS() class GLTFSIMULATOR_API UGameManagerSubSystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    UGameManagerSubSystem();

    virtual bool ShouldCreateSubsystem(UObject *Outer) const override;
    virtual void Initialize(FSubsystemCollectionBase &Collection) override;
    virtual void Deinitialize() override;

    static UGameManagerSubSystem *GetSubSystem(UWorld *InWorld);
    static UGameManagerSubSystem *GetSubSystem(AActor *InActor);
    UFUNCTION(BlueprintCallable)
    void SaveSettings();
    UFUNCTION(BlueprintCallable)
    void UpdateSettings();
    UFUNCTION(BlueprintCallable)
    void TogglePause();

    UFUNCTION(BlueprintCallable)
    void SetGamePaused(bool bPaused);

    UFUNCTION(BlueprintCallable)
    void SetWorldLoading(bool bLoading);

    UFUNCTION(BlueprintPure)
    bool IsWorldLoading() const { return bIsWorldLoading; }

    void SetPlayerActor(AActor *Actor) { PlayerActor = Actor; }
    void SetCameraComponent(USceneComponent *InCamera) { CurrentCamera = InCamera; }
    void SetGameSettings(UGameSettings *Settings) { GameSettings = Settings; }
    void SetWorldData(UWorldData *Data) { CurrentWorldData = Data; }
    UFUNCTION(BlueprintCallable)
    void SetCurrentWorldName(FString Name) { CurrentWorldName = Name; }
    void SetPlayerLocation(FVector Location) { PlayerLocation = Location; }
    void SetPostProcess(UPostProcessComponent *InPostProcess) { PostProcess = InPostProcess; }
    template <typename T>
    T* GetPlayerActor() const { return Cast<T>(PlayerActor); }
    template <typename T>
    T *GetCameraComponent() const { return Cast<T>(CurrentCamera); }

    UFUNCTION(BlueprintPure, Category="SettingData")
    UGameSettings *GetGameSettings() const { return GameSettings; }

    FVector GetPlayerLocation() const { return PlayerLocation; }
    FVector GetCameraLocation() const { return IsValid(CurrentCamera) ? CurrentCamera->GetComponentLocation() : FVector::ZeroVector; }
    bool GetGamePaused() const { return bIsGamePaused; }
    UWorldData *GetWorldData() const { return CurrentWorldData; }
    FString GetCurrentWorldName() const { return CurrentWorldName; }
    static void ToggleFullscreen();
    void SetLoadingStatus(float InValue) { LoadingStatus = (int32)(InValue * 100); }

    UFUNCTION(BlueprintCallable)
    float GetLoadingStatus() const { return (float)(LoadingStatus / 100.0f); }

    UFUNCTION(BlueprintPure)
    FString GetDebugText();

private:
    UPROPERTY()
    TObjectPtr<AActor> PlayerActor;
    UPROPERTY()
    TObjectPtr<USceneComponent> CurrentCamera;
    UPROPERTY()
    TObjectPtr<UGameSettings> GameSettings;
    UPROPERTY()
    TObjectPtr<UPostProcessComponent> PostProcess;
    UPROPERTY()
    TObjectPtr<UWorldData> CurrentWorldData;
    bool bIsGamePaused = false;
    bool bIsWorldLoading = false;
    FString CurrentWorldName;
    FVector PlayerLocation;
    int32 LoadingStatus = 0;
    int32 TotalSumFPS;
    int32 TotalCountFPS;
    FString GetHardwareInfoText(FString InString);
    FString GetFramerateInfoText(FString InString);
};