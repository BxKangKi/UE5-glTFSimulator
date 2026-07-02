// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Model/Data.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "glTFStreamSubSystem.generated.h"

class AActor;
class AglTFStreamActor;
class UAssetManageSubSystem;
class ACharacterController;
class APlayerController;

UCLASS()
class GLTFSIMULATOR_API UglTFStreamSubSystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    static UglTFStreamSubSystem* Get(UObject* WorldContextObject);

    virtual void Deinitialize() override;

    void StartMainWorldStreaming(AActor* InOwnerActor, TSubclassOf<AglTFStreamActor> InSpawnActorClass, const FString& InModelDirectory, const FString& InPlayerDirectory, const FString& InInitialPlayerName);
    void StopMainWorldStreaming();

    bool AreInitialModelsReady() const;
    bool IsInitialWorldReady();
    bool IsPlayerLoaded() const;
    float GetLoadingStatus() const;

    UFUNCTION(BlueprintCallable, Category="glTF Streaming|Player")
    bool CycleNextPlayerCharacter();

private:
    UPROPERTY()
    TObjectPtr<AActor> OwnerActor;

    UPROPERTY()
    TSubclassOf<AglTFStreamActor> SpawnActorClass;

    UPROPERTY()
    TMap<FString, TObjectPtr<AglTFStreamActor>> SpawnActorMap;

    TArray<FString> GlbFilePaths;
    TArray<FString> PlayerGlbFilePaths;
    TSet<FString> CompletedInitialPaths;
    TSet<FString> MissingFilePaths;
    TMap<FString, FModelData> ModelMetadataMap;

    FString ModelDirectory;
    FString PlayerDirectory;
    FString InitialPlayerName;
    FString CurrentPlayerPath;
    int32 CurrentPathIndex = 0;
    int32 CurrentPlayerPathIndex = INDEX_NONE;
    bool bActive = false;
    bool bInitialPathScanComplete = false;
    bool bInitialPlayerLoadComplete = false;
    bool bInitialPlayerLoadStarted = false;
    bool bWaitingForPlayerLoad = false;
    bool bPlayerActivated = false;
    FString WaitingPath;
    TWeakObjectPtr<ACharacterController> ActivePlayerCharacter;
    TWeakObjectPtr<ACharacterController> PendingPlayerCharacter;
    TWeakObjectPtr<ACharacterController> PreviousPlayerCharacter;

    FTimerHandle TimerHandle_ProcessPath;
    FTimerHandle TimerHandle_WaitActor;
    FTimerHandle TimerHandle_UpdateStreaming;
    FTimerHandle TimerHandle_WaitPlayer;

    void ProcessNextPathAsync();
    void WaitForCurrentActorAsync();
    void UpdateStreamingAsync();
    void BeginInitialPlayerStreamingIfNeeded();
    void StartPlayerStreaming();
    void WaitForPlayerActorAsync();
    void WaitForPlayerLoadAsync();
    void RequestLoadPlayerAtIndex(int32 PlayerPathIndex, bool bIsInitialLoad);
    ACharacterController* SpawnReplacementPlayerCharacterForLoad(const FString& PlayerPath);
    bool CommitPendingPlayerCharacter();
    void DestroyPreviousPlayerCharacter();
    bool ResolveInitialPlayerIndex();
    ACharacterController* GetPlayerCharacter() const;
    void DeactivatePlayerCharacter();
    void ActivatePlayerIfWorldReady();
    void DiscoverPlayerPaths();
    void PersistCurrentPlayerSelection();

    bool TryLoadValidModelMetadata(const FString& GlbPath, FModelData& OutModelData, bool& bOutJsonExists) const;
    bool IsValidModelMetadata(const FModelData& ModelData) const;
    bool IsPlayerInsideModelRange(const FModelData& ModelData) const;
    FVector GetPlayerLocation() const;

    AglTFStreamActor* EnsureSpawnActor(const FString& GlbPath);
    void DestroySpawnActor(const FString& GlbPath);
    void CacheActorMetadata(const FString& GlbPath, const AglTFStreamActor* Actor);

    void ScheduleProcessNextPath();
    void ScheduleUpdateStreaming();
    void ScheduleWaitForPlayerActor();
    void ScheduleWaitForPlayerLoad();
    void ClearTimers();
    void WriteLogAsync(const FString& Message) const;
};
