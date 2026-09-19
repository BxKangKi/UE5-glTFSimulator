// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file StaticActor.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Model/StaticActor.h"

#include "Async/ParallelFor.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "Model/InstancedMeshActor.h"
#include "Model/WorldSceneStreamAction.h"
#include "Model/glTFMaterialOverrideUtils.h"
#include "Setting/GameSettings.h"
#include "Simulator/ModelDatabaseSubsystem.h"
#include "Simulator/ModelDefinitionJson.h"
#include "Simulator/RuntimeModelResolver.h"
#include "System/ActorHelper.h"
#include "System/FileFunctionLibrary.h"
#include "System/GameManagerSubSystem.h"
#include "System/GameUpdateSubSystem.h"
#include "System/SafeFileIO.h"
#include "System/StreamingMovementGateSubsystem.h"
#include "System/WorldArchive.h"
#include "System/WorldBakedModelAsset.h"
#include "System/glTFRuntimeSafety.h"
#include "System/GlTFSimulatorGameInstance.h"
#include "System/GlTFSimulatorAssetRegistry.h"
#include "TimerManager.h"
#include "World/WaterActor.h"
#include "UObject/UObjectGlobals.h"

namespace StaticActorPrivate
{
    struct FStaticPreparedGroup
    {
        FInstancedMeshGroupInitData Data;
        TSet<FName> MeshNames;
    };

    struct FStaticGroupingWork
    {
        TMap<FName, FModelNodeData> Nodes;
        TMap<FName, FName> MeshToGroup;
        TMap<FName, FInstancedMeshGroupInitData> Groups;
        int32 InvalidNodeCount = 0;
    };

    void BuildStaticGroups(FStaticGroupingWork& Work)
    {
        TMap<FName, FStaticPreparedGroup> PreparedGroups;
        PreparedGroups.Reserve(Work.MeshToGroup.Num());
        Work.Groups.Reset();
        Work.InvalidNodeCount = 0;

        for (TPair<FName, FModelNodeData>& Pair : Work.Nodes)
        {
            const FModelNodeData& Node = Pair.Value;
            const FName* GroupName = Work.MeshToGroup.Find(Node.MeshName);
            if (Pair.Key.IsNone()
                || Node.MeshName.IsNone()
                || Node.Transform.ContainsNaN()
                || !GroupName)
            {
                ++Work.InvalidNodeCount;
                continue;
            }

            FStaticPreparedGroup& Group = PreparedGroups.FindOrAdd(*GroupName);
            Group.MeshNames.Add(Node.MeshName);
            if (Node.bAlwaysLoaded
                || Node.FineChunk.X < 0 || Node.FineChunk.X >= 16
                || Node.FineChunk.Y < 0 || Node.FineChunk.Y >= 16
                || Node.FineChunk.Z < 0 || Node.FineChunk.Z >= 16)
            {
                Group.Data.AlwaysLoadedNodeNames.Add(Pair.Key);
            }
            else
            {
                Group.Data.SpatialChunks.FindOrAdd(Node.CoarseChunk)
                    .FindOrAdd(Node.FineChunk).Add(Pair.Key);
            }
            Group.Data.Nodes.Add(Pair.Key, MoveTemp(Pair.Value));
        }

        Work.Groups.Reserve(PreparedGroups.Num());
        for (TPair<FName, FStaticPreparedGroup>& Pair : PreparedGroups)
        {
            FInstancedMeshGroupInitData& Data = Pair.Value.Data;
            Data.ReferencedMeshNames = Pair.Value.MeshNames.Array();
            Data.ReferencedMeshNames.Sort([](const FName A, const FName B) { return A.LexicalLess(B); });
            Data.AlwaysLoadedNodeNames.Sort([](const FName A, const FName B) { return A.LexicalLess(B); });
            Work.Groups.Add(Pair.Key, MoveTemp(Data));
        }
        Work.Nodes.Reset();
        Work.MeshToGroup.Reset();
    }

    bool EnsureGameThread(const TCHAR* FunctionName)
    {
        return ensureMsgf(
            IsInGameThread(), TEXT("%s must run on the game thread"), FunctionName);
    }

    FName MakeMeshGroupName(const FModelMeshData& Mesh)
    {
        return FName(*FString::Printf(
            TEXT("Mesh_%d_%d_%d_%d_C%d_S%d"),
            Mesh.LOD0,
            Mesh.LOD1,
            Mesh.LOD2,
            Mesh.LOD3,
            Mesh.Data.bComplexCollision ? 1 : 0,
            Mesh.Data.bSimpleCollision ? 1 : 0));
    }

    FString NormalizeReference(const FString& Reference)
    {
        FGuid UUID;
        return FGWorldArchive::ParseModelReference(Reference, UUID)
            ? FGWorldArchive::MakeModelReference(UUID)
            : FString();
    }

    bool ResolveScene(
        UObject* Context,
        const FString& Reference,
        FGuid& OutUUID,
        FString& OutCanonicalReference,
        FString& OutReason)
    {
        const UWorld* World = IsValid(Context) ? Context->GetWorld() : nullptr;
        const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
        const UModelDatabaseSubsystem* Database = GameInstance
            ? GameInstance->GetSubsystem<UModelDatabaseSubsystem>()
            : nullptr;
        FModelDefinition Definition;
        if (!Database
            || !Database->IsBuiltWorld()
            || !Database->FindUUIDForReference(Reference, OutUUID)
            || !Database->ResolveLoadable(
                OutUUID, Definition, OutCanonicalReference))
        {
            OutReason = TEXT("model is absent from the verified .gworld directory");
            return false;
        }
        if (Definition.ModelType != EModelDefinitionType::Static)
        {
            OutReason = FString::Printf(
                TEXT("world scene requires ModelType=Static; got %s"),
                *ModelDefinitionJson::ModelTypeToString(Definition.ModelType));
            return false;
        }
        if (NormalizeReference(OutCanonicalReference).IsEmpty())
        {
            OutReason = TEXT("model database returned a non-gworld runtime reference");
            return false;
        }
        return true;
    }
}

AStaticActor::AStaticActor()
{
    PrimaryActorTick.bCanEverTick = false;
    Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    Root->SetMobility(EComponentMobility::Movable);
    SetRootComponent(Root);
}

void AStaticActor::Init(const FString& InModelReference)
{
    if (!StaticActorPrivate::EnsureGameThread(
            TEXT("AStaticActor::Init")))
    {
        return;
    }

    const bool bRestart = HasActorBegunPlay();
    if (bRestart)
    {
        ReleaseRuntimeResourcesForWorldExit();
    }

    ModelReference = StaticActorPrivate::NormalizeReference(
        InModelReference);
    bRuntimeResourcesReleased = false;
    bIsDestroyed = false;
    if (bRestart)
    {
        bIsLoaded = false;
        bAsyncLoading = false;
        LoadingStatus = 0.0f;
        StartBuiltLoad();
    }
}


bool AStaticActor::LoadStatic(const FString& InModelReference, const FString& InObjectName)
{
    check(IsInGameThread());
    FResolvedRuntimeModel Resolved;
    FString Error;
    if (!FRuntimeModelResolver::Resolve(this, InModelReference, Resolved, Error)
        || Resolved.Definition.ModelType != EModelDefinitionType::Static)
    {
        UE_LOG(LogTemp, Error, TEXT("StaticActor rejected model reference '%s': %s"),
            *InModelReference, Error.IsEmpty() ? TEXT("ModelType must be Static") : *Error);
        return false;
    }

    ObjectName = InObjectName;
    BaseName = Resolved.Definition.Name;
    Init(Resolved.Reference);
    return !ModelReference.IsEmpty();
}
void AStaticActor::SetRenderOnlyStreaming(const bool bRenderOnly)
{
    if (StaticActorPrivate::EnsureGameThread(
            TEXT("AStaticActor::SetRenderOnlyStreaming")))
    {
        bRenderOnlyStreaming = bRenderOnly;
    }
}

void AStaticActor::BeginPlay()
{
    if (!StaticActorPrivate::EnsureGameThread(
            TEXT("AStaticActor::BeginPlay")))
    {
        return;
    }
    Super::BeginPlay();

    bRuntimeResourcesReleased = false;
    bIsDestroyed = false;
    bIsLoaded = false;
    bAsyncLoading = false;
    bHasModelMetadata = false;
    LoadingStatus = 0.0f;
    GameUpdateTickHandle = INDEX_NONE;
    MetadataRequestSerial = 0;
    ActiveStreamActions.Empty();
    StreamGroupProgress.Empty();
    AllNodeMap.Empty();
    AllMeshMap.Empty();
    WaterNodeMap.Empty();
    LoadedWaterNodes.Empty();
    OwnedInstancedMeshActors.Empty();
    WaterActorMap.Empty();
    PendingInstancedGroups.Empty();
    PendingInstancedGroupNames.Empty();
    PendingInstancedGroupIndex = 0;
    PendingStreamGroupNames.Empty();
    PendingStreamGroupIndex = 0;
    bPendingWaterStream = false;
    ModelMetadata = FModelData();

    if (UGlTFSimulatorAssetRegistry* Registry =
            UGlTFSimulatorGameInstance::GetAssetRegistryFromContext(this))
    {
        if (!IsValid(DecalLight))
        {
            DecalLight = Registry->StaticDecalLightMaterial.LoadSynchronous();
        }
        if (!WaterClass)
        {
            UClass* ResolvedWaterClass = Registry->WaterActorClass.LoadSynchronous();
            if (IsValid(ResolvedWaterClass) && ResolvedWaterClass->IsChildOf(AWaterActor::StaticClass())
                && !ResolvedWaterClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
            {
                WaterClass = ResolvedWaterClass;
            }
        }
    }
    if (!WaterClass)
    {
        WaterClass = AWaterActor::StaticClass();
    }

    StartBuiltLoad();
}

void AStaticActor::StartBuiltLoad()
{
    check(IsInGameThread());
    if (bIsDestroyed)
    {
        return;
    }

    ModelReference = StaticActorPrivate::NormalizeReference(ModelReference);
    FGuid UUID;
    FString CanonicalReference;
    FString Reason;
    if (ModelReference.IsEmpty()
        || !StaticActorPrivate::ResolveScene(
            this, ModelReference, UUID, CanonicalReference, Reason))
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        WriteLogAsync(FString::Printf(
            TEXT("Static load rejected. Reference=%s Reason=%s"),
            *ModelReference,
            Reason.IsEmpty() ? TEXT("invalid gworld reference") : *Reason));
        return;
    }

    ModelReference = CanonicalReference;
    LoadBuiltMetadataAsync(UUID);
}

void AStaticActor::EndPlay(
    const EEndPlayReason::Type EndPlayReason)
{
    ReleaseRuntimeResourcesForWorldExit();
    Super::EndPlay(EndPlayReason);
}

void AStaticActor::Destroyed()
{
    ReleaseRuntimeResourcesForWorldExit();
    Super::Destroyed();
}

void AStaticActor::ReleaseRuntimeResourcesForWorldExit()
{
    if (!StaticActorPrivate::EnsureGameThread(
            TEXT("AStaticActor::ReleaseRuntimeResourcesForWorldExit"))
        || bRuntimeResourcesReleased)
    {
        return;
    }
    bRuntimeResourcesReleased = true;

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearAllTimersForObject(this);
        if (UStreamingMovementGateSubsystem* Gate =
                World->GetSubsystem<UStreamingMovementGateSubsystem>())
        {
            Gate->ClearModelRegions(this);
        }
    }

    bIsDestroyed = true;
    bAsyncLoading = false;
    bIsLoaded = false;
    LoadingStatus = 1.0f;
    ++MetadataRequestSerial;
    UnregisterGameUpdate();
    CancelActiveStreamActions();
    ReleaseStreamingResources();
    BakedAsset = nullptr;
    AllNodeMap.Empty();
    AllMeshMap.Empty();
    WaterNodeMap.Empty();
    LoadedWaterNodes.Empty();
    WaterActorMap.Empty();
    PendingInstancedGroups.Empty();
    PendingInstancedGroupNames.Empty();
    PendingInstancedGroupIndex = 0;
    PendingStreamGroupNames.Empty();
    PendingStreamGroupIndex = 0;
    bPendingWaterStream = false;
    ModelMetadata = FModelData();
    bHasModelMetadata = false;
    ModelReference.Reset();
    StreamGroupProgress.Empty();
}

void AStaticActor::LoadBuiltMetadataAsync(const FGuid& UUID)
{
    check(IsInGameThread());
    const UGameInstance* GameInstance = GetGameInstance();
    const UModelDatabaseSubsystem* Database = GameInstance
        ? GameInstance->GetSubsystem<UModelDatabaseSubsystem>()
        : nullptr;
    const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Reader =
        Database ? Database->GetArchiveReaderForModel(UUID) : nullptr;
    const uint64 RequestSerial = ++MetadataRequestSerial;
    const FString ExpectedReference = ModelReference;
    bAsyncLoading = true;
    LoadingStatus = FMath::Max(LoadingStatus, 0.05f);

    TWeakObjectPtr<AStaticActor> WeakThis(this);
    const bool bQueued = Reader.IsValid()
        && FSafeFileIO::RunTrackedWorker(
            [WeakThis, Reader, UUID, RequestSerial, ExpectedReference]() mutable
            {
                FGWorldModelMetadata Metadata;
                FGWorldModelManifest Manifest;
                FString Errors[2];
                bool Results[2] = { false, false };

                // Metadata and the manifest occupy independent immutable archive members. Reading
                // them concurrently uses private file handles and overlaps decompression/CRC work
                // without ever sharing a seek cursor or touching a UObject from a worker.
                ParallelFor(2, [&](const int32 Index)
                {
                    if (Index == 0)
                    {
                        Results[0] = Reader->ReadModelMetadata(UUID, Metadata, Errors[0]);
                    }
                    else
                    {
                        Results[1] = Reader->ReadModelManifest(UUID, Manifest, Errors[1]);
                    }
                });

                const bool bSuccess = Results[0] && Results[1];
                FString Error;
                if (!bSuccess)
                {
                    Error = !Results[0] ? MoveTemp(Errors[0]) : MoveTemp(Errors[1]);
                    if (Error.IsEmpty())
                    {
                        Error = TEXT("failed to read prepared .gworld model tables");
                    }
                }

                FSafeFileIO::DispatchTrackedGameThread(
                    [WeakThis,
                        Reader,
                        UUID,
                        RequestSerial,
                        ExpectedReference,
                        bSuccess,
                        Metadata = MoveTemp(Metadata),
                        Manifest = MoveTemp(Manifest),
                        Error = MoveTemp(Error)]() mutable
                    {
                        if (AStaticActor* StrongThis = WeakThis.Get())
                        {
                            StrongThis->OnBuiltMetadataLoaded(
                                RequestSerial,
                                ExpectedReference,
                                UUID,
                                Reader,
                                bSuccess,
                                MoveTemp(Metadata),
                                MoveTemp(Manifest),
                                MoveTemp(Error));
                        }
                    });
            });

    if (!bQueued)
    {
        OnBuiltMetadataLoaded(
            RequestSerial,
            ExpectedReference,
            UUID,
            Reader,
            false,
            FGWorldModelMetadata(),
            FGWorldModelManifest(),
            TEXT(".gworld metadata worker queue is shutting down"));
    }
}

void AStaticActor::OnBuiltMetadataLoaded(
    const uint64 RequestSerial,
    const FString& ExpectedReference,
    const FGuid& UUID,
    const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe>& Reader,
    const bool bSuccess,
    FGWorldModelMetadata&& Metadata,
    FGWorldModelManifest&& Manifest,
    FString&& Error)
{
    check(IsInGameThread());
    if (bIsDestroyed
        || RequestSerial != MetadataRequestSerial
        || ModelReference != ExpectedReference)
    {
        return;
    }
    if (!bSuccess || !Reader.IsValid())
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        WriteLogAsync(FString::Printf(
            TEXT("Static metadata range rejected. Reference=%s Reason=%s"),
            *ModelReference, *Error));
        return;
    }

    // Keep the archive table intact until the prepared facade has validated it. Only the tiny
    // bounds row is copied here so an out-of-range actor can be rejected before any UObject setup.
    ModelMetadata = Metadata.SceneData.ModelData;
    bHasModelMetadata = true;
    LoadingStatus = 0.5f;

    // The outer streamer checks the compact archive summary first. Repeat the exact metadata
    // bounds check here because this deferred actor may have moved while its ranges were read.
    if (!IsPlayerInsideModelRange())
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        return;
    }

    // Publish the tables that the worker already read. This deliberately avoids Resolve() here:
    // static world streaming does not need definition JSON/bone aliases, and synchronously loading
    // those rows plus metadata/manifest again was a major game-thread startup hitch.
    FString ResolveError;
    BakedAsset = FRuntimeModelResolver::LoadAssetFromPreparedTables(
        UUID,
        ModelReference,
        Reader,
        MoveTemp(Metadata),
        MoveTemp(Manifest),
        ResolveError);
    if (!IsValid(BakedAsset))
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        WriteLogAsync(FString::Printf(
            TEXT("Baked scene facade initialization failed: %s"),
            *ResolveError));
        return;
    }


    // The facade only consumes the complete node-transform/manifest view. Transfer the scene
    // placement maps afterwards so validation sees the exact table read from disk and no second
    // copy of these potentially huge maps is retained.
    AllNodeMap = MoveTemp(Metadata.SceneData.NodeMap);
    WaterNodeMap = MoveTemp(Metadata.SceneData.WaterNodeMap);
    AllMeshMap = MoveTemp(Metadata.SceneData.MeshMap);
    ModelMetadata = MoveTemp(Metadata.SceneData.ModelData);

    BeginBuildInstancedMeshActors();
}

void AStaticActor::CancelActiveStreamActions()
{
    check(IsInGameThread());
    for (TPair<FName, TObjectPtr<UWorldSceneStreamAction>>& Pair :
        ActiveStreamActions)
    {
        if (IsValid(Pair.Value))
        {
            Pair.Value->CancelAndRelease();
        }
    }
    ActiveStreamActions.Empty();
    PendingStreamGroupNames.Empty();
    PendingStreamGroupIndex = 0;
    bPendingWaterStream = false;
}

void AStaticActor::ReleaseStreamingResources()
{
    check(IsInGameThread());
    ReleaseInstancedMeshActors();
    for (TPair<FName, TObjectPtr<AWaterActor>>& Pair : WaterActorMap)
    {
        if (IsValid(Pair.Value))
        {
            Pair.Value->Destroy();
        }
    }
    WaterActorMap.Empty();
    LoadedWaterNodes.Empty();
}

void AStaticActor::BeginBuildInstancedMeshActors()
{
    check(IsInGameThread());
    ReleaseInstancedMeshActors();
    PendingInstancedGroups.Empty();
    PendingInstancedGroupNames.Empty();
    PendingInstancedGroupIndex = 0;

    // Grouping can touch hundreds of thousands of node rows but does not need a UObject. Move the
    // scene-wide node map into detached worker-owned storage instead of copying it, and leave only a
    // compact mesh->group lookup prepared on the game thread.
    TSharedRef<StaticActorPrivate::FStaticGroupingWork, ESPMode::ThreadSafe> Work =
        MakeShared<StaticActorPrivate::FStaticGroupingWork, ESPMode::ThreadSafe>();
    Work->Nodes = MoveTemp(AllNodeMap);
    Work->MeshToGroup.Reserve(AllMeshMap.Num());
    for (const TPair<FName, FModelMeshData>& Pair : AllMeshMap)
    {
        const FModelMeshData& Mesh = Pair.Value;
        const bool bHasRuntimeMesh = Mesh.LOD0 != INDEX_NONE
            || Mesh.LOD1 != INDEX_NONE
            || Mesh.LOD2 != INDEX_NONE
            || Mesh.LOD3 != INDEX_NONE;
        if (!Pair.Key.IsNone() && bHasRuntimeMesh && !Mesh.Size.ContainsNaN())
        {
            Work->MeshToGroup.Add(Pair.Key, StaticActorPrivate::MakeMeshGroupName(Mesh));
        }
    }

    const uint64 RequestSerial = MetadataRequestSerial;
    const FString ExpectedReference = ModelReference;
    TWeakObjectPtr<AStaticActor> WeakThis(this);
    const bool bQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, Work, RequestSerial, ExpectedReference]() mutable
        {
            StaticActorPrivate::BuildStaticGroups(*Work);
            FSafeFileIO::DispatchTrackedGameThread(
                [WeakThis, Work, RequestSerial, ExpectedReference]() mutable
                {
                    if (AStaticActor* StrongThis = WeakThis.Get())
                    {
                        StrongThis->OnInstancedMeshGroupsPrepared(
                            RequestSerial,
                            ExpectedReference,
                            MoveTemp(Work->Groups),
                            Work->InvalidNodeCount);
                    }
                });
        });

    if (!bQueued)
    {
        // Shutdown/drain refuses new worker tasks. Do not move an O(N) grouping/index build back to
        // GameThread; invalidate this request and let teardown release the detached native work.
        ++MetadataRequestSerial;
        bAsyncLoading = false;
        UE_LOG(LogTemp, Verbose, TEXT("Static grouping skipped because the worker queue is shutting down."));
    }
}

void AStaticActor::OnInstancedMeshGroupsPrepared(
    const uint64 RequestSerial,
    const FString& ExpectedReference,
    TMap<FName, FInstancedMeshGroupInitData>&& Groups,
    const int32 InvalidNodeCount)
{
    check(IsInGameThread());
    if (bIsDestroyed
        || RequestSerial != MetadataRequestSerial
        || ModelReference != ExpectedReference)
    {
        return;
    }

    PendingInstancedGroups = MoveTemp(Groups);
    PendingInstancedGroupNames.Reset();
    PendingInstancedGroups.GetKeys(PendingInstancedGroupNames);
    PendingInstancedGroupNames.Sort([](const FName A, const FName B)
    {
        return A.LexicalLess(B);
    });
    PendingInstancedGroupIndex = 0;

    if (InvalidNodeCount > 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Removed %d invalid static node(s) while grouping. Static=%s"),
            InvalidNodeCount, *ModelReference);
    }

    if (PendingInstancedGroupNames.IsEmpty())
    {
        FinishBuildInstancedMeshActors();
        return;
    }

    LoadingStatus = FMath::Max(LoadingStatus, 0.50f);
    BuildInstancedMeshActorsStep();
}

void AStaticActor::BuildInstancedMeshActorsStep()
{
    check(IsInGameThread());
    if (bIsDestroyed)
    {
        PendingInstancedGroups.Empty();
        PendingInstancedGroupNames.Empty();
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        FinishBuildInstancedMeshActors();
        return;
    }

    int32 SpawnBudget = 2;
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        if (const UGameSettings* Settings = Manager->GetGameSettings())
        {
            SpawnBudget = Settings->GetStreamingSceneSpawnBudget();
        }
    }

    const int32 EndIndex = FMath::Min(
        PendingInstancedGroupIndex + FMath::Max(1, SpawnBudget),
        PendingInstancedGroupNames.Num());
    for (; PendingInstancedGroupIndex < EndIndex; ++PendingInstancedGroupIndex)
    {
        const FName GroupName = PendingInstancedGroupNames[PendingInstancedGroupIndex];
        FInstancedMeshGroupInitData* GroupData = PendingInstancedGroups.Find(GroupName);
        if (!GroupData)
        {
            continue;
        }

        FInstancedMeshGroupInitData PreparedGroup = MoveTemp(*GroupData);
        PendingInstancedGroups.Remove(GroupName);

        FActorSpawnParameters Parameters;
        Parameters.Owner = this;
        Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AInstancedMeshActor* InstancedActor =
            FActorHelper::SpawnActorDeferred<AInstancedMeshActor>(
                World, AInstancedMeshActor::StaticClass(), GetActorTransform(), Parameters);
        const bool bInitialized = IsValid(InstancedActor)
            && InstancedActor->InitializeGroup(GroupName, MoveTemp(PreparedGroup));
        if (IsValid(InstancedActor))
        {
            InstancedActor->FinishSpawning(GetActorTransform());
        }
        if (!bInitialized
            || !IsValid(InstancedActor)
            || !InstancedActor->AttachToActor(
                this, FAttachmentTransformRules::KeepWorldTransform))
        {
            if (IsValid(InstancedActor))
            {
                InstancedActor->Destroy();
            }
            continue;
        }
        OwnedInstancedMeshActors.Add(GroupName, InstancedActor);
    }

    const float GroupFraction = PendingInstancedGroupNames.IsEmpty()
        ? 1.0f
        : static_cast<float>(PendingInstancedGroupIndex)
            / static_cast<float>(PendingInstancedGroupNames.Num());
    LoadingStatus = FMath::Max(LoadingStatus, FMath::Lerp(0.50f, 0.60f, GroupFraction));

    if (PendingInstancedGroupIndex < PendingInstancedGroupNames.Num())
    {
        World->GetTimerManager().SetTimerForNextTick(
            this, &AStaticActor::BuildInstancedMeshActorsStep);
        return;
    }
    FinishBuildInstancedMeshActors();
}

void AStaticActor::FinishBuildInstancedMeshActors()
{
    check(IsInGameThread());
    PendingInstancedGroups.Empty();
    PendingInstancedGroupNames.Empty();
    PendingInstancedGroupIndex = 0;

    // Node rows are now owned by their mesh-group actors. Discard the duplicate scene-wide map.
    AllNodeMap.Empty();
    PruneUnreferencedMeshMetadata();

    if (!OwnedInstancedMeshActors.IsEmpty() || !WaterNodeMap.IsEmpty())
    {
        bAsyncLoading = false;
        StartStreaming();
    }
    else
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
    }
}

void AStaticActor::ReleaseInstancedMeshActors()
{
    check(IsInGameThread());
    for (TPair<FName, TObjectPtr<AInstancedMeshActor>>& Pair :
        OwnedInstancedMeshActors)
    {
        if (AInstancedMeshActor* Actor = Pair.Value.Get(); IsValid(Actor))
        {
            Actor->ReleaseRuntimeResources();
            Actor->Destroy();
        }
    }
    OwnedInstancedMeshActors.Empty();
}

void AStaticActor::RemoveInstancedMeshGroup(const FName GroupName)
{
    check(IsInGameThread());
    if (TObjectPtr<AInstancedMeshActor>* Found =
            OwnedInstancedMeshActors.Find(GroupName))
    {
        if (IsValid(Found->Get()))
        {
            Found->Get()->ReleaseRuntimeResources();
            Found->Get()->Destroy();
        }
    }
    OwnedInstancedMeshActors.Remove(GroupName);
    PruneUnreferencedMeshMetadata();
}

void AStaticActor::PruneUnreferencedMeshMetadata()
{
    check(IsInGameThread());
    // Group identity is a pure function of FModelMeshData. Avoid re-walking every node stored by
    // every instanced actor after grouping; dense scenes can contain orders of magnitude more nodes
    // than mesh rows. Keeping an unused mesh row that shares a live group's signature is harmless,
    // while removing rows whose group no longer exists is sufficient for streaming correctness.
    for (auto It = AllMeshMap.CreateIterator(); It; ++It)
    {
        if (!OwnedInstancedMeshActors.Contains(
                StaticActorPrivate::MakeMeshGroupName(It.Value())))
        {
            It.RemoveCurrent();
        }
    }
}

void AStaticActor::OnStreamProgress(
    const FName GroupName,
    const float Progress)
{
    check(IsInGameThread());
    StreamGroupProgress.FindOrAdd(GroupName) =
        FMath::Clamp(Progress, 0.0f, 1.0f);
    float Sum = 0.0f;
    for (const TPair<FName, float>& Pair : StreamGroupProgress)
    {
        Sum += Pair.Value;
    }
    const float BatchProgress = StreamGroupProgress.IsEmpty()
        ? 1.0f
        : Sum / static_cast<float>(StreamGroupProgress.Num());
    LoadingStatus = FMath::Max(
        LoadingStatus,
        FMath::Clamp(0.5f + BatchProgress * 0.5f, 0.5f, 1.0f));
}

bool AStaticActor::IsPlayerInsideModelRange() const
{
    check(IsInGameThread());
    if (!bHasModelMetadata || ModelMetadata.Size.IsNearlyZero(0.001f))
    {
        return true;
    }

    FVector PlayerLocation = FVector::ZeroVector;
    if (UGameManagerSubSystem* Manager =
            UGameManagerSubSystem::GetSubSystem(
                const_cast<AStaticActor*>(this)))
    {
        PlayerLocation = Manager->GetPlayerLocation();
    }
    float DistanceMultiplier = 64.0f;
    if (UGameManagerSubSystem* Manager =
            UGameManagerSubSystem::GetSubSystem(const_cast<AStaticActor*>(this)))
    {
        if (const UGameSettings* Settings = Manager->GetGameSettings())
        {
            DistanceMultiplier = Settings->GetEffectiveStreamingDistanceMultiplier();
        }
    }
    const float Radius = FMath::Max3(
        ModelMetadata.Size.X,
        ModelMetadata.Size.Y,
        ModelMetadata.Size.Z) * FMath::Max(1.0f, DistanceMultiplier);
    const FVector WorldCenter =
        GetActorTransform().TransformPosition(ModelMetadata.Center);
    return FVector::DistSquared(PlayerLocation, WorldCenter)
        <= FMath::Square(FMath::Max(1.0f, Radius));
}

void AStaticActor::WriteLogAsync(const FString& Message) const
{
    UFileFunctionLibrary::WriteSimulatorLogAsync(
        TEXT("StaticActor"), Message);
}

void AStaticActor::StartStreaming()
{
    check(IsInGameThread());
    if (OwnedInstancedMeshActors.IsEmpty() && WaterNodeMap.IsEmpty())
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        return;
    }
    RegisterGameUpdate();
    StartStreamingStep();
}

void AStaticActor::RegisterGameUpdate()
{
    check(IsInGameThread());
    if (GameUpdateTickHandle != INDEX_NONE)
    {
        return;
    }
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [WeakThis = TWeakObjectPtr<AStaticActor>(this)](float DeltaSeconds)
            {
                if (AStaticActor* StrongThis = WeakThis.Get())
                {
                    StrongThis->UpdateStreaming(DeltaSeconds);
                }
            },
            15);
    }
}

void AStaticActor::UnregisterGameUpdate()
{
    check(IsInGameThread());
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
    }
    GameUpdateTickHandle = INDEX_NONE;
}

void AStaticActor::UpdateStreaming(float /*DeltaSeconds*/)
{
    check(IsInGameThread());
    StartStreamingStep();
}

FglTFRuntimeStaticMeshConfig AStaticActor::BuildStreamingMeshConfig()
{
    check(IsInGameThread());
    FglTFRuntimeStaticMeshConfig Config;
    // The RuntimeLOD finalizer receives fully reconstructed materials and textures from .dat.
    // Parser caches would only retain transient build products beyond their streaming lifetime.
    Config.CacheMode = EglTFRuntimeCacheMode::None;
    Config.MaterialsConfig.CacheMode = EglTFRuntimeCacheMode::None;
    Config.CollisionComplexity = ECollisionTraceFlag::CTF_UseComplexAsSimple;
    if (UGameManagerSubSystem* Manager =
            UGameManagerSubSystem::GetSubSystem(this))
    {
        glTFMaterialOverrideUtils::ApplyOverrides(
            Manager->GetMaterialDefaultReferences(), Config.MaterialsConfig);
    }
    Config.MaterialsConfig.bGeneratesMipMaps = false;
    Config.MaterialsConfig.bLoadMipMaps = true;
    Config.MaterialsConfig.ImagesConfig.bStreaming = false;
    Config.MaterialsConfig.SpecularFactor = 0.0f;
    const int32 TextureLimit = UGameSettings::ResolveMaxTextureResolution(this);
    Config.MaterialsConfig.ImagesConfig.MaxWidth = TextureLimit;
    Config.MaterialsConfig.ImagesConfig.MaxHeight = TextureLimit;
    Config.Outer = nullptr; // The stream action supplies a world-aware transient outer.
    // Runtime world streaming should not retain a CPU vertex copy for every visual mesh. The
    // stream action enables CPU access only for groups that actually request complex collision.
    Config.bAllowCPUAccess = false;
    // Runtime Lumen-card generation serializes expensive render-data work and is a major source of
    // long hitches on large worlds. Dynamic runtime meshes remain visible to normal surface/cache
    // paths without eagerly generating cards for every streamed group.
    Config.bBuildLumenCards = false;
    Config.bBuildNavCollision = !bRenderOnlyStreaming;
    if (bRenderOnlyStreaming)
    {
        Config.CollisionComplexity = ECollisionTraceFlag::CTF_UseDefault;
        Config.bBuildComplexCollision = false;
        Config.bBuildSimpleCollision = false;
    }
    return Config;
}

void AStaticActor::StartStreamingStep()
{
    check(IsInGameThread());
    if (bIsDestroyed)
    {
        bAsyncLoading = false;
        UnregisterGameUpdate();
        return;
    }
    if (!IsValid(BakedAsset))
    {
        bAsyncLoading = false;
        bIsLoaded = true;
        LoadingStatus = 1.0f;
        UnregisterGameUpdate();
        WriteLogAsync(FString::Printf(
            TEXT("Baked scene facade became invalid: %s"), *ModelReference));
        return;
    }
    if (OwnedInstancedMeshActors.IsEmpty() && WaterNodeMap.IsEmpty())
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        UnregisterGameUpdate();
        return;
    }
    if (bAsyncLoading)
    {
        return;
    }

    // Begin one streaming evaluation cycle. Creating/activating hundreds of async actions in one
    // frame caused visible stalls on dense models, so group activation itself is now budgeted.
    bAsyncLoading = true;
    bIsLoaded = false;
    PendingStreamGroupNames.Empty();
    OwnedInstancedMeshActors.GetKeys(PendingStreamGroupNames);
    PendingStreamGroupNames.Sort([](const FName A, const FName B)
    {
        return A.LexicalLess(B);
    });
    PendingStreamGroupIndex = 0;
    bPendingWaterStream = !WaterNodeMap.IsEmpty();
    StreamGroupProgress.Empty();
    for (const FName GroupName : PendingStreamGroupNames)
    {
        StreamGroupProgress.Add(GroupName, 0.0f);
    }
    if (bPendingWaterStream)
    {
        StreamGroupProgress.Add(NAME_None, 0.0f);
    }
    LaunchNextStreamingBatch();
}

void AStaticActor::LaunchNextStreamingBatch()
{
    check(IsInGameThread());
    if (bIsDestroyed)
    {
        bAsyncLoading = false;
        PendingStreamGroupNames.Empty();
        PendingStreamGroupIndex = 0;
        bPendingWaterStream = false;
        return;
    }
    if (!bAsyncLoading)
    {
        return;
    }

    FVector PlayerLocation = FVector::ZeroVector;
    int32 SafeChunkSize = FMath::Max(1, ChunkSize);
    // Keep the number of decoded-but-not-yet-finalized bundles bounded by the same limit that
    // protects glTFRuntime's native mesh finalizers. The old scene-spawn budget could be 32, which
    // allowed dozens of texture/material bundles to become resident while only a handful could
    // enter the native finalizer, increasing RAM without improving throughput.
    int32 GroupBudget = FglTFRuntimeSafety::GetMeshBuildConcurrencyLimit();
    float UnloadDistanceMultiplier = 1.10f;
    float EffectiveDistanceMultiplier = FMath::Max(1.0f, StreamDistance + 1.0f);
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        PlayerLocation = Manager->GetPlayerLocation();
        if (const UGameSettings* Settings = Manager->GetGameSettings())
        {
            SafeChunkSize = FMath::Min(
                SafeChunkSize, Settings->GetStreamingNodeBudgetPerFrame());
            GroupBudget = FMath::Min(
                GroupBudget, Settings->GetStreamingSceneSpawnBudget());
            UnloadDistanceMultiplier = Settings->GetStreamingUnloadDistanceMultiplier();
            EffectiveDistanceMultiplier = Settings->GetEffectiveStreamingDistanceMultiplier();
        }
    }
    GroupBudget = FMath::Clamp(GroupBudget, 1, 16);
    const int32 AvailableSlots = FMath::Max(
        0, GroupBudget - ActiveStreamActions.Num());

    // UWorldSceneStreamAction's historical formula is MeshSize + MeshSize * Distance, so pass
    // multiplier-1 to make the setting itself represent the intuitive final size multiplier.
    const float EffectiveStreamDistance = FMath::Max(0.0f, EffectiveDistanceMultiplier - 1.0f);
    const FglTFRuntimeStaticMeshConfig MeshConfig = BuildStreamingMeshConfig();

    const auto StartGroup =
        [this, &PlayerLocation, &MeshConfig, SafeChunkSize, EffectiveStreamDistance, UnloadDistanceMultiplier](
            AInstancedMeshActor* InstancedActor,
            const bool bWaterGroup)
        {
            const FName GroupName = bWaterGroup
                ? NAME_None : InstancedActor->GetGroupName();
            UWorldSceneStreamAction* Action =
                UWorldSceneStreamAction::StreamAsync(
                    this,
                    this,
                    InstancedActor,
                    PlayerLocation,
                    MeshConfig,
                    EffectiveStreamDistance,
                    SafeChunkSize,
                    bRenderOnlyStreaming,
                    bWaterGroup,
                    UnloadDistanceMultiplier);
            if (!IsValid(Action))
            {
                return false;
            }
            ActiveStreamActions.Add(GroupName, Action);
            StreamGroupProgress.FindOrAdd(GroupName) = 0.0f;
            Action->Completed.AddDynamic(
                this, &AStaticActor::OnStreamCompleted);
            Action->Progress.AddDynamic(
                this, &AStaticActor::OnStreamProgress);
            Action->Activate();
            return true;
        };

    int32 LaunchedThisBatch = 0;
    TArray<FName> FailedGroups;
    while (LaunchedThisBatch < AvailableSlots
        && PendingStreamGroupIndex < PendingStreamGroupNames.Num())
    {
        const FName GroupName = PendingStreamGroupNames[PendingStreamGroupIndex++];
        TObjectPtr<AInstancedMeshActor>* Found = OwnedInstancedMeshActors.Find(GroupName);
        AInstancedMeshActor* InstancedActor = Found ? Found->Get() : nullptr;
        if (!IsValid(InstancedActor)
            || InstancedActor->GetOwner() != this
            || !StartGroup(InstancedActor, false))
        {
            FailedGroups.Add(GroupName);
            StreamGroupProgress.FindOrAdd(GroupName) = 1.0f;
            continue;
        }
        ++LaunchedThisBatch;
    }
    for (const FName GroupName : FailedGroups)
    {
        RemoveInstancedMeshGroup(GroupName);
    }

    if (LaunchedThisBatch < AvailableSlots && bPendingWaterStream)
    {
        bPendingWaterStream = false;
        if (!StartGroup(nullptr, true))
        {
            WaterNodeMap.Empty();
            LoadedWaterNodes.Empty();
            WaterActorMap.Empty();
            StreamGroupProgress.FindOrAdd(NAME_None) = 1.0f;
        }
        else
        {
            ++LaunchedThisBatch;
        }
    }

    if (ActiveStreamActions.IsEmpty())
    {
        if (PendingStreamGroupIndex < PendingStreamGroupNames.Num() || bPendingWaterStream)
        {
            if (UWorld* World = GetWorld())
            {
                World->GetTimerManager().SetTimerForNextTick(
                    this, &AStaticActor::LaunchNextStreamingBatch);
            }
            else
            {
                LaunchNextStreamingBatch();
            }
            return;
        }
        FinishStreamingCycle();
    }
}

void AStaticActor::FinishStreamingCycle()
{
    check(IsInGameThread());
    PendingStreamGroupNames.Empty();
    PendingStreamGroupIndex = 0;
    bPendingWaterStream = false;
    bIsLoaded = true;
    bAsyncLoading = false;
    LoadingStatus = 1.0f;
    StreamGroupProgress.Empty();
}

void AStaticActor::OnStreamCompleted(
    const FWorldSceneStreamResult& Result)
{
    check(IsInGameThread());
    ActiveStreamActions.Remove(Result.GroupName);
    StreamGroupProgress.FindOrAdd(Result.GroupName) = 1.0f;
    if (bIsDestroyed)
    {
        return;
    }
    if (Result.bWaterGroup)
    {
        WaterNodeMap = Result.WaterNodeMap;
        LoadedWaterNodes = Result.LoadedWaterNodes;
        WaterActorMap = Result.WaterActorMap;
    }
    else if (Result.bGroupFailed)
    {
        RemoveInstancedMeshGroup(Result.GroupName);
        WriteLogAsync(FString::Printf(
            TEXT("Failed mesh group isolated. Static=%s Mesh=%s"),
            *ModelReference, *Result.GroupName.ToString()));
    }
    const bool bHasPendingGroups =
        PendingStreamGroupIndex < PendingStreamGroupNames.Num() || bPendingWaterStream;
    if (bHasPendingGroups)
    {
        // Back-fill a completed slot on the next game-thread tick instead of waiting for every
        // action in the current wave. Deferring one tick avoids re-entrant Activate()/Completed()
        // chains for empty groups while keeping archive I/O and native finalizers continuously fed.
        if (UWorld* World = GetWorld())
        {
            World->GetTimerManager().SetTimerForNextTick(
                this, &AStaticActor::LaunchNextStreamingBatch);
        }
        else
        {
            LaunchNextStreamingBatch();
        }
        return;
    }
    if (ActiveStreamActions.IsEmpty())
    {
        FinishStreamingCycle();
    }
}
