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
#include "Simulator/NodeTokenLibrary.h"
#include "System/MultiplayerWorldSubSystem.h"
#include "System/GameManagerSubSystem.h"
#include "System/FileFunctionLibrary.h"
#include "System/SafeFileIO.h"
#include "System/StreamingMovementGateSubsystem.h"
#include "System/GlbValidation.h"
#include "System/glTFRuntimeSafety.h"
#include "Vehicle/VehiclePawn.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeFunctionLibrary.h"

namespace
{
    FTransform ResolvePlacementNodeTransform(
        const TMap<int32, FglTFRuntimeNode>& NodeMap,
        const FglTFRuntimeNode& Node,
        bool& bOutValid)
    {
        FTransform Result = Node.Transform;
        int32 ParentIndex = Node.ParentIndex;
        TSet<int32> Visited;
        bOutValid = !Result.ContainsNaN();
        while (bOutValid)
        {
            const FglTFRuntimeNode* Parent = NodeMap.Find(ParentIndex);
            if (!Parent) break;
            if (Visited.Contains(ParentIndex) || Parent->Transform.ContainsNaN())
            {
                bOutValid = false;
                break;
            }
            Visited.Add(ParentIndex);
            Result = Result * Parent->Transform;
            ParentIndex = Parent->ParentIndex;
        }

        const FVector Location = Result.GetLocation();
        const FVector Scale = Result.GetScale3D();
        const FQuat Rotation = Result.GetRotation();
        bOutValid = bOutValid && !Result.ContainsNaN() &&
            FMath::IsFinite(Location.X) && FMath::IsFinite(Location.Y) && FMath::IsFinite(Location.Z) &&
            FMath::IsFinite(Scale.X) && FMath::IsFinite(Scale.Y) && FMath::IsFinite(Scale.Z) &&
            !Scale.IsNearlyZero() && Rotation.IsNormalized();
        return Result;
    }

    bool IsDuplicatePlacement(const FWorldChunkObject& A, const FWorldChunkObject& B)
    {
        return A.UUID == B.UUID &&
            A.Location.Equals(B.Location, 0.01) &&
            A.Rotation.Equals(B.Rotation, 0.00001) &&
            A.Scale.Equals(B.Scale, 0.00001);
    }
}

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

FString UWorldObjectStreamingSubsystem::PrefabChunkPath(const FWorldChunkCoordinate& Coordinate) const
{
    return FPaths::Combine(PrefabDataRoot, Coordinate.ToPrefabFileName());
}

FString UWorldObjectStreamingSubsystem::EntityChunkPath(const FWorldChunkCoordinate& Coordinate) const
{
    return FPaths::Combine(EntityDataRoot, Coordinate.ToEntityFileName());
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
    PrefabDataRoot = FPaths::Combine(WorldRoot, TEXT("data"), TEXT("chunks"));
    EntityDataRoot = FPaths::Combine(WorldRoot, TEXT("data"), TEXT("entities"));
    if (!IFileManager::Get().MakeDirectory(*PrefabDataRoot, true) ||
        !IFileManager::Get().MakeDirectory(*EntityDataRoot, true))
    {
        UE_LOG(LogTemp, Error, TEXT("World-object loading aborted because chunk directories could not be created. Root=%s"), *WorldRoot);
        WorldRoot.Reset();
        PrefabDataRoot.Reset();
        EntityDataRoot.Reset();
        return;
    }
    if (!ImportPlacementFilesBlocking())
    {
        UE_LOG(LogTemp, Error,
            TEXT("One or more .inst.glb placement files were preserved because import failed. Valid files remain retryable."));
    }
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
    PendingRegistrations.Empty();
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
                Chunk.Objects[Index] = SnapshotActor(Actor, Chunk.Objects[Index].UUID, Chunk.Objects[Index].StorageKind);
        if (HasPersistenceAuthority() && (Chunk.bDirty || Chunk.Revision != Chunk.SavingRevision))
        {
            TArray<FWorldChunkObject> Prefabs;
            TArray<FWorldChunkObject> Entities;
            for (const FWorldChunkObject& Object : Chunk.Objects)
            {
                if (Object.StorageKind == EWorldObjectStorageKind::Prefab)
                {
                    Prefabs.Add(Object);
                }
                else
                {
                    Entities.Add(Object);
                }
            }
            FBinaryDataStore::SaveWorldChunkAsync(PrefabChunkPath(Pair.Key), Prefabs);
            FBinaryDataStore::SaveWorldChunkAsync(EntityChunkPath(Pair.Key), Entities);
        }
        for (const TWeakObjectPtr<AActor>& Actor : Chunk.Actors)
            if (Actor.IsValid()) Actor->Destroy();
    }
    LoadedChunks.Empty();
    WorldRoot.Reset();
    PrefabDataRoot.Reset();
    EntityDataRoot.Reset();
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
        const FString PrefabPath = PrefabChunkPath(Request.Coordinate);
        const FString EntityPath = EntityChunkPath(Request.Coordinate);
        FBinaryDataStore::LoadWorldChunkAsync(PrefabPath,
            [WeakThis, Coordinate = Request.Coordinate, bTransient = Request.bTransientBoundaryLoad,
                RequestGeneration, EntityPath](bool bPrefabSuccess, TArray<FWorldChunkObject> Prefabs, FString PrefabError) mutable
            {
                UWorldObjectStreamingSubsystem* StrongThis = WeakThis.Get();
                if (!IsValid(StrongThis)) return;
                if (!bPrefabSuccess)
                {
                    StrongThis->FinishLoad(Coordinate, bTransient, false, MoveTemp(Prefabs),
                        TArray<FWorldChunkObject>(),
                        MoveTemp(PrefabError), RequestGeneration);
                    return;
                }
                FBinaryDataStore::LoadWorldChunkAsync(EntityPath,
                    [WeakThis, Coordinate, bTransient, RequestGeneration, Prefabs = MoveTemp(Prefabs)](
                        bool bEntitySuccess, TArray<FWorldChunkObject> Entities, FString EntityError) mutable
                    {
                        if (UWorldObjectStreamingSubsystem* Current = WeakThis.Get())
                        {
                            Current->FinishLoad(Coordinate, bTransient, bEntitySuccess,
                                MoveTemp(Prefabs), MoveTemp(Entities), MoveTemp(EntityError), RequestGeneration);
                        }
                    });
            });
    }
}

void UWorldObjectStreamingSubsystem::FinishLoad(
    const FWorldChunkCoordinate& Coordinate, const bool bTransient, const bool bSuccess,
    TArray<FWorldChunkObject>&& Prefabs, TArray<FWorldChunkObject>&& Entities,
    FString&& Error, const uint64 RequestGeneration)
{
    check(IsInGameThread());
    ActiveLoads = FMath::Max(0, ActiveLoads - 1);
    LoadingChunks.Remove(Coordinate);
    if (!bRunning || Generation != RequestGeneration) return;
    if (!bSuccess)
    {
        UE_LOG(LogTemp, Error, TEXT("World chunk pair rejected. Prefab=%s Entity=%s Reason=%s"),
            *PrefabChunkPath(Coordinate), *EntityChunkPath(Coordinate), *Error);
        PumpLoads();
        return;
    }

    FRuntimeChunk& Chunk = LoadedChunks.Add(Coordinate);
    for (FWorldChunkObject& Object : Prefabs) Object.StorageKind = EWorldObjectStorageKind::Prefab;
    for (FWorldChunkObject& Object : Entities) Object.StorageKind = EWorldObjectStorageKind::Entity;
    Chunk.Objects = MoveTemp(Prefabs);
    Chunk.Objects.Append(MoveTemp(Entities));
    Chunk.Actors.SetNum(Chunk.Objects.Num());
    Chunk.bTransientBoundaryLoad = bTransient && !DesiredChunks.Contains(Coordinate);
    for (int32 Index = 0; Index < Chunk.Objects.Num(); ++Index)
        Chunk.Actors[Index] = SpawnObject(Chunk.Objects[Index]);

    // Register objects that were placed while this file was still loading. Accepting the request
    // up front prevents a one-shot placement from being lost and guarantees a dirty chunk commit.
    for (int32 Index = PendingRegistrations.Num() - 1; Index >= 0; --Index)
    {
        const FPendingRegistration& Pending = PendingRegistrations[Index];
        if (!(Pending.Coordinate == Coordinate)) continue;
        if (AActor* Actor = Pending.Actor.Get())
        {
            Chunk.Objects.Add(SnapshotActor(Actor, Pending.UUID, Pending.StorageKind));
            Chunk.Actors.Add(Actor);
            Chunk.bDirty = true;
            ++Chunk.Revision;
            if (UStreamingMovementGateSubsystem* Gate = GetWorld()
                ? GetWorld()->GetSubsystem<UStreamingMovementGateSubsystem>() : nullptr)
            {
                Gate->RegisterMovable(Actor);
            }
        }
        PendingRegistrations.RemoveAtSwap(Index, 1, EAllowShrinking::No);
    }
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
        const FString Message = FString::Printf(
            TEXT("Chunk object skipped because UUID is absent, invalid, or not loadable. UUID=%s"),
            *Object.UUID.ToString(EGuidFormats::DigitsWithHyphensLower));
        UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
        UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("ModelUUID"), Message);
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

FWorldChunkObject UWorldObjectStreamingSubsystem::SnapshotActor(
    AActor* Actor,
    const FGuid& UUID,
    const EWorldObjectStorageKind StorageKind) const
{
    FWorldChunkObject Result;
    Result.UUID = UUID;
    Result.StorageKind = StorageKind;
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

EWorldObjectStorageKind UWorldObjectStreamingSubsystem::ResolveStorageKind(
    const FGuid& UUID,
    bool& bOutValid) const
{
    bOutValid = false;
    const UWorld* World = GetWorld();
    const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
    const UModelDatabaseSubsystem* Database = GameInstance
        ? GameInstance->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    FModelDefinition Definition;
    FString CachePath;
    if (!Database || !Database->ResolveLoadable(UUID, Definition, CachePath))
    {
        return EWorldObjectStorageKind::Entity;
    }

    bOutValid = Definition.ModelType == EModelDefinitionType::Prefab ||
        Definition.ModelType == EModelDefinitionType::Entity ||
        Definition.ModelType == EModelDefinitionType::Item;
    return Definition.ModelType == EModelDefinitionType::Prefab
        ? EWorldObjectStorageKind::Prefab
        : EWorldObjectStorageKind::Entity;
}

bool UWorldObjectStreamingSubsystem::ImportPlacementFilesBlocking()
{
    check(IsInGameThread());
    if (!HasPersistenceAuthority()) return true;

    UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
    UModelDatabaseSubsystem* Database = GameInstance
        ? GameInstance->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    if (!Database || !Database->IsReady())
    {
        UE_LOG(LogTemp, Error, TEXT("Placement import skipped because the model database is not ready."));
        return false;
    }

    TArray<FString> PlacementFiles;
    IFileManager::Get().FindFilesRecursive(
        PlacementFiles,
        *FPaths::Combine(WorldRoot, TEXT("model")),
        TEXT("*.inst.glb"),
        true,
        false,
        false);
    PlacementFiles.Sort();
    bool bAllImported = true;

    for (FString PlacementPath : PlacementFiles)
    {
        PlacementPath = GlbValidation::NormalizePath(PlacementPath);
        FString ValidationError;
        if (!GlbValidation::ValidateFile(PlacementPath, ValidationError))
        {
            bAllImported = false;
            const FString Message = FString::Printf(
                TEXT("Placement source preserved: invalid .inst.glb container. File=%s Reason=%s"),
                *PlacementPath, *ValidationError);
            UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
            UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("PlacementImport"), Message);
            continue;
        }

        UglTFRuntimeAsset* PlacementAsset = nullptr;
        TArray<FglTFRuntimeNode> Nodes;
        FglTFRuntimeConfig Config;
        Config.bAllowExternalFiles = false;
        const bool bParsed = FglTFRuntimeSafety::ExecuteSynchronousOperation(
            FString::Printf(TEXT("Placement import %s"), *FPaths::GetCleanFilename(PlacementPath)),
            [&PlacementAsset, &Nodes, &Config, &PlacementPath]()
            {
                PlacementAsset = UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(
                    PlacementPath, false, Config);
                if (IsValid(PlacementAsset))
                {
                    // Placement files are node-transform manifests. No mesh API is called here.
                    Nodes = PlacementAsset->GetNodes();
                }
            });
        if (!bParsed || !IsValid(PlacementAsset))
        {
            bAllImported = false;
            const FString Message = FString::Printf(
                TEXT("Placement source preserved: parser creation failed. File=%s"), *PlacementPath);
            UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
            UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("PlacementImport"), Message);
            continue;
        }

        TMap<int32, FglTFRuntimeNode> NodeMap;
        for (const FglTFRuntimeNode& Node : Nodes)
        {
            if (Node.Index >= 0 && !NodeMap.Contains(Node.Index)) NodeMap.Add(Node.Index, Node);
        }

        TMap<FWorldChunkCoordinate, TArray<FWorldChunkObject>> PlacementsByChunk;
        bool bSourceValid = true;
        int32 PlacementCount = 0;
        for (const FglTFRuntimeNode& Node : Nodes)
        {
            const FSimulatorParsedNodeName ParsedName = USimulatorNodeTokenLibrary::ParseNodeName(Node.Name);
            const bool bHasInstanceToken = ParsedName.HasEffectiveToken(FName(TEXT("INST")));
            const bool bLooksLikeInstance = Node.Name.Contains(TEXT(";INST"), ESearchCase::IgnoreCase);
            if (!bHasInstanceToken)
            {
                if (bLooksLikeInstance)
                {
                    bSourceValid = false;
                    const FString Message = FString::Printf(
                        TEXT("Placement source preserved: malformed instance node name. File=%s Node=%s Expected=<PrefabName>;INST"),
                        *PlacementPath, *Node.Name);
                    UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
                    UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("PlacementImport"), Message);
                    break;
                }
                continue;
            }
            ++PlacementCount;

            if (!Node.Name.Equals(ParsedName.BaseName + TEXT(";INST"), ESearchCase::CaseSensitive))
            {
                bSourceValid = false;
                const FString Message = FString::Printf(
                    TEXT("Placement source preserved: instance node name is not exactly <PrefabName>;INST. File=%s Node=%s"),
                    *PlacementPath, *Node.Name);
                UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
                UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("PlacementImport"), Message);
                break;
            }

            FGuid UUID;
            FString NameError;
            bool bTransformValid = false;
            const FTransform Transform = ResolvePlacementNodeTransform(NodeMap, Node, bTransformValid);
            if (!Database->FindPrefabUUIDByName(ParsedName.BaseName, UUID, NameError) || !bTransformValid)
            {
                bSourceValid = false;
                const FString Message = FString::Printf(
                    TEXT("Placement source preserved: invalid PrefabName;INST node. File=%s Node=%s Reason=%s"),
                    *PlacementPath, *Node.Name,
                    bTransformValid ? *NameError : TEXT("invalid transform or parent hierarchy"));
                UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
                UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("PlacementImport"), Message);
                break;
            }

            FWorldChunkObject Object;
            Object.UUID = UUID;
            Object.Location = Transform.GetLocation();
            Object.Rotation = Transform.GetRotation().GetNormalized();
            Object.Scale = Transform.GetScale3D();
            Object.StorageKind = EWorldObjectStorageKind::Prefab;
            PlacementsByChunk.FindOrAdd(ToChunk(Object.Location)).Add(Object);
        }

        FglTFRuntimeSafety::RequestAssetRelease(PlacementAsset);
        PlacementAsset = nullptr;
        if (!bSourceValid || PlacementCount == 0)
        {
            bAllImported = false;
            if (PlacementCount == 0)
            {
                const FString Message = FString::Printf(
                    TEXT("Placement source preserved: no valid PrefabName;INST nodes were found. File=%s"),
                    *PlacementPath);
                UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
                UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("PlacementImport"), Message);
            }
            continue;
        }

        bool bAllChunkSavesSucceeded = true;
        for (const TPair<FWorldChunkCoordinate, TArray<FWorldChunkObject>>& Pair : PlacementsByChunk)
        {
            const FString TargetPath = PrefabChunkPath(Pair.Key);
            TArray<FWorldChunkObject> Merged;
            FString LoadError;
            if (IFileManager::Get().FileExists(*TargetPath) &&
                !FBinaryDataStore::LoadWorldChunk(TargetPath, Merged, LoadError))
            {
                bAllChunkSavesSucceeded = false;
                const FString Message = FString::Printf(
                    TEXT("Placement source preserved: destination chunk could not be validated. File=%s Chunk=%s Reason=%s"),
                    *PlacementPath, *TargetPath, *LoadError);
                UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
                UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("PlacementImport"), Message);
                break;
            }

            for (const FWorldChunkObject& Incoming : Pair.Value)
            {
                if (!Merged.ContainsByPredicate(
                    [&Incoming](const FWorldChunkObject& Existing)
                    {
                        return IsDuplicatePlacement(Existing, Incoming);
                    }))
                {
                    Merged.Add(Incoming);
                }
            }
            const FSafeFileWriteResult Saved = FBinaryDataStore::SaveWorldChunkBlocking(TargetPath, Merged);
            if (!Saved.IsSuccess())
            {
                bAllChunkSavesSucceeded = false;
                const FString Message = FString::Printf(
                    TEXT("Placement source preserved: destination chunk save failed. File=%s Chunk=%s Reason=%s"),
                    *PlacementPath, *TargetPath, *Saved.Error);
                UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
                UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("PlacementImport"), Message);
                break;
            }
        }

        if (!bAllChunkSavesSucceeded || !IFileManager::Get().Delete(*PlacementPath, false, true, true))
        {
            bAllImported = false;
            const FString Message = FString::Printf(
                TEXT("Placement source preserved or could not be deleted after import. File=%s AllChunksSaved=%s"),
                *PlacementPath, bAllChunkSavesSucceeded ? TEXT("true") : TEXT("false"));
            UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
            UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("PlacementImport"), Message);
        }
    }
    return bAllImported;
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
    bool bStorageValid = false;
    const EWorldObjectStorageKind StorageKind = ResolveStorageKind(ModelUUID, bStorageValid);
    if (!bStorageValid)
    {
        const FString Message = FString::Printf(
            TEXT("Placed object registration rejected because UUID does not resolve to a prefab, entity, or item. UUID=%s"),
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
            PendingRegistrations.Add({Actor, ModelUUID, Coordinate, StorageKind});
        }
        QueueLoad(Coordinate, true);
        PumpLoads();
        return true;
    }
    Chunk->Objects.Add(SnapshotActor(Actor, ModelUUID, StorageKind));
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
                    Chunk.Objects[Index] = SnapshotActor(Actor, Chunk.Objects[Index].UUID, Chunk.Objects[Index].StorageKind);
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
            const FWorldChunkObject Snapshot = SnapshotActor(
                Actor, Chunk.Objects[Index].UUID, Chunk.Objects[Index].StorageKind);
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
        To->Objects.Add(SnapshotActor(
            Actor, From->Objects[Index].UUID, From->Objects[Index].StorageKind));
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
    Chunk.PendingSaveParts = 2;
    Chunk.bSaveBatchFailed = false;
    const uint64 SavedRevision = Chunk.SavingRevision;
    const uint64 SavedGeneration = Generation;
    TArray<FWorldChunkObject> Prefabs;
    TArray<FWorldChunkObject> Entities;
    for (const FWorldChunkObject& Object : Chunk.Objects)
    {
        if (Object.StorageKind == EWorldObjectStorageKind::Prefab)
        {
            Prefabs.Add(Object);
        }
        else
        {
            Entities.Add(Object);
        }
    }
    TWeakObjectPtr<UWorldObjectStreamingSubsystem> WeakThis(this);
    const auto CompletePart = [WeakThis, Coordinate, SavedRevision, SavedGeneration](FSafeFileWriteResult Result)
    {
        if (UWorldObjectStreamingSubsystem* StrongThis = WeakThis.Get())
        {
            StrongThis->CompleteSavePart(Coordinate, SavedRevision, SavedGeneration, Result);
        }
    };
    FBinaryDataStore::SaveWorldChunkAsync(PrefabChunkPath(Coordinate), Prefabs, CompletePart);
    FBinaryDataStore::SaveWorldChunkAsync(EntityChunkPath(Coordinate), Entities, CompletePart);
}

void UWorldObjectStreamingSubsystem::CompleteSavePart(
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
        Current->bSaveBatchFailed = true;
        UE_LOG(LogTemp, Error,
            TEXT("Chunk save failed without replacing the prior generation. File=%s Reason=%s"),
            *Result.Path, *Result.Error);
    }
    Current->PendingSaveParts = FMath::Max(0, Current->PendingSaveParts - 1);
    if (Current->PendingSaveParts > 0) return;

    Current->bSaving = false;
    if (Current->bSaveBatchFailed || Current->Revision != SavedRevision)
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
