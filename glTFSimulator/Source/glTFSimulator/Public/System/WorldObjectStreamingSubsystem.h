// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// 512 m world-object voxel streaming backed by one random-access .dat file.

/**
 * @file WorldObjectStreamingSubsystem.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "System/EntityArchive.h"
#include "WorldObjectStreamingSubsystem.generated.h"

/**
 * Owns Worlds/Data/WorldName.dat. Disk work uses immutable snapshots on tracked workers; actor
 * creation/destruction and maps stay game-thread-only. Periodic writes are coalesced to one-second
 * checkpoints; unload/stop still flush immediately. A change made during a commit advances its
 * revision so a newer snapshot follows it.
 */
UCLASS()
class GLTFSIMULATOR_API UWorldObjectStreamingSubsystem final : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    static constexpr double ChunkSizeCentimeters = 51200.0;

    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool IsTickable() const override { return bRunning && !HasAnyFlags(RF_ClassDefaultObject); }

    void Start(const FString& InWorldRoot, float InLoadRadiusMeters = 2048.0f);
    /** Applies a new settings-derived radius without reopening the .dat archive. */
    void SetLoadRadiusMeters(float InLoadRadiusMeters);
    void Stop();
    bool IsRunning() const { return bRunning; }
    bool IsInitialAreaReady() const
    {
        // A failed range remains non-ready while its throttled retry loop is active; reporting ready
        // here could let shutdown save over state that was never successfully restored.
        return bRunning && ActiveLoads == 0 && PendingLoads.IsEmpty()
            && LoadingChunks.IsEmpty() && FailedLoadRetryAt.IsEmpty();
    }
    bool IsLocationLoaded(const FVector& WorldLocation) const;
    /** Boundary crossing prefetch. The transient chunk is saved and released after the object moves. */
    void EnsureLocationLoaded(const FVector& WorldLocation);
    FWorldChunkCoordinate ToChunk(const FVector& WorldLocation) const;

    /** Registers a newly placed entity in the sole entity chunk store. */
    bool RegisterPlacedObject(AActor* Actor, const FGuid& ModelUUID);
    void UnregisterObject(AActor* Actor, bool bKeepPersistentRecord);
    void MarkObjectChanged(AActor* Actor);

    /** Loads the non-spatial dynamic state from the same .dat commit log. */
    void LoadRuntimeStateAsync(
        TFunction<void(bool, bool, FWorldRuntimeState, FString)> Callback);

    /** Coalesced player/time state save; completion is always delivered on the game thread. */
    void SaveRuntimeStateAsync(
        const FWorldRuntimeState& State,
        FSafeFileIO::FWriteCallback Callback = FSafeFileIO::FWriteCallback());

private:
    struct FRuntimeChunk
    {
        TArray<FWorldChunkObject> Objects;
        TArray<TWeakObjectPtr<AActor>> Actors;
        uint64 Revision = 0;
        uint64 SavingRevision = 0;
        double NextPeriodicSaveAt = 0.0;
        bool bDirty = false;
        bool bSaving = false;
        bool bUnloadAfterSave = false;
        bool bTransientBoundaryLoad = false;
    };

    struct FPendingLoad
    {
        FWorldChunkCoordinate Coordinate;
        bool bTransientBoundaryLoad = false;
    };

    struct FPendingRegistration
    {
        TWeakObjectPtr<AActor> Actor;
        FGuid EntityUUID;
        FGuid ModelUUID;
        FWorldChunkCoordinate Coordinate;
    };

    FString WorldRoot;
    TSharedPtr<FEntityArchiveStore, ESPMode::ThreadSafe> Archive;
    float LoadRadiusCentimeters = 204800.0f;
    uint64 Generation = 0;
    bool bRunning = false;
    float DesiredRefreshAccumulator = 0.0f;
    int32 ActiveLoads = 0;
    static constexpr int32 MaxConcurrentLoads = 8;
    static constexpr double PeriodicSaveIntervalSeconds = 1.0;
    static constexpr double FailedLoadRetryDelaySeconds = 1.0;
    static constexpr float DesiredRefreshIntervalSeconds = 0.25f;

    TMap<FWorldChunkCoordinate, FRuntimeChunk> LoadedChunks;
    TSet<FWorldChunkCoordinate> LoadingChunks;
    TSet<FWorldChunkCoordinate> DesiredChunks;
    TArray<FPendingLoad> PendingLoads;
    /** Corrupt/transient reads are throttled instead of being re-enqueued every frame. */
    TMap<FWorldChunkCoordinate, double> FailedLoadRetryAt;
    /** Objects accepted while their destination chunk is still loading. */
    TArray<FPendingRegistration> PendingRegistrations;

    void RebuildDesiredChunks();
    void QueueLoad(const FWorldChunkCoordinate& Coordinate, bool bTransientBoundaryLoad);
    void PumpLoads();
    /** Installs one validated payload (including a known-empty cell) on the game thread. */
    void InstallLoadedChunk(const FWorldChunkCoordinate& Coordinate, bool bTransientBoundaryLoad,
        TArray<FWorldChunkObject>&& Entities);
    void FinishLoad(const FWorldChunkCoordinate& Coordinate, bool bTransientBoundaryLoad,
        bool bSuccess, TArray<FWorldChunkObject>&& Entities, FString&& Error, uint64 RequestGeneration);
    AActor* SpawnObject(const FWorldChunkObject& Object);
    void UpdateObjectsAndCrossings();
    /** Publishes all eligible coordinates beneath one .dat commit footer. */
    void BeginSaveBatch(const TArray<FWorldChunkCoordinate>& Coordinates);
    void CompleteSave(const FWorldChunkCoordinate& Coordinate, uint64 SavedRevision,
        uint64 SavedGeneration, const FSafeFileWriteResult& Result);
    void RequestUnload(const FWorldChunkCoordinate& Coordinate);
    void FinalizeUnload(const FWorldChunkCoordinate& Coordinate);
    FWorldChunkObject SnapshotActor(
        AActor* Actor,
        const FGuid& EntityUUID,
        const FGuid& ModelUUID) const;
    bool IsPersistableEntity(const FGuid& UUID) const;
    bool HasPersistenceAuthority() const;
};
