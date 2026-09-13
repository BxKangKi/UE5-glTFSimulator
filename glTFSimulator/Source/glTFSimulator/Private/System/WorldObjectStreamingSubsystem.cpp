// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldObjectStreamingSubsystem.cpp
 * 역할: entity 청크의 객체 생성·제거·저장을 조정합니다.
 * 핵심 기능: 비동기 범위 읽기, 초기 복구, 변경 객체 추적, 저장 병합.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "System/WorldObjectStreamingSubsystem.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Model/DynamicActor.h"
#include "Model/StaticActor.h"
#include "System/GlTFSimulatorGameInstance.h"
#include "System/GlTFSimulatorAssetRegistry.h"
#include "Simulator/ModelDatabaseSubsystem.h"
#include "System/MultiplayerWorldSubSystem.h"
#include "System/GameManagerSubSystem.h"
#include "System/FileFunctionLibrary.h"
#include "System/SafeFileIO.h"
#include "System/StreamingMovementGateSubsystem.h"
#include "Vehicle/VehiclePawn.h"

TStatId UWorldObjectStreamingSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UWorldObjectStreamingSubsystem, STATGROUP_Tickables);
}

bool UWorldObjectStreamingSubsystem::HasPersistenceAuthority() const
{
    const UWorld* World = GetWorld();
    return World && World->GetNetMode() != NM_Client;
}

FWorldChunkCoordinate UWorldObjectStreamingSubsystem::ToChunk(const FVector& Location) const
{
    // UE 5.8 returns int64 from FloorToInt(double). Chunk coordinates intentionally remain int32
    // for compact keys and filenames, so clamp before the explicit narrowing conversion.
    const auto ToChunkAxis = [](const double AxisCentimeters)
    {
        const int64 Index = FMath::FloorToInt(
            AxisCentimeters / UWorldObjectStreamingSubsystem::ChunkSizeCentimeters);
        return static_cast<int32>(FMath::Clamp<int64>(
            Index,
            static_cast<int64>(MIN_int32),
            static_cast<int64>(MAX_int32)));
    };
    return {
        ToChunkAxis(Location.X),
        ToChunkAxis(Location.Y),
        ToChunkAxis(Location.Z)};
}

void UWorldObjectStreamingSubsystem::Start(const FString& InWorldRoot, const float InLoadRadiusMeters)
{
    check(IsInGameThread());
    Stop();
    if (InWorldRoot.TrimStartAndEnd().IsEmpty())
    {
        UE_LOG(LogTemp, Error,
            TEXT("World-object loading aborted because the explicit world root is empty; no relative data paths will be used."));
        return;
    }
    WorldRoot = FSafeFileIO::NormalizeFilePath(InWorldRoot);
    if (WorldRoot.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("World-object loading aborted because the explicit world root is invalid."));
        return;
    }
    // A network client receives authoritative dynamic actors through replication and must not
    // require or create the server's mutable .dat file. Keep the subsystem in a ready no-I/O
    // state so bootstrap can continue when the deployment contains only .gwd + config.json.
    if (!HasPersistenceAuthority())
    {
        const float RequestedRadiusMeters = FMath::IsFinite(InLoadRadiusMeters)
            ? InLoadRadiusMeters : 2048.0f;
        LoadRadiusCentimeters = FMath::Clamp(
            RequestedRadiusMeters * 100.0f, 51200.0f, 614400.0f);
        ++Generation;
        bRunning = true;
        DesiredRefreshAccumulator = 0.0f;
        return;
    }
    FString ArchiveError;
    Archive = FEntityArchiveStore::Open(WorldRoot, true, ArchiveError);
    if (!Archive.IsValid())
    {
        UE_LOG(LogTemp, Error,
            TEXT("World-object loading aborted because the .dat archive could not be opened. Root=%s Reason=%s"),
            *WorldRoot,
            *ArchiveError);
        WorldRoot.Reset();
        return;
    }
    // Bound configuration-derived fan-out. At 512 m chunks the 4 km cap is at most 17^3 keys;
    // allowing an unbounded/NaN radius could otherwise enqueue billions of empty range requests.
    const float RequestedRadiusMeters = FMath::IsFinite(InLoadRadiusMeters)
        ? InLoadRadiusMeters : 2048.0f;
    LoadRadiusCentimeters = FMath::Clamp(
        RequestedRadiusMeters * 100.0f, 51200.0f, 614400.0f);
    ++Generation;
    bRunning = true;
    // Rebuild once now, then wait a full interval instead of repeating the same radius scan on the
    // immediately following tick.
    DesiredRefreshAccumulator = DesiredRefreshIntervalSeconds;
    RebuildDesiredChunks();
    PumpLoads();
}

void UWorldObjectStreamingSubsystem::SetLoadRadiusMeters(const float InLoadRadiusMeters)
{
    check(IsInGameThread());
    const float RequestedRadiusMeters = FMath::IsFinite(InLoadRadiusMeters)
        ? InLoadRadiusMeters : 2048.0f;
    const float NewRadiusCentimeters = FMath::Clamp(
        RequestedRadiusMeters * 100.0f, 51200.0f, 614400.0f);
    if (FMath::IsNearlyEqual(NewRadiusCentimeters, LoadRadiusCentimeters, 1.0f))
    {
        return;
    }
    LoadRadiusCentimeters = NewRadiusCentimeters;
    if (bRunning)
    {
        DesiredRefreshAccumulator = DesiredRefreshIntervalSeconds;
        RebuildDesiredChunks();
        PumpLoads();
    }
}

void UWorldObjectStreamingSubsystem::Stop()
{
    check(IsInGameThread());
    ++Generation;
    bRunning = false;
    PendingLoads.Empty();
    LoadingChunks.Empty();
    DesiredChunks.Empty();
    FailedLoadRetryAt.Empty();
    ActiveLoads = 0;

    // Rebuild the changed portion of the spatial index from final actor transforms. Reserving one
    // write order per touched coordinate supersedes older in-flight snapshots; publishing the whole
    // set under one footer also keeps shutdown-time boundary crossings atomic.
    const TSharedPtr<FEntityArchiveStore, ESPMode::ThreadSafe> Store = Archive;
    const bool bCanPersist = Store.IsValid() && HasPersistenceAuthority();
    TSet<FWorldChunkCoordinate> LoadedCoordinates;
    TMap<FWorldChunkCoordinate, TArray<FWorldChunkObject>> FinalSnapshots;
    TSet<FWorldChunkCoordinate> ChangedCoordinates;
    TSet<FWorldChunkCoordinate> NeedsDiskMerge;
    for (const TPair<FWorldChunkCoordinate, FRuntimeChunk>& Pair : LoadedChunks)
    {
        LoadedCoordinates.Add(Pair.Key);
        FinalSnapshots.Add(Pair.Key);
    }

    const auto HasDynamicDifference = [](const FWorldChunkObject& A, const FWorldChunkObject& B)
    {
        return !A.Location.Equals(B.Location, 0.1)
            || !A.Rotation.Equals(B.Rotation, 0.0001)
            || !A.Scale.Equals(B.Scale, 0.0001)
            || !A.Velocity.Equals(B.Velocity, 0.1)
            || !A.AngularVelocity.Equals(B.AngularVelocity, 0.001);
    };
    const auto AddOrReplace = [](TArray<FWorldChunkObject>& Objects, const FWorldChunkObject& Object)
    {
        Objects.RemoveAllSwap(
            [&Object](const FWorldChunkObject& Existing)
            {
                return Existing.EntityUUID == Object.EntityUUID;
            },
            EAllowShrinking::No);
        Objects.Add(Object);
    };

    for (TPair<FWorldChunkCoordinate, FRuntimeChunk>& Pair : LoadedChunks)
    {
        const FWorldChunkCoordinate Source = Pair.Key;
        FRuntimeChunk& Chunk = Pair.Value;
        if (Chunk.bDirty || Chunk.bSaving || Chunk.Revision != Chunk.SavingRevision)
        {
            ChangedCoordinates.Add(Source);
        }
        for (int32 Index = 0; Index < Chunk.Objects.Num(); ++Index)
        {
            const FWorldChunkObject Previous = Chunk.Objects[Index];
            FWorldChunkObject Snapshot = Previous;
            if (Chunk.Actors.IsValidIndex(Index))
            {
                if (AActor* Actor = Chunk.Actors[Index].Get())
                {
                    Snapshot = SnapshotActor(Actor, Previous.EntityUUID, Previous.ModelUUID);
                }
            }
            const FWorldChunkCoordinate Destination = ToChunk(Snapshot.Location);
            if (HasDynamicDifference(Previous, Snapshot)) ChangedCoordinates.Add(Source);
            if (!(Destination == Source))
            {
                ChangedCoordinates.Add(Source);
                ChangedCoordinates.Add(Destination);
                if (!LoadedCoordinates.Contains(Destination)) NeedsDiskMerge.Add(Destination);
            }
            AddOrReplace(FinalSnapshots.FindOrAdd(Destination), Snapshot);
        }
    }

    // A placement can be accepted while its destination range is loading. Include it in the same
    // final transaction; unloaded destinations are range-read and merged by the worker first.
    for (const FPendingRegistration& Pending : PendingRegistrations)
    {
        if (AActor* Actor = Pending.Actor.Get())
        {
            const FWorldChunkObject Snapshot = SnapshotActor(
                Actor, Pending.EntityUUID, Pending.ModelUUID);
            const FWorldChunkCoordinate Destination = ToChunk(Snapshot.Location);
            AddOrReplace(FinalSnapshots.FindOrAdd(Destination), Snapshot);
            ChangedCoordinates.Add(Destination);
            if (!LoadedCoordinates.Contains(Destination)) NeedsDiskMerge.Add(Destination);
            Actor->Destroy();
        }
    }

    TMap<FWorldChunkCoordinate, TArray<FWorldChunkObject>> CommitSnapshots;
    TMap<FWorldChunkCoordinate, uint64> WriteOrders;
    if (bCanPersist && !ChangedCoordinates.IsEmpty())
    {
        for (const FWorldChunkCoordinate& Coordinate : ChangedCoordinates)
        {
            CommitSnapshots.Add(Coordinate, FinalSnapshots.FindRef(Coordinate));
            WriteOrders.Add(Coordinate, Store->ReserveChunkWrite(Coordinate));
        }
        const bool bQueued = FSafeFileIO::RunTrackedWorker(
            [Store, CommitSnapshots, WriteOrders, NeedsDiskMerge]() mutable
        {
            // Only destinations absent from memory require an on-demand disk merge. This prevents
            // a teleport or last-frame placement from overwriting entities already in that chunk.
            for (const FWorldChunkCoordinate& Coordinate : NeedsDiskMerge)
            {
                TArray<FWorldChunkObject> Existing;
                FString Error;
                if (!Store->LoadChunk(Coordinate, Existing, Error))
                {
                    UE_LOG(LogTemp, Error,
                        TEXT("Final .dat destination could not be merged; the prior generation remains active. Reason=%s"),
                        *Error);
                    return;
                }
                const TArray<FWorldChunkObject> Incoming = CommitSnapshots.FindRef(Coordinate);
                for (const FWorldChunkObject& Object : Incoming)
                {
                    Existing.RemoveAllSwap(
                        [&Object](const FWorldChunkObject& Item)
                        {
                            return Item.EntityUUID == Object.EntityUUID;
                        },
                        EAllowShrinking::No);
                    Existing.Add(Object);
                }
                CommitSnapshots.Add(Coordinate, MoveTemp(Existing));
            }
            const FSafeFileWriteResult Result = Store->SaveChunks(CommitSnapshots, WriteOrders);
            if (!Result.IsSuccess())
            {
                UE_LOG(LogTemp, Error,
                    TEXT("Final .dat batch was not committed. File=%s Reason=%s"),
                    *Result.Path, *Result.Error);
            }
        });
        if (!bQueued)
        {
            UE_LOG(LogTemp, Error,
                TEXT("Final .dat batch was not queued because the tracked worker pool is shutting down."));
        }
    }

    PendingRegistrations.Empty();
    for (TPair<FWorldChunkCoordinate, FRuntimeChunk>& Pair : LoadedChunks)
    {
        FRuntimeChunk& Chunk = Pair.Value;
        for (const TWeakObjectPtr<AActor>& Actor : Chunk.Actors)
            if (Actor.IsValid()) Actor->Destroy();
    }
    LoadedChunks.Empty();
    Archive.Reset();
    WorldRoot.Reset();
    DesiredRefreshAccumulator = 0.0f;
}

void UWorldObjectStreamingSubsystem::Deinitialize()
{
    Stop();
    Super::Deinitialize();
}

bool UWorldObjectStreamingSubsystem::IsLocationLoaded(const FVector& WorldLocation) const
{
    if (bRunning && !HasPersistenceAuthority()) return true;
    return LoadedChunks.Contains(ToChunk(WorldLocation));
}

void UWorldObjectStreamingSubsystem::EnsureLocationLoaded(const FVector& WorldLocation)
{
    check(IsInGameThread());
    if (!HasPersistenceAuthority()) return;
    const FWorldChunkCoordinate Coordinate = ToChunk(WorldLocation);
    if (!LoadedChunks.Contains(Coordinate) && !LoadingChunks.Contains(Coordinate))
    {
        QueueLoad(Coordinate, true);
        PumpLoads();
    }
}

void UWorldObjectStreamingSubsystem::RebuildDesiredChunks()
{
    TSet<FWorldChunkCoordinate> NewDesired;
    UWorld* World = GetWorld();
    if (!World) return;
    const int32 Radius = FMath::CeilToInt(LoadRadiusCentimeters / ChunkSizeCentimeters);
    const int32 Diameter = Radius * 2 + 1;
    // Reserve the one-observer cube up front. The spherical distance check will use less, and
    // additional players can grow normally without penalizing the dominant single-player path.
    NewDesired.Reserve(Diameter * Diameter * Diameter);
    const double RadiusSq = FMath::Square(static_cast<double>(LoadRadiusCentimeters + ChunkSizeCentimeters));
    for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
    {
        const APlayerController* Controller = It->Get();
        const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
        if (!IsValid(Pawn)) continue;
        const FVector Observer = Pawn->GetActorLocation();
        const FWorldChunkCoordinate Center = ToChunk(Observer);
        for (int32 DeltaZ = -Radius; DeltaZ <= Radius; ++DeltaZ)
            for (int32 DeltaY = -Radius; DeltaY <= Radius; ++DeltaY)
                for (int32 DeltaX = -Radius; DeltaX <= Radius; ++DeltaX)
                {
                    const int32 X = static_cast<int32>(FMath::Clamp<int64>(
                        static_cast<int64>(Center.X) + DeltaX, MIN_int32, MAX_int32));
                    const int32 Y = static_cast<int32>(FMath::Clamp<int64>(
                        static_cast<int64>(Center.Y) + DeltaY, MIN_int32, MAX_int32));
                    const int32 Z = static_cast<int32>(FMath::Clamp<int64>(
                        static_cast<int64>(Center.Z) + DeltaZ, MIN_int32, MAX_int32));
                    const FVector ChunkCenter((X + 0.5) * ChunkSizeCentimeters,
                        (Y + 0.5) * ChunkSizeCentimeters, (Z + 0.5) * ChunkSizeCentimeters);
                    if (FVector::DistSquared(Observer, ChunkCenter) <= RadiusSq)
                        NewDesired.Add({X, Y, Z});
                }
    }
    // Known-empty cells are installed synchronously by PumpLoads. Reserve all three containers so
    // the first 2-4 km radius does not repeatedly rehash on the game thread.
    LoadedChunks.Reserve(LoadedChunks.Num() + NewDesired.Num());
    LoadingChunks.Reserve(LoadingChunks.Num() + NewDesired.Num());
    PendingLoads.Reserve(PendingLoads.Num() + NewDesired.Num());
    for (const FWorldChunkCoordinate& Coordinate : NewDesired)
    {
        if (FRuntimeChunk* Existing = LoadedChunks.Find(Coordinate)) Existing->bTransientBoundaryLoad = false;
        else if (!LoadingChunks.Contains(Coordinate)) QueueLoad(Coordinate, false);
    }
    TArray<FWorldChunkCoordinate> NoLongerDesired;
    NoLongerDesired.Reserve(LoadedChunks.Num());
    for (const TPair<FWorldChunkCoordinate, FRuntimeChunk>& Pair : LoadedChunks)
        if (!NewDesired.Contains(Pair.Key) && !Pair.Value.bTransientBoundaryLoad) NoLongerDesired.Add(Pair.Key);

    // A caller can destroy a just-placed actor before its destination range finishes loading.
    // Remove that weak registration now so it cannot retain retry/readiness state indefinitely.
    PendingRegistrations.RemoveAllSwap(
        [](const FPendingRegistration& Pending)
        {
            return !Pending.Actor.IsValid();
        },
        EAllowShrinking::No);

    // A failed desired read remains part of initial readiness until it succeeds. Once neither an
    // observer nor a pending placement needs that coordinate, discard its retry marker so an old
    // corrupt far-away cell cannot hold the current area in a permanent loading state.
    for (auto It = FailedLoadRetryAt.CreateIterator(); It; ++It)
    {
        const FWorldChunkCoordinate FailedCoordinate = It.Key();
        const bool bHasPendingRegistration = PendingRegistrations.ContainsByPredicate(
            [&FailedCoordinate](const FPendingRegistration& Pending)
            {
                return Pending.Coordinate == FailedCoordinate;
            });
        if (!NewDesired.Contains(FailedCoordinate) && !bHasPendingRegistration)
        {
            It.RemoveCurrent();
        }
    }

    DesiredChunks = MoveTemp(NewDesired);
    for (const FWorldChunkCoordinate& Coordinate : NoLongerDesired) RequestUnload(Coordinate);
}

void UWorldObjectStreamingSubsystem::QueueLoad(const FWorldChunkCoordinate& Coordinate, const bool bTransient)
{
    if (LoadedChunks.Contains(Coordinate) || LoadingChunks.Contains(Coordinate)) return;

    // Persistent corruption must not enqueue and log ten reads per second. A later rebuild or a
    // boundary check retries after this short cooldown while readiness remains explicitly false.
    if (const double* RetryAt = FailedLoadRetryAt.Find(Coordinate))
    {
        if (FPlatformTime::Seconds() < *RetryAt)
        {
            return;
        }
        FailedLoadRetryAt.Remove(Coordinate);
    }

    LoadingChunks.Add(Coordinate);
    PendingLoads.Add({Coordinate, bTransient});
}

void UWorldObjectStreamingSubsystem::PumpLoads()
{
    while (bRunning && ActiveLoads < MaxConcurrentLoads && PendingLoads.Num() > 0)
    {
        // Queue order is irrelevant because every requested chunk is independently versioned.
        // Pop from the tail to avoid shifting the remaining array on every dispatch.
        const FPendingLoad Request = PendingLoads.Pop(EAllowShrinking::No);
        const uint64 RequestGeneration = Generation;
        TWeakObjectPtr<UWorldObjectStreamingSubsystem> WeakThis(this);
        const TSharedPtr<FEntityArchiveStore, ESPMode::ThreadSafe> Store = Archive;
        const FWorldChunkCoordinate Coordinate = Request.Coordinate;
        const bool bTransient = Request.bTransientBoundaryLoad;

        if (!Store.IsValid())
        {
            const double RetryAt = FPlatformTime::Seconds() + FailedLoadRetryDelaySeconds;
            LoadingChunks.Remove(Coordinate);
            FailedLoadRetryAt.Add(Coordinate, RetryAt);
            UE_LOG(LogTemp, Error,
                TEXT("Entity chunk request rejected because its archive store is closed. Chunk=(%d,%d,%d)"),
                Coordinate.X, Coordinate.Y, Coordinate.Z);
            for (const FPendingLoad& Pending : PendingLoads)
            {
                LoadingChunks.Remove(Pending.Coordinate);
                FailedLoadRetryAt.Add(Pending.Coordinate, RetryAt);
            }
            PendingLoads.Empty();
            break;
        }

        // Most cells in a large spherical streaming radius have never contained an entity. The
        // in-memory commit directory proves those cells are empty, so install them synchronously
        // and reserve the worker/OS range-read path only for coordinates that own a payload.
        if (!Store->ContainsChunk(Coordinate))
        {
            LoadingChunks.Remove(Coordinate);
            FailedLoadRetryAt.Remove(Coordinate);
            InstallLoadedChunk(
                Coordinate, bTransient, TArray<FWorldChunkObject>());
            continue;
        }

        ++ActiveLoads;
        const bool bQueued = FSafeFileIO::RunTrackedWorker(
            [WeakThis, Store, Coordinate, bTransient, RequestGeneration]() mutable
            {
                TArray<FWorldChunkObject> Entities;
                FString Error;
                const bool bSuccess = Store->LoadChunk(Coordinate, Entities, Error);
                FSafeFileIO::DispatchTrackedGameThread(
                    [WeakThis, Coordinate, bTransient, RequestGeneration, bSuccess,
                        Entities = MoveTemp(Entities), Error = MoveTemp(Error)]() mutable
                {
                    if (UWorldObjectStreamingSubsystem* StrongThis = WeakThis.Get())
                    {
                        StrongThis->FinishLoad(Coordinate, bTransient, bSuccess,
                            MoveTemp(Entities), MoveTemp(Error), RequestGeneration);
                    }
                });
            });
        if (!bQueued)
        {
            // Queue rejection is synchronous. Drain rejected requests in this loop instead of
            // calling FinishLoad(), whose normal completion path pumps again and could recurse once
            // per pending chunk while the worker pool is shutting down.
            ActiveLoads = FMath::Max(0, ActiveLoads - 1);
            LoadingChunks.Remove(Coordinate);
            const double RetryAt = FPlatformTime::Seconds() + FailedLoadRetryDelaySeconds;
            FailedLoadRetryAt.Add(Coordinate, RetryAt);
            UE_LOG(LogTemp, Error,
                TEXT("Entity chunk request rejected because the .dat worker queue is shutting down. Chunk=(%d,%d,%d)"),
                Coordinate.X, Coordinate.Y, Coordinate.Z);
            for (const FPendingLoad& Pending : PendingLoads)
            {
                LoadingChunks.Remove(Pending.Coordinate);
                FailedLoadRetryAt.Add(Pending.Coordinate, RetryAt);
            }
            PendingLoads.Empty();
            break;
        }
    }
}

void UWorldObjectStreamingSubsystem::InstallLoadedChunk(
    const FWorldChunkCoordinate& Coordinate,
    const bool bTransient,
    TArray<FWorldChunkObject>&& Entities)
{
    check(IsInGameThread());

    // This is the sole map-install path for both file-backed and known-empty coordinates. Keeping
    // actor-array alignment and pending-registration transfer together prevents index aliasing.
    FRuntimeChunk& Chunk = LoadedChunks.Add(Coordinate);
    Chunk.Objects = MoveTemp(Entities);
    Chunk.Actors.SetNum(Chunk.Objects.Num());
    Chunk.bTransientBoundaryLoad = bTransient && !DesiredChunks.Contains(Coordinate);
    for (int32 Index = 0; Index < Chunk.Objects.Num(); ++Index)
    {
        Chunk.Actors[Index] = SpawnObject(Chunk.Objects[Index]);
    }

    UWorld* const World = GetWorld();
    UStreamingMovementGateSubsystem* const Gate = World
        ? World->GetSubsystem<UStreamingMovementGateSubsystem>() : nullptr;

    // Register objects that were placed while this coordinate was queued. Reverse removal keeps
    // RemoveAtSwap valid and ensures each accepted placement is transferred exactly once.
    for (int32 Index = PendingRegistrations.Num() - 1; Index >= 0; --Index)
    {
        const FPendingRegistration& Pending = PendingRegistrations[Index];
        if (!(Pending.Coordinate == Coordinate)) continue;
        if (AActor* Actor = Pending.Actor.Get())
        {
            Chunk.Objects.Add(SnapshotActor(
                Actor,
                Pending.EntityUUID,
                Pending.ModelUUID));
            Chunk.Actors.Add(Actor);
            Chunk.bDirty = true;
            ++Chunk.Revision;
            if (Gate)
            {
                Gate->RegisterMovable(Actor);
            }
        }
        PendingRegistrations.RemoveAtSwap(Index, 1, EAllowShrinking::No);
    }
}

void UWorldObjectStreamingSubsystem::FinishLoad(
    const FWorldChunkCoordinate& Coordinate, const bool bTransient, const bool bSuccess,
    TArray<FWorldChunkObject>&& Entities, FString&& Error, const uint64 RequestGeneration)
{
    check(IsInGameThread());
    // Stop()/Start() can reuse this subsystem while an old range read is still returning. Validate
    // its session before touching counters or sets: otherwise an obsolete completion could decrement
    // the new session's ActiveLoads or remove the new request's LoadingChunks key.
    if (!bRunning || Generation != RequestGeneration) return;
    ActiveLoads = FMath::Max(0, ActiveLoads - 1);
    LoadingChunks.Remove(Coordinate);
    if (!bSuccess)
    {
        FailedLoadRetryAt.Add(
            Coordinate,
            FPlatformTime::Seconds() + FailedLoadRetryDelaySeconds);
        UE_LOG(LogTemp, Error, TEXT("Entity chunk rejected. Archive=%s Chunk=(%d,%d,%d) Reason=%s"),
            Archive.IsValid() ? *Archive->GetPath() : TEXT("<closed>"),
            Coordinate.X, Coordinate.Y, Coordinate.Z, *Error);
        PumpLoads();
        return;
    }

    FailedLoadRetryAt.Remove(Coordinate);
    InstallLoadedChunk(Coordinate, bTransient, MoveTemp(Entities));
    PumpLoads();
}

AActor* UWorldObjectStreamingSubsystem::SpawnObject(const FWorldChunkObject& Object)
{
    UWorld* World = GetWorld();
    UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
    UModelDatabaseSubsystem* Database = GameInstance ? GameInstance->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    FModelDefinition Definition;
    FString RuntimeReference;
    if (!World || !Database || !Database->ResolveLoadable(Object.ModelUUID, Definition, RuntimeReference))
    {
        const FString Message = FString::Printf(
            TEXT("Chunk object skipped because UUID is absent, invalid, or not loadable. UUID=%s"),
            *Object.ModelUUID.ToString(EGuidFormats::DigitsWithHyphensLower));
        UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
        UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("ModelUUID"), Message);
        return nullptr;
    }
    if (Definition.ModelType == EModelDefinitionType::Character)
    {
        return nullptr;
    }

    const FTransform Transform(Object.Rotation, Object.Location, Object.Scale);
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AActor* Spawned = nullptr;
    UGlTFSimulatorAssetRegistry* Registry =
        UGlTFSimulatorGameInstance::GetAssetRegistryFromContext(this);

    auto ResolveClass = [](UClass* Candidate, UClass* RequiredBase, UClass* Fallback) -> UClass*
    {
        return IsValid(Candidate) && Candidate->IsChildOf(RequiredBase)
            && !Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
            ? Candidate : Fallback;
    };

    if (Definition.ModelType == EModelDefinitionType::Static)
    {
        UClass* Candidate = Registry ? Registry->StaticActorClass.LoadSynchronous() : nullptr;
        UClass* SpawnClass = ResolveClass(Candidate, AStaticActor::StaticClass(), AStaticActor::StaticClass());
        AStaticActor* Static = World->SpawnActor<AStaticActor>(SpawnClass, Transform, Params);
        if (!IsValid(Static) && SpawnClass != AStaticActor::StaticClass())
        {
            UE_LOG(LogTemp, Warning,
                TEXT("Configured Static actor class failed during chunk restore; retrying native AStaticActor. Class=%s"),
                *GetNameSafe(SpawnClass));
            Static = World->SpawnActor<AStaticActor>(AStaticActor::StaticClass(), Transform, Params);
        }
        if (IsValid(Static))
        {
            Static->SetRenderOnlyStreaming(UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this));
            if (Static->LoadStatic(RuntimeReference, Definition.Name))
            {
                Spawned = Static;
            }
            else
            {
                Static->Destroy();
            }
        }
    }
    else if (Definition.ModelType == EModelDefinitionType::Dynamic
        && Definition.EntityType == EModelEntityType::Vehicle)
    {
        UClass* Candidate = Registry ? Registry->VehiclePawnClass.LoadSynchronous() : nullptr;
        UClass* SpawnClass = ResolveClass(Candidate, AVehiclePawn::StaticClass(), AVehiclePawn::StaticClass());
        AVehiclePawn* Vehicle = World->SpawnActor<AVehiclePawn>(SpawnClass, Transform, Params);
        if (!IsValid(Vehicle) && SpawnClass != AVehiclePawn::StaticClass())
        {
            UE_LOG(LogTemp, Warning,
                TEXT("Configured vehicle class failed during chunk restore; retrying native AVehiclePawn. Class=%s"),
                *GetNameSafe(SpawnClass));
            Vehicle = World->SpawnActor<AVehiclePawn>(AVehiclePawn::StaticClass(), Transform, Params);
        }
        if (IsValid(Vehicle) && Vehicle->LoadVehicleModel(RuntimeReference, Definition.Name))
        {
            Spawned = Vehicle;
        }
        else if (IsValid(Vehicle))
        {
            Vehicle->Destroy();
        }
    }
    else if (Definition.ModelType == EModelDefinitionType::Dynamic)
    {
        UClass* Candidate = Registry ? Registry->DynamicActorClass.LoadSynchronous() : nullptr;
        UClass* SpawnClass = ResolveClass(Candidate, ADynamicActor::StaticClass(), ADynamicActor::StaticClass());
        ADynamicActor* Entity = World->SpawnActor<ADynamicActor>(SpawnClass, Transform, Params);
        if (!IsValid(Entity) && SpawnClass != ADynamicActor::StaticClass())
        {
            UE_LOG(LogTemp, Warning,
                TEXT("Configured Dynamic actor class failed during chunk restore; retrying native ADynamicActor. Class=%s"),
                *GetNameSafe(SpawnClass));
            Entity = World->SpawnActor<ADynamicActor>(ADynamicActor::StaticClass(), Transform, Params);
        }
        if (IsValid(Entity))
        {
            Entity->SetRenderOnlyMode(UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this));
        }
        if (IsValid(Entity) && Entity->LoadDynamic(RuntimeReference, Definition.Name))
        {
            Spawned = Entity;
        }
        else if (IsValid(Entity))
        {
            Entity->Destroy();
        }
    }

    if (Spawned)
    {
        if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
        {
            Manager->TrackStreamedWorldObject(Spawned);
        }
        if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Spawned->GetRootComponent());
            IsValid(Primitive) && Primitive->IsSimulatingPhysics())
        {
            Primitive->SetPhysicsLinearVelocity(Object.Velocity);
            Primitive->SetPhysicsAngularVelocityInRadians(Object.AngularVelocity);
        }
        if (UStreamingMovementGateSubsystem* Gate = World->GetSubsystem<UStreamingMovementGateSubsystem>())
        {
            Gate->RegisterMovable(Spawned);
        }
    }
    return Spawned;
}

FWorldChunkObject UWorldObjectStreamingSubsystem::SnapshotActor(
    AActor* Actor,
    const FGuid& EntityUUID,
    const FGuid& ModelUUID) const
{
    FWorldChunkObject Result;
    Result.EntityUUID = EntityUUID;
    Result.ModelUUID = ModelUUID;
    if (!IsValid(Actor)) return Result;
    const FTransform Transform = Actor->GetActorTransform();
    Result.Location = Transform.GetLocation();
    Result.Rotation = Transform.GetRotation().GetNormalized();
    Result.Scale = Transform.GetScale3D();
    if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
        IsValid(Primitive) && Primitive->IsSimulatingPhysics())
    {
        Result.Velocity = Primitive->GetPhysicsLinearVelocity();
        Result.AngularVelocity = Primitive->GetPhysicsAngularVelocityInRadians();
    }
    else Result.Velocity = Actor->GetVelocity();
    return Result;
}

bool UWorldObjectStreamingSubsystem::IsPersistableEntity(const FGuid& UUID) const
{
    const UWorld* World = GetWorld();
    const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
    const UModelDatabaseSubsystem* Database = GameInstance
        ? GameInstance->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    FModelDefinition Definition;
    FString RuntimeReference;
    if (!Database || !Database->ResolveLoadable(UUID, Definition, RuntimeReference))
    {
        return false;
    }
    return Definition.ModelType == EModelDefinitionType::Static
        || Definition.ModelType == EModelDefinitionType::Dynamic;
}

bool UWorldObjectStreamingSubsystem::RegisterPlacedObject(AActor* Actor, const FGuid& ModelUUID)
{
    check(IsInGameThread());
    if (!bRunning || !HasPersistenceAuthority() || !IsValid(Actor) || !ModelUUID.IsValid())
    {
        if (!ModelUUID.IsValid())
        {
            const FString Message = FString::Printf(
                TEXT("Placed object registration rejected because its model UUID is invalid. Actor=%s"),
                *GetNameSafe(Actor));
            UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
            UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("ModelUUID"), Message);
        }
        return false;
    }
    if (!IsPersistableEntity(ModelUUID))
    {
        const FString Message = FString::Printf(
            TEXT("Placed object registration rejected because UUID does not resolve to a Static or Dynamic model. UUID=%s"),
            *ModelUUID.ToString(EGuidFormats::DigitsWithHyphensLower));
        UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
        UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("ModelUUID"), Message);
        return false;
    }
    const FWorldChunkCoordinate Coordinate = ToChunk(Actor->GetActorLocation());
    FRuntimeChunk* Chunk = LoadedChunks.Find(Coordinate);
    if (!Chunk)
    {
        const bool bAlreadyPending = PendingRegistrations.ContainsByPredicate(
            [Actor](const FPendingRegistration& Pending) { return Pending.Actor.Get() == Actor; });
        if (!bAlreadyPending)
        {
            PendingRegistrations.Add({Actor, FGuid::NewGuid(), ModelUUID, Coordinate});
        }
        QueueLoad(Coordinate, true);
        PumpLoads();
        return true;
    }
    Chunk->Objects.Add(SnapshotActor(Actor, FGuid::NewGuid(), ModelUUID));
    Chunk->Actors.Add(Actor);
    Chunk->bDirty = true;
    ++Chunk->Revision;
    UWorld* const World = GetWorld();
    if (UStreamingMovementGateSubsystem* Gate = World
        ? World->GetSubsystem<UStreamingMovementGateSubsystem>() : nullptr)
        Gate->RegisterMovable(Actor);
    return true;
}

void UWorldObjectStreamingSubsystem::UnregisterObject(AActor* Actor, const bool bKeepPersistentRecord)
{
    check(IsInGameThread());
    PendingRegistrations.RemoveAllSwap(
        [Actor](const FPendingRegistration& Pending) { return Pending.Actor.Get() == Actor; },
        EAllowShrinking::No);
    for (TPair<FWorldChunkCoordinate, FRuntimeChunk>& Pair : LoadedChunks)
    {
        FRuntimeChunk& Chunk = Pair.Value;
        for (int32 Index = Chunk.Actors.Num() - 1; Index >= 0; --Index)
            if (Chunk.Actors[Index].Get() == Actor)
            {
                if (bKeepPersistentRecord && Chunk.Objects.IsValidIndex(Index))
                    Chunk.Objects[Index] = SnapshotActor(
                        Actor,
                        Chunk.Objects[Index].EntityUUID,
                        Chunk.Objects[Index].ModelUUID);
                else
                {
                    Chunk.Actors.RemoveAtSwap(Index, 1, EAllowShrinking::No);
                    Chunk.Objects.RemoveAtSwap(Index, 1, EAllowShrinking::No);
                }
                Chunk.bDirty = true;
                ++Chunk.Revision;
                break;
            }
    }
    UWorld* const World = GetWorld();
    if (UStreamingMovementGateSubsystem* Gate = World
        ? World->GetSubsystem<UStreamingMovementGateSubsystem>() : nullptr)
        Gate->UnregisterMovable(Actor);
}

void UWorldObjectStreamingSubsystem::MarkObjectChanged(AActor* Actor)
{
    check(IsInGameThread());
    for (TPair<FWorldChunkCoordinate, FRuntimeChunk>& Pair : LoadedChunks)
        if (Pair.Value.Actors.ContainsByPredicate([Actor](const TWeakObjectPtr<AActor>& Item)
            { return Item.Get() == Actor; }))
        {
            Pair.Value.bDirty = true;
            ++Pair.Value.Revision;
            return;
    }
}

void UWorldObjectStreamingSubsystem::LoadRuntimeStateAsync(
    TFunction<void(bool, bool, FWorldRuntimeState, FString)> Callback)
{
    check(IsInGameThread());
    if (bRunning && !HasPersistenceAuthority())
    {
        // Clients receive mutable world/player state through replication. Report a valid missing
        // local record so startup can continue without touching or warning about the server-owned
        // .dat file, which is intentionally absent from a two-file client deployment.
        if (Callback)
        {
            Callback(true, true, FWorldRuntimeState(), FString());
        }
        return;
    }
    const TSharedPtr<FEntityArchiveStore, ESPMode::ThreadSafe> Store = Archive;
    if (!bRunning || !Store.IsValid())
    {
        if (Callback)
        {
            Callback(false, false, FWorldRuntimeState(), TEXT("The .dat streamer is not running"));
        }
        return;
    }

    // Keep the caller-owned fallback callback intact until queue submission succeeds. Moving it
    // into a rejected task would silently drop the completion and strand startup state machines.
    const bool bQueued = FSafeFileIO::RunTrackedWorker([Store, Callback]() mutable
    {
        FWorldRuntimeState State;
        bool bMissing = false;
        FString Error;
        const bool bSuccess = Store->LoadRuntimeState(State, bMissing, Error);
        FSafeFileIO::DispatchTrackedGameThread(
            [Callback = MoveTemp(Callback), bSuccess, bMissing,
                State = MoveTemp(State), Error = MoveTemp(Error)]() mutable
        {
            if (Callback) Callback(bSuccess, bMissing, MoveTemp(State), MoveTemp(Error));
        });
    });
    if (!bQueued && Callback)
    {
        Callback(false, false, FWorldRuntimeState(), TEXT("The .dat read queue is shutting down"));
    }
}

void UWorldObjectStreamingSubsystem::SaveRuntimeStateAsync(
    const FWorldRuntimeState& State,
    FSafeFileIO::FWriteCallback Callback)
{
    check(IsInGameThread());
    const TSharedPtr<FEntityArchiveStore, ESPMode::ThreadSafe> Store = Archive;
    if (!bRunning || !Store.IsValid())
    {
        if (Callback)
        {
            FSafeFileWriteResult Result;
            Result.Status = ESafeFileIOStatus::InvalidPath;
            Result.Error = TEXT("The .dat streamer is not running");
            Callback(MoveTemp(Result));
        }
        return;
    }

    const uint64 WriteOrder = Store->ReserveRuntimeStateWrite();
    const bool bQueued = FSafeFileIO::RunTrackedWorker(
        [Store, State, Callback, WriteOrder]() mutable
    {
        FSafeFileWriteResult Result = Store->SaveRuntimeState(State, WriteOrder);
        FSafeFileIO::DispatchTrackedGameThread(
            [Callback = MoveTemp(Callback), Result = MoveTemp(Result)]() mutable
        {
            if (Callback) Callback(MoveTemp(Result));
        });
    });
    if (!bQueued && Callback)
    {
        FSafeFileWriteResult Result;
        Result.Status = ESafeFileIOStatus::ShuttingDown;
        Result.Path = Store->GetPath();
        Result.Error = TEXT("The .dat write queue is shutting down");
        Callback(MoveTemp(Result));
    }
}

void UWorldObjectStreamingSubsystem::UpdateObjectsAndCrossings()
{
    TArray<TTuple<FWorldChunkCoordinate, int32, FWorldChunkCoordinate>> Crossings;
    for (TPair<FWorldChunkCoordinate, FRuntimeChunk>& Pair : LoadedChunks)
    {
        FRuntimeChunk& Chunk = Pair.Value;
        for (int32 Index = 0; Index < Chunk.Actors.Num() && Index < Chunk.Objects.Num(); ++Index)
        {
            AActor* Actor = Chunk.Actors[Index].Get();
            if (!IsValid(Actor)) continue;
            const FWorldChunkObject Snapshot = SnapshotActor(
                Actor,
                Chunk.Objects[Index].EntityUUID,
                Chunk.Objects[Index].ModelUUID);
            const FWorldChunkCoordinate Destination = ToChunk(Snapshot.Location);
            if (!(Destination == Pair.Key))
            {
                FRuntimeChunk* DestinationChunk = LoadedChunks.Find(Destination);
                if (!DestinationChunk)
                {
                    QueueLoad(Destination, true);
                    continue; // Movement gate freezes this object until the boundary file is ready.
                }
                // Do not mutate either side while one of its immutable snapshots is being written.
                // The crossing is retried after completion and will then commit both rows together.
                if (Chunk.bSaving || DestinationChunk->bSaving) continue;
                Crossings.Emplace(Pair.Key, Index, Destination);
            }
            else if (!Snapshot.Location.Equals(Chunk.Objects[Index].Location, 0.1)
                || !Snapshot.Rotation.Equals(Chunk.Objects[Index].Rotation, 0.0001)
                || !Snapshot.Scale.Equals(Chunk.Objects[Index].Scale, 0.0001)
                || !Snapshot.Velocity.Equals(Chunk.Objects[Index].Velocity, 0.1)
                || !Snapshot.AngularVelocity.Equals(Chunk.Objects[Index].AngularVelocity, 0.001))
            {
                Chunk.Objects[Index] = Snapshot;
                Chunk.bDirty = true;
                ++Chunk.Revision;
            }
        }
    }
    for (int32 MoveIndex = Crossings.Num() - 1; MoveIndex >= 0; --MoveIndex)
    {
        const FWorldChunkCoordinate Source = Crossings[MoveIndex].Get<0>();
        const int32 Index = Crossings[MoveIndex].Get<1>();
        const FWorldChunkCoordinate Destination = Crossings[MoveIndex].Get<2>();
        FRuntimeChunk* From = LoadedChunks.Find(Source);
        FRuntimeChunk* To = LoadedChunks.Find(Destination);
        if (!From || !To || !From->Actors.IsValidIndex(Index) || !From->Objects.IsValidIndex(Index)) continue;
        AActor* Actor = From->Actors[Index].Get();
        // An actor can become pending-kill between the observation and mutation phases (for example,
        // another gameplay system may destroy it from an earlier tick callback). Never manufacture a
        // zero-transform archive row from an expired weak pointer; leave the prior durable row intact
        // until the owning gameplay path explicitly unregisters it.
        if (!IsValid(Actor)) continue;
        To->Objects.Add(SnapshotActor(
            Actor,
            From->Objects[Index].EntityUUID,
            From->Objects[Index].ModelUUID));
        To->Actors.Add(Actor);
        From->Actors.RemoveAtSwap(Index, 1, EAllowShrinking::No);
        From->Objects.RemoveAtSwap(Index, 1, EAllowShrinking::No);
        From->bDirty = To->bDirty = true;
        ++From->Revision; ++To->Revision;
        // Both halves must enter the same batch this tick even if their ordinary one-second save
        // windows differ. This makes a durable generation show either side of the crossing, never
        // a duplicate or a missing entity.
        From->NextPeriodicSaveAt = 0.0;
        To->NextPeriodicSaveAt = 0.0;
        if (To->bTransientBoundaryLoad && !DesiredChunks.Contains(Destination)) To->bUnloadAfterSave = true;
    }
}

void UWorldObjectStreamingSubsystem::BeginSaveBatch(
    const TArray<FWorldChunkCoordinate>& Coordinates)
{
    if (!HasPersistenceAuthority() || Coordinates.IsEmpty()) return;
    const TSharedPtr<FEntityArchiveStore, ESPMode::ThreadSafe> Store = Archive;
    if (!Store.IsValid()) return;

    TMap<FWorldChunkCoordinate, TArray<FWorldChunkObject>> Snapshots;
    TMap<FWorldChunkCoordinate, uint64> WriteOrders;
    TArray<TTuple<FWorldChunkCoordinate, uint64>> SavedRows;
    TSet<FWorldChunkCoordinate> Seen;
    for (const FWorldChunkCoordinate& Coordinate : Coordinates)
    {
        if (Seen.Contains(Coordinate)) continue;
        Seen.Add(Coordinate);
        FRuntimeChunk* Chunk = LoadedChunks.Find(Coordinate);
        if (!Chunk || Chunk->bSaving || !Chunk->bDirty) continue;

        Chunk->bSaving = true;
        Chunk->bDirty = false;
        Chunk->SavingRevision = Chunk->Revision;
        Snapshots.Add(Coordinate, Chunk->Objects);
        WriteOrders.Add(Coordinate, Store->ReserveChunkWrite(Coordinate));
        SavedRows.Emplace(Coordinate, Chunk->SavingRevision);
    }
    if (Snapshots.IsEmpty()) return;

    const uint64 SavedGeneration = Generation;
    TWeakObjectPtr<UWorldObjectStreamingSubsystem> WeakThis(this);
    const bool bQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, Store, Snapshots, WriteOrders, SavedRows, SavedGeneration]()
    {
        const FSafeFileWriteResult Result = Store->SaveChunks(Snapshots, WriteOrders);
        FSafeFileIO::DispatchTrackedGameThread(
            [WeakThis, SavedRows, SavedGeneration, Result]()
        {
            if (UWorldObjectStreamingSubsystem* StrongThis = WeakThis.Get())
            {
                for (const TTuple<FWorldChunkCoordinate, uint64>& Row : SavedRows)
                {
                    StrongThis->CompleteSave(
                        Row.Get<0>(), Row.Get<1>(), SavedGeneration, Result);
                }
            }
        });
    });
    if (!bQueued)
    {
        FSafeFileWriteResult Result;
        Result.Status = ESafeFileIOStatus::ShuttingDown;
        Result.Path = Store->GetPath();
        Result.Error = TEXT(".dat commit worker queue is shutting down");
        for (const TTuple<FWorldChunkCoordinate, uint64>& Row : SavedRows)
        {
            CompleteSave(Row.Get<0>(), Row.Get<1>(), SavedGeneration, Result);
        }
    }
}

void UWorldObjectStreamingSubsystem::CompleteSave(
    const FWorldChunkCoordinate& Coordinate,
    const uint64 SavedRevision,
    const uint64 SavedGeneration,
    const FSafeFileWriteResult& Result)
{
    if (Generation != SavedGeneration) return;
    FRuntimeChunk* Current = LoadedChunks.Find(Coordinate);
    if (!Current || Current->SavingRevision != SavedRevision) return;
    if (!Result.IsSuccess())
    {
        UE_LOG(LogTemp, Error,
            TEXT("Chunk save failed without replacing the prior generation. File=%s Reason=%s"),
            *Result.Path, *Result.Error);
        Current->bDirty = true;
        Current->bSaving = false;
        return;
    }
    Current->bSaving = false;
    Current->NextPeriodicSaveAt = FPlatformTime::Seconds() + PeriodicSaveIntervalSeconds;
    if (Current->Revision != SavedRevision)
    {
        Current->bDirty = true;
        return;
    }
    if (Current->bUnloadAfterSave)
    {
        FinalizeUnload(Coordinate);
    }
}

void UWorldObjectStreamingSubsystem::RequestUnload(const FWorldChunkCoordinate& Coordinate)
{
    FRuntimeChunk* Chunk = LoadedChunks.Find(Coordinate);
    if (!Chunk) return;
    Chunk->bUnloadAfterSave = true;
    if (!HasPersistenceAuthority())
    {
        FinalizeUnload(Coordinate);
        return;
    }
    // Dirty chunks are picked up by the end-of-tick batch together with any related boundary
    // crossing. A chunk already being written is released by CompleteSave().
    if (!Chunk->bDirty && !Chunk->bSaving) FinalizeUnload(Coordinate);
}

void UWorldObjectStreamingSubsystem::FinalizeUnload(const FWorldChunkCoordinate& Coordinate)
{
    FRuntimeChunk Chunk;
    if (!LoadedChunks.RemoveAndCopyValue(Coordinate, Chunk)) return;
    UWorld* const World = GetWorld();
    UStreamingMovementGateSubsystem* Gate = World
        ? World->GetSubsystem<UStreamingMovementGateSubsystem>() : nullptr;
    for (const TWeakObjectPtr<AActor>& WeakActor : Chunk.Actors)
        if (AActor* Actor = WeakActor.Get())
        {
            if (Gate) Gate->UnregisterMovable(Actor);
            Actor->Destroy();
        }
}

void UWorldObjectStreamingSubsystem::Tick(float DeltaTime)
{
    check(IsInGameThread());
    if (!bRunning || !HasPersistenceAuthority()) return;
    DesiredRefreshAccumulator -= FMath::Max(0.0f, DeltaTime);
    if (DesiredRefreshAccumulator <= 0.0f)
    {
        RebuildDesiredChunks();
        // Boundary-crossing prefetch still runs every tick; the expensive radius-set rebuild only
        // needs a quarter-second cadence for 512 m cells and their one-cell safety margin.
        DesiredRefreshAccumulator = DesiredRefreshIntervalSeconds;
    }
    UpdateObjectsAndCrossings();
    PumpLoads();

    // Coalesce everything observed during this tick into one immutable write per changed chunk.
    TArray<FWorldChunkCoordinate> Dirty;
    const double Now = FPlatformTime::Seconds();
    if (HasPersistenceAuthority())
        for (const TPair<FWorldChunkCoordinate, FRuntimeChunk>& Pair : LoadedChunks)
            if (Pair.Value.bDirty && !Pair.Value.bSaving
                && (Pair.Value.bUnloadAfterSave || Now >= Pair.Value.NextPeriodicSaveAt))
            {
                Dirty.Add(Pair.Key);
            }
    BeginSaveBatch(Dirty);
}
