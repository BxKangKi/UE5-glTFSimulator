// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Model/ModelData.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/SubclassOf.h"
#include "TimerManager.h"
#include "glTFStreamSubSystem.generated.h"

class AActor;
class AglTFStreamActor;
class UAssetManageSubSystem;
class ACharacterController;

/** File identity captured when a character load fails, so a replaced file can be retried. */
struct FFailedPlayerFileState
{
    int64 FileSize = INDEX_NONE;
    FDateTime Timestamp;
};

UCLASS()
class GLTFSIMULATOR_API UglTFStreamSubSystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    static UglTFStreamSubSystem* Get(UObject* WorldContextObject);

    virtual void Deinitialize() override;

    void StartMainWorldStreaming(AActor* InOwnerActor, TSubclassOf<AglTFStreamActor> InSpawnActorClass, const FString& InModelDirectory, const FString& InPlayerDirectory, const FString& InInitialPlayerName, bool bInRenderOnlyStreaming = false);
    void StopMainWorldStreaming();

    bool AreInitialModelsReady() const;
    bool IsInitialWorldReady();
    bool IsPlayerLoaded() const;
    float GetLoadingStatus() const;

    UFUNCTION(BlueprintCallable, Category="glTF Streaming|Player")
    bool CycleNextPlayerCharacter();

    void SetRenderOnlyStreaming(bool bInRenderOnlyStreaming) { bRenderOnlyStreaming = bInRenderOnlyStreaming; }
    bool IsRenderOnlyStreaming() const { return bRenderOnlyStreaming; }

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
    TSet<FString> ValidatedModelPaths;
    TMap<FString, FFailedPlayerFileState> FailedPlayerPaths;
    TSet<FString> MetadataUnavailablePaths;
    TMap<FString, FModelData> ModelMetadataMap;

    FString ModelDirectory;
    FString PlayerDirectory;
    FString InitialPlayerName;
    FString CurrentPlayerPath;
    FString PendingPlayerPath;
    int32 CurrentPathIndex = 0;
    int32 CurrentPlayerPathIndex = INDEX_NONE;
    int32 PendingPlayerPathIndex = INDEX_NONE;
    bool bActive = false;
    bool bInitialPathScanComplete = false;
    bool bInitialPlayerLoadComplete = false;
    bool bInitialPlayerLoadStarted = false;
    bool bWaitingForPlayerLoad = false;
    bool bPlayerActivated = false;
    bool bPendingPlayerIsInitialLoad = false;
    bool bRenderOnlyStreaming = false;
    FString WaitingPath;
    /** The single gameplay pawn. Runtime character meshes are swapped on demand on this pawn. */
    TWeakObjectPtr<ACharacterController> ActivePlayerCharacter;
    double PlayerActorWaitStartedAt = 0.0;
    double PlayerLoadStartedAt = 0.0;
    double ModelWaitStartedAt = 0.0;
    double NextModelFileAuditTime = 0.0;

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
    /** Starts one on-demand character GLB load on the existing pawn. Game-thread only. */
    void RequestLoadPlayerAtIndex(int32 PlayerPathIndex, bool bIsInitialLoad);
    bool ResolveInitialPlayerIndex();
    int32 FindNextLoadablePlayerIndex(int32 StartIndex) const;
    bool IsPlayerPathQuarantined(const FString& PlayerPath) const;
    void QuarantinePlayerPath(const FString& PlayerPath);
    void HandlePlayerLoadFailure(const FString& Reason);
    void CompletePlayerStreamingWithExistingCharacter(const FString& Reason);
    ACharacterController* GetPlayerCharacter() const;
    void DeactivatePlayerCharacter();
    void ActivatePlayerIfWorldReady();
    void DiscoverPlayerPaths();
    void PersistCurrentPlayerSelection();

    bool TryLoadValidModelMetadata(const FString& GlbPath, FModelData& OutModelData, bool& bOutJsonExists) const;
    bool IsValidModelMetadata(const FModelData& ModelData) const;
    bool IsPlayerInsideModelRange(const FModelData& ModelData, float RadiusMultiplier = 1.0f) const;
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
