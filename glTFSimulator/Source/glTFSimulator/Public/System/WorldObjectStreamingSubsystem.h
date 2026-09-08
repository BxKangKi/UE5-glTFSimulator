// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// 512 m world-object voxel streaming and coalesced persistence.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "System/BinaryDataStore.h"
#include "WorldObjectStreamingSubsystem.generated.h"

/**
 * Owns data/chunks/chunk.x.y.z.dat and data/entities/entity.x.y.z.dat. Disk work is immutable-snapshot async work; actor creation/destruction and
 * maps are game-thread-only. A dirty chunk is written at most once per tick, and a change made while
 * a write is active advances its revision so the newer snapshot is written afterward.
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
    void Stop();
    bool IsRunning() const { return bRunning; }
    bool IsInitialAreaReady() const
    {
        return bRunning && ActiveLoads == 0 && PendingLoads.IsEmpty() && LoadingChunks.IsEmpty();
    }
    bool IsLocationLoaded(const FVector& WorldLocation) const;
    /** Boundary crossing prefetch. The transient chunk is saved and released after the object moves. */
    void EnsureLocationLoaded(const FVector& WorldLocation);
    FWorldChunkCoordinate ToChunk(const FVector& WorldLocation) const;

    /** Registers a newly placed object so static prefabs and dynamic entities share chunk storage. */
    bool RegisterPlacedObject(AActor* Actor, const FGuid& ModelUUID);
    void UnregisterObject(AActor* Actor, bool bKeepPersistentRecord);
    void MarkObjectChanged(AActor* Actor);

private:
    struct FRuntimeChunk
    {
        TArray<FWorldChunkObject> Objects;
        TArray<TWeakObjectPtr<AActor>> Actors;
        uint64 Revision = 0;
        uint64 SavingRevision = 0;
        bool bDirty = false;
        bool bSaving = false;
        bool bUnloadAfterSave = false;
        bool bTransientBoundaryLoad = false;
        int32 PendingSaveParts = 0;
        bool bSaveBatchFailed = false;
    };

    struct FPendingLoad
    {
        FWorldChunkCoordinate Coordinate;
        bool bTransientBoundaryLoad = false;
    };

    struct FPendingRegistration
    {
        TWeakObjectPtr<AActor> Actor;
        FGuid UUID;
        FWorldChunkCoordinate Coordinate;
        EWorldObjectStorageKind StorageKind = EWorldObjectStorageKind::Prefab;
    };

    FString WorldRoot;
    FString PrefabDataRoot;
    FString EntityDataRoot;
    float LoadRadiusCentimeters = 204800.0f;
    uint64 Generation = 0;
    bool bRunning = false;
    float DesiredRefreshAccumulator = 0.0f;
    int32 ActiveLoads = 0;
    static constexpr int32 MaxConcurrentLoads = 8;

    TMap<FWorldChunkCoordinate, FRuntimeChunk> LoadedChunks;
    TSet<FWorldChunkCoordinate> LoadingChunks;
    TSet<FWorldChunkCoordinate> DesiredChunks;
    TArray<FPendingLoad> PendingLoads;
    /** Objects accepted while their destination chunk is still loading. */
    TArray<FPendingRegistration> PendingRegistrations;

    FString PrefabChunkPath(const FWorldChunkCoordinate& Coordinate) const;
    FString EntityChunkPath(const FWorldChunkCoordinate& Coordinate) const;
    void RebuildDesiredChunks();
    void QueueLoad(const FWorldChunkCoordinate& Coordinate, bool bTransientBoundaryLoad);
    void PumpLoads();
    void FinishLoad(const FWorldChunkCoordinate& Coordinate, bool bTransientBoundaryLoad,
        bool bSuccess, TArray<FWorldChunkObject>&& Prefabs, TArray<FWorldChunkObject>&& Entities,
        FString&& Error, uint64 RequestGeneration);
    AActor* SpawnObject(const FWorldChunkObject& Object);
    void UpdateObjectsAndCrossings();
    void BeginSave(const FWorldChunkCoordinate& Coordinate, FRuntimeChunk& Chunk);
    void CompleteSavePart(const FWorldChunkCoordinate& Coordinate, uint64 SavedRevision,
        uint64 SavedGeneration, const FSafeFileWriteResult& Result);
    void RequestUnload(const FWorldChunkCoordinate& Coordinate);
    void FinalizeUnload(const FWorldChunkCoordinate& Coordinate);
    FWorldChunkObject SnapshotActor(AActor* Actor, const FGuid& UUID, EWorldObjectStorageKind StorageKind) const;
    EWorldObjectStorageKind ResolveStorageKind(const FGuid& UUID, bool& bOutValid) const;
    bool ImportPlacementFilesBlocking();
    bool HasPersistenceAuthority() const;
};
