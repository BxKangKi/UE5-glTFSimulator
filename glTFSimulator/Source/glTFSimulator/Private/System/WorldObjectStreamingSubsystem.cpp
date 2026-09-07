// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/WorldObjectStreamingSubsystem.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "World/PrefabActor.h"
#include "Simulator/ModelDatabaseSubsystem.h"
#include "System/MultiplayerWorldSubSystem.h"
#include "System/GameManagerSubSystem.h"
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

FString UWorldObjectStreamingSubsystem::ChunkPath(const FWorldChunkCoordinate& Coordinate) const
{
    return FPaths::Combine(DataRoot, Coordinate.ToFileName());
}

void UWorldObjectStreamingSubsystem::Start(const FString& InWorldRoot, const float InLoadRadiusMeters)
{
    check(IsInGameThread());
    Stop();
    WorldRoot = FSafeFileIO::NormalizeFilePath(InWorldRoot);
    DataRoot = FPaths::Combine(WorldRoot, TEXT("data"));
    IFileManager::Get().MakeDirectory(*DataRoot, true);
    LoadRadiusCentimeters = FMath::Max(51200.0f, InLoadRadiusMeters * 100.0f);
    ++Generation;
    bRunning = true;
    DesiredRefreshAccumulator = 0.0f;
    RebuildDesiredChunks();
    PumpLoads();
}

void UWorldObjectStreamingSubsystem::Stop()
{
    check(IsInGameThread());
    ++Generation;
    bRunning = false;
    PendingLoads.Empty();
    LoadingChunks.Empty();
    DesiredChunks.Empty();
    ActiveLoads = 0;

    // Snapshot and enqueue final writes before actors are destroyed. FSafeFileIO owns the copied
    // bytes and module shutdown drains its tracked workers; callbacks hold no raw UObject pointer.
    for (TPair<FWorldChunkCoordinate, FRuntimeChunk>& Pair : LoadedChunks)
    {
        FRuntimeChunk& Chunk = Pair.Value;
        for (int32 Index = 0; Index < Chunk.Actors.Num() && Index < Chunk.Objects.Num(); ++Index)
            if (AActor* Actor = Chunk.Actors[Index].Get())
                Chunk.Objects[Index] = SnapshotActor(Actor, Chunk.Objects[Index].UUID);
        if (HasPersistenceAuthority() && (Chunk.bDirty || Chunk.Revision != Chunk.SavingRevision))
            FBinaryDataStore::SaveWorldChunkAsync(ChunkPath(Pair.Key), Chunk.Objects);
        for (const TWeakObjectPtr<AActor>& Actor : Chunk.Actors)
            if (Actor.IsValid()) Actor->Destroy();
    }
    LoadedChunks.Empty();
    WorldRoot.Reset();
    DataRoot.Reset();
    DesiredRefreshAccumulator = 0.0f;
}

void UWorldObjectStreamingSubsystem::Deinitialize()
{
    Stop();
    Super::Deinitialize();
}

bool UWorldObjectStreamingSubsystem::IsLocationLoaded(const FVector& WorldLocation) const
{
    return LoadedChunks.Contains(ToChunk(WorldLocation));
}

void UWorldObjectStreamingSubsystem::EnsureLocationLoaded(const FVector& WorldLocation)
{
    check(IsInGameThread());
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
    const double RadiusSq = FMath::Square(static_cast<double>(LoadRadiusCentimeters + ChunkSizeCentimeters));
    for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
    {
        const APlayerController* Controller = It->Get();
        const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
        if (!IsValid(Pawn)) continue;
        const FVector Observer = Pawn->GetActorLocation();
        const FWorldChunkCoordinate Center = ToChunk(Observer);
        for (int32 Z = Center.Z - Radius; Z <= Center.Z + Radius; ++Z)
            for (int32 Y = Center.Y - Radius; Y <= Center.Y + Radius; ++Y)
                for (int32 X = Center.X - Radius; X <= Center.X + Radius; ++X)
                {
                    const FVector ChunkCenter((X + 0.5) * ChunkSizeCentimeters,
                        (Y + 0.5) * ChunkSizeCentimeters, (Z + 0.5) * ChunkSizeCentimeters);
                    if (FVector::DistSquared(Observer, ChunkCenter) <= RadiusSq)
                        NewDesired.Add({X, Y, Z});
                }
    }
    for (const FWorldChunkCoordinate& Coordinate : NewDesired)
    {
        if (FRuntimeChunk* Existing = LoadedChunks.Find(Coordinate)) Existing->bTransientBoundaryLoad = false;
        else if (!LoadingChunks.Contains(Coordinate)) QueueLoad(Coordinate, false);
    }
    TArray<FWorldChunkCoordinate> NoLongerDesired;
    for (const TPair<FWorldChunkCoordinate, FRuntimeChunk>& Pair : LoadedChunks)
        if (!NewDesired.Contains(Pair.Key) && !Pair.Value.bTransientBoundaryLoad) NoLongerDesired.Add(Pair.Key);
    DesiredChunks = MoveTemp(NewDesired);
    for (const FWorldChunkCoordinate& Coordinate : NoLongerDesired) RequestUnload(Coordinate);
}

void UWorldObjectStreamingSubsystem::QueueLoad(const FWorldChunkCoordinate& Coordinate, const bool bTransient)
{
    if (LoadedChunks.Contains(Coordinate) || LoadingChunks.Contains(Coordinate)) return;
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
        ++ActiveLoads;
        const uint64 RequestGeneration = Generation;
        TWeakObjectPtr<UWorldObjectStreamingSubsystem> WeakThis(this);
        FBinaryDataStore::LoadWorldChunkAsync(ChunkPath(Request.Coordinate),
            [WeakThis, Coordinate = Request.Coordinate, bTransient = Request.bTransientBoundaryLoad,
                RequestGeneration](bool bSuccess, TArray<FWorldChunkObject> Objects, FString Error) mutable
            {
                if (UWorldObjectStreamingSubsystem* StrongThis = WeakThis.Get())
                    StrongThis->FinishLoad(Coordinate, bTransient, bSuccess, MoveTemp(Objects), MoveTemp(Error), RequestGeneration);
            });
    }
}

void UWorldObjectStreamingSubsystem::FinishLoad(
    const FWorldChunkCoordinate& Coordinate, const bool bTransient, const bool bSuccess,
    TArray<FWorldChunkObject>&& Objects, FString&& Error, const uint64 RequestGeneration)
{
    check(IsInGameThread());
    ActiveLoads = FMath::Max(0, ActiveLoads - 1);
    LoadingChunks.Remove(Coordinate);
    if (!bRunning || Generation != RequestGeneration) return;
    if (!bSuccess)
    {
        UE_LOG(LogTemp, Error, TEXT("World chunk rejected. File=%s Reason=%s"), *ChunkPath(Coordinate), *Error);
        PumpLoads();
        return;
    }

    FRuntimeChunk& Chunk = LoadedChunks.Add(Coordinate);
    Chunk.Objects = MoveTemp(Objects);
    Chunk.Actors.SetNum(Chunk.Objects.Num());
    Chunk.bTransientBoundaryLoad = bTransient && !DesiredChunks.Contains(Coordinate);
    for (int32 Index = 0; Index < Chunk.Objects.Num(); ++Index)
        Chunk.Actors[Index] = SpawnObject(Chunk.Objects[Index]);
    PumpLoads();
}

AActor* UWorldObjectStreamingSubsystem::SpawnObject(const FWorldChunkObject& Object)
{
    UWorld* World = GetWorld();
    UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
    UModelDatabaseSubsystem* Database = GameInstance ? GameInstance->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    FModelDefinition Definition;
    FString CachePath;
    if (!World || !Database || !Database->ResolveLoadable(Object.UUID, Definition, CachePath))
    {
        // Missing UUIDs are deliberately preserved in the chunk and ignored at runtime.
        return nullptr;
    }
    if (Definition.ModelType == EModelDefinitionType::None
        || Definition.ModelType == EModelDefinitionType::Scene
        || Definition.ModelType == EModelDefinitionType::Character)
        return nullptr;

    const FTransform Transform(Object.Rotation, Object.Location, Object.Scale);
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AActor* Spawned = nullptr;
    if (Definition.ModelType == EModelDefinitionType::Entity && Definition.EntityType == EModelEntityType::Vehicle)
    {
        AVehiclePawn* Vehicle = World->SpawnActor<AVehiclePawn>(AVehiclePawn::StaticClass(), Transform, Params);
        if (IsValid(Vehicle) && Vehicle->LoadVehicleModel(Definition.GlbPath, Definition.Name)) Spawned = Vehicle;
        else if (IsValid(Vehicle)) Vehicle->Destroy();
    }
    else if (Definition.ModelType == EModelDefinitionType::Prefab
        || Definition.ModelType == EModelDefinitionType::Item
        || (Definition.ModelType == EModelDefinitionType::Entity
            && (Definition.EntityType == EModelEntityType::Prop
                || Definition.EntityType == EModelEntityType::Animal)))
    {
        // Until a dedicated animal actor is introduced, Animal is a movable generic entity. It is
        // still classified distinctly in JSON/db.dat and participates in chunk persistence.
        APrefabActor* Prefab = World->SpawnActor<APrefabActor>(APrefabActor::StaticClass(), Transform, Params);
        if (IsValid(Prefab))
            Prefab->SetRenderOnlyMode(UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this));
        if (IsValid(Prefab) && Prefab->LoadPrefab(Definition.GlbPath, Definition.Name)) Spawned = Prefab;
        else if (IsValid(Prefab)) Prefab->Destroy();
    }
    if (Spawned)
    {
        if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
            Manager->TrackStreamedWorldObject(Spawned);
        if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Spawned->GetRootComponent());
            IsValid(Primitive) && Primitive->IsSimulatingPhysics())
        {
            Primitive->SetPhysicsLinearVelocity(Object.Velocity);
            Primitive->SetPhysicsAngularVelocityInRadians(Object.AngularVelocity);
        }
        if (UStreamingMovementGateSubsystem* Gate = World->GetSubsystem<UStreamingMovementGateSubsystem>())
            Gate->RegisterMovable(Spawned);
    }
    return Spawned;
}

FWorldChunkObject UWorldObjectStreamingSubsystem::SnapshotActor(AActor* Actor, const FGuid& UUID) const
{
    FWorldChunkObject Result;
    Result.UUID = UUID;
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

bool UWorldObjectStreamingSubsystem::RegisterPlacedObject(AActor* Actor, const FGuid& ModelUUID)
{
    check(IsInGameThread());
    if (!bRunning || !HasPersistenceAuthority() || !IsValid(Actor) || !ModelUUID.IsValid()) return false;
    const FWorldChunkCoordinate Coordinate = ToChunk(Actor->GetActorLocation());
    FRuntimeChunk* Chunk = LoadedChunks.Find(Coordinate);
    if (!Chunk)
    {
        QueueLoad(Coordinate, true);
        PumpLoads();
        return false;
    }
    Chunk->Objects.Add(SnapshotActor(Actor, ModelUUID));
    Chunk->Actors.Add(Actor);
    Chunk->bDirty = true;
    ++Chunk->Revision;
    if (UStreamingMovementGateSubsystem* Gate = GetWorld()->GetSubsystem<UStreamingMovementGateSubsystem>())
        Gate->RegisterMovable(Actor);
    return true;
}

void UWorldObjectStreamingSubsystem::UnregisterObject(AActor* Actor, const bool bKeepPersistentRecord)
{
    check(IsInGameThread());
    for (TPair<FWorldChunkCoordinate, FRuntimeChunk>& Pair : LoadedChunks)
    {
        FRuntimeChunk& Chunk = Pair.Value;
        for (int32 Index = Chunk.Actors.Num() - 1; Index >= 0; --Index)
            if (Chunk.Actors[Index].Get() == Actor)
            {
                if (bKeepPersistentRecord && Chunk.Objects.IsValidIndex(Index))
                    Chunk.Objects[Index] = SnapshotActor(Actor, Chunk.Objects[Index].UUID);
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
    if (UStreamingMovementGateSubsystem* Gate = GetWorld()->GetSubsystem<UStreamingMovementGateSubsystem>())
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
            const FWorldChunkObject Snapshot = SnapshotActor(Actor, Chunk.Objects[Index].UUID);
            const FWorldChunkCoordinate Destination = ToChunk(Snapshot.Location);
            if (!(Destination == Pair.Key))
            {
                if (!LoadedChunks.Contains(Destination))
                {
                    QueueLoad(Destination, true);
                    continue; // Movement gate freezes this object until the boundary file is ready.
                }
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
        To->Objects.Add(SnapshotActor(Actor, From->Objects[Index].UUID));
        To->Actors.Add(Actor);
        From->Actors.RemoveAtSwap(Index, 1, EAllowShrinking::No);
        From->Objects.RemoveAtSwap(Index, 1, EAllowShrinking::No);
        From->bDirty = To->bDirty = true;
        ++From->Revision; ++To->Revision;
        if (To->bTransientBoundaryLoad && !DesiredChunks.Contains(Destination)) To->bUnloadAfterSave = true;
    }
}

void UWorldObjectStreamingSubsystem::BeginSave(const FWorldChunkCoordinate& Coordinate, FRuntimeChunk& Chunk)
{
    if (!HasPersistenceAuthority() || Chunk.bSaving || !Chunk.bDirty) return;
    Chunk.bSaving = true;
    Chunk.bDirty = false;
    Chunk.SavingRevision = Chunk.Revision;
    const uint64 SavedRevision = Chunk.SavingRevision;
    const uint64 SavedGeneration = Generation;
    TWeakObjectPtr<UWorldObjectStreamingSubsystem> WeakThis(this);
    FBinaryDataStore::SaveWorldChunkAsync(ChunkPath(Coordinate), Chunk.Objects,
        [WeakThis, Coordinate, SavedRevision, SavedGeneration](FSafeFileWriteResult Result)
        {
            UWorldObjectStreamingSubsystem* StrongThis = WeakThis.Get();
            if (!IsValid(StrongThis) || StrongThis->Generation != SavedGeneration) return;
            FRuntimeChunk* Current = StrongThis->LoadedChunks.Find(Coordinate);
            if (!Current) return;
            Current->bSaving = false;
            if (!Result.IsSuccess())
            {
                Current->bDirty = true;
                UE_LOG(LogTemp, Error, TEXT("Chunk save failed without replacing the prior generation. File=%s Reason=%s"),
                    *Result.Path, *Result.Error);
                return;
            }
            if (Current->Revision != SavedRevision) Current->bDirty = true;
            else if (Current->bUnloadAfterSave) StrongThis->FinalizeUnload(Coordinate);
        });
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
    if (Chunk->bDirty || Chunk->bSaving) BeginSave(Coordinate, *Chunk);
    else FinalizeUnload(Coordinate);
}

void UWorldObjectStreamingSubsystem::FinalizeUnload(const FWorldChunkCoordinate& Coordinate)
{
    FRuntimeChunk Chunk;
    if (!LoadedChunks.RemoveAndCopyValue(Coordinate, Chunk)) return;
    UStreamingMovementGateSubsystem* Gate = GetWorld()
        ? GetWorld()->GetSubsystem<UStreamingMovementGateSubsystem>() : nullptr;
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
    if (!bRunning) return;
    DesiredRefreshAccumulator -= FMath::Max(0.0f, DeltaTime);
    if (DesiredRefreshAccumulator <= 0.0f)
    {
        RebuildDesiredChunks();
        DesiredRefreshAccumulator = 0.10f;
    }
    UpdateObjectsAndCrossings();
    PumpLoads();

    // Coalesce everything observed during this tick into one immutable write per changed chunk.
    TArray<FWorldChunkCoordinate> Dirty;
    if (HasPersistenceAuthority())
        for (const TPair<FWorldChunkCoordinate, FRuntimeChunk>& Pair : LoadedChunks)
            if (Pair.Value.bDirty && !Pair.Value.bSaving) Dirty.Add(Pair.Key);
    for (const FWorldChunkCoordinate& Coordinate : Dirty)
        if (FRuntimeChunk* Chunk = LoadedChunks.Find(Coordinate)) BeginSave(Coordinate, *Chunk);
}
