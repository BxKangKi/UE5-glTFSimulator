// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldSceneStreamingSubsystem.h
 * 역할: 거리 기반으로 씬과 플레이어 모델을 스트리밍합니다.
 * 핵심 기능: 씬 액터 클래스 선택·생성, 초기 준비 상태, 플레이어 모델 교체.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "Model/ModelData.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TimerManager.h"
#include "WorldSceneStreamingSubsystem.generated.h"

class AActor;
class ACharacterController;
class AStaticActor;

/**
 * A scene row copied from the small .gwd root directory.
 *
 * This is intentionally metadata-only: keeping these rows resident is cheap, while the large
 * decoded .dat ranges stay on disk until EnsureSceneActor decides that the player is close enough.
 */
struct FWorldSceneStreamRecord
{
    FGuid UUID;
    FString RuntimeReference;
    FModelData Bounds;
};

/** Immutable character selector. The runtime reference is a gwd:// UUID, never a file path. */
struct FWorldCharacterStreamRecord
{
    FGuid UUID;
    FString RuntimeReference;
    FString Name;
    FString DisplayName;
};

/**
 * Distance streamer for static scene models and the selected player character.
 *
 * All discovery comes from UModelDatabaseSubsystem's already-open .gwd directory. This class
 * never scans resources/, never opens a source GLB, and never maintains a second metadata cache.
 * A scene actor is created only inside its coarse archive bounds; AStaticActor then range-reads that
 * model's detailed metadata and requested decoded .dat members through the shared archive reader.
 */
UCLASS()
class GLTFSIMULATOR_API UWorldSceneStreamingSubsystem final : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    static UWorldSceneStreamingSubsystem* Get(UObject* WorldContextObject);

    virtual void Deinitialize() override;

    void StartWorldStreaming(
        AActor* InOwnerActor,
        const FString& InWorldRoot,
        const FString& InInitialPlayerName,
        bool bInRenderOnlyStreaming = false);
    void StopWorldStreaming();

    bool AreInitialModelsReady() const;
    bool IsInitialWorldReady();
    bool IsPlayerLoaded() const;
    float GetLoadingStatus() const;

    UFUNCTION(BlueprintCallable, Category="World|Streaming|Player")
    bool CycleNextPlayerCharacter();

    /** Mutates game-thread-owned streaming state. Worker-thread calls are rejected. */
    void SetRenderOnlyStreaming(bool bInRenderOnlyStreaming);
    bool IsRenderOnlyStreaming() const;

    /** True only while this persistent GameInstance subsystem belongs to World. */
    bool IsActiveForWorld(const UWorld* World) const;

    /**
     * Reports a terminal startup rejection without exposing mutable streamer state.
     * GameManager uses this to stop its next-tick loading poll instead of waiting forever.
     */
    bool HasStartupFailed() const;

private:
    UPROPERTY()
    TObjectPtr<AActor> OwnerActor;

    /** Strong references exist only for scenes currently inside the streaming radius. */
    UPROPERTY()
    TMap<FString, TObjectPtr<AStaticActor>> ActiveSceneActors;

    TArray<FWorldSceneStreamRecord> SceneRecords;
    TArray<FWorldCharacterStreamRecord> CharacterRecords;
    TSet<FString> FailedCharacterReferences;

    FString WorldRoot;
    FString InitialPlayerName;
    FString CurrentCharacterReference;
    FString PendingCharacterReference;
    int32 CurrentCharacterIndex = INDEX_NONE;
    int32 PendingCharacterIndex = INDEX_NONE;

    bool bActive = false;
    bool bStartupFailed = false;
    bool bInitialScenePassComplete = false;
    bool bInitialPlayerLoadStarted = false;
    bool bInitialPlayerLoadComplete = false;
    bool bWaitingForPlayerLoad = false;
    bool bPendingPlayerIsInitialLoad = false;
    bool bPlayerActivated = false;
    bool bRenderOnlyStreaming = false;

    TWeakObjectPtr<ACharacterController> ActivePlayerCharacter;
    double PlayerActorWaitStartedAt = 0.0;
    double PlayerLoadStartedAt = 0.0;

    /** UI-only monotonic smoothing; it never delays actual archive reads or readiness. */
    mutable float LastReportedLoadingStatus = 0.0f;
    mutable uint64 LastLoadingProgressFrame = ~uint64(0);

    FTimerHandle TimerHandle_UpdateStreaming;
    FTimerHandle TimerHandle_WaitPlayer;

    void UpdateStreaming();
    void ScheduleStreamingUpdates();
    bool IsPlayerInsideSceneRange(const FModelData& Bounds, float RadiusMultiplier) const;
    FVector GetPlayerLocation() const;
    AStaticActor* EnsureSceneActor(const FWorldSceneStreamRecord& Record);
    void DestroySceneActor(const FString& RuntimeReference);

    void BeginInitialPlayerStreamingIfNeeded();
    void WaitForPlayerActor();
    void WaitForPlayerLoad();
    void ScheduleWaitForPlayerActor();
    void ScheduleWaitForPlayerLoad();
    bool ResolveInitialCharacterIndex();
    int32 FindNextLoadableCharacterIndex(int32 StartIndex) const;
    void RequestCharacterAtIndex(int32 CharacterIndex, bool bIsInitialLoad);
    void HandlePlayerLoadFailure(const FString& Reason);
    void CompletePlayerStreamingWithExistingCharacter(const FString& Reason);
    void ActivatePlayerIfWorldReady();
    void PersistCurrentPlayerSelection();
    ACharacterController* GetPlayerCharacter() const;
    void DeactivatePlayerCharacter();

    void ClearTimers();
    void WriteLogAsync(const FString& Message) const;
};
