// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file StaticActor.cpp
 * 역할: 하나의 baked Static 모델을 월드에 표현합니다.
 * 핵심 기능: gworld 참조 초기화, 메타데이터 범위 읽기, 노드·메시 스트리밍.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "Model/StaticActor.h"

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
#include "System/GlTFSimulatorGameInstance.h"
#include "System/GlTFSimulatorAssetRegistry.h"
#include "TimerManager.h"
#include "World/WaterActor.h"

namespace StaticActorPrivate
{
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
            OutReason = TEXT("model is absent from the verified .gwd directory");
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
            OutReason = TEXT("model database returned a non-gwd runtime reference");
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
    PendingFailedGroupNodes.Empty();
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
    PendingFailedGroupNodes.Empty();
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
        Database ? Database->GetArchiveReader() : nullptr;
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
                FString Error;
                const bool bSuccess = Reader->ReadModelMetadata(
                    UUID, Metadata, Error);
                FSafeFileIO::DispatchTrackedGameThread(
                    [WeakThis,
                        RequestSerial,
                        ExpectedReference,
                        bSuccess,
                        Metadata = MoveTemp(Metadata),
                        Error = MoveTemp(Error)]() mutable
                    {
                        if (AStaticActor* StrongThis = WeakThis.Get())
                        {
                            StrongThis->OnBuiltMetadataLoaded(
                                RequestSerial,
                                ExpectedReference,
                                bSuccess,
                                MoveTemp(Metadata),
                                MoveTemp(Error));
                        }
                    });
            });

    if (!bQueued)
    {
        OnBuiltMetadataLoaded(
            RequestSerial,
            ExpectedReference,
            false,
            FGWorldModelMetadata(),
            TEXT(".gwd metadata worker queue is shutting down"));
    }
}

void AStaticActor::OnBuiltMetadataLoaded(
    const uint64 RequestSerial,
    const FString& ExpectedReference,
    const bool bSuccess,
    FGWorldModelMetadata&& Metadata,
    FString&& Error)
{
    check(IsInGameThread());
    if (bIsDestroyed
        || RequestSerial != MetadataRequestSerial
        || ModelReference != ExpectedReference)
    {
        return;
    }
    if (!bSuccess)
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        WriteLogAsync(FString::Printf(
            TEXT("Static metadata range rejected. Reference=%s Reason=%s"),
            *ModelReference, *Error));
        return;
    }

    AllNodeMap = MoveTemp(Metadata.SceneData.NodeMap);
    WaterNodeMap = MoveTemp(Metadata.SceneData.WaterNodeMap);
    AllMeshMap = MoveTemp(Metadata.SceneData.MeshMap);
    ModelMetadata = MoveTemp(Metadata.SceneData.ModelData);
    bHasModelMetadata = true;
    LoadingStatus = 0.5f;

    // The outer streamer checks the compact archive summary first. Repeat the exact metadata
    // bounds check here because this deferred actor may have moved while its range was read.
    if (!IsPlayerInsideModelRange())
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        AllNodeMap.Empty();
        AllMeshMap.Empty();
        WaterNodeMap.Empty();
        return;
    }

    FResolvedRuntimeModel Resolved;
    FString ResolveError;
    if (!FRuntimeModelResolver::Resolve(
            this, ModelReference, Resolved, ResolveError))
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        WriteLogAsync(FString::Printf(
            TEXT("Baked scene facade resolve failed: %s"), *ResolveError));
        return;
    }
    BakedAsset = FRuntimeModelResolver::LoadAssetSynchronously(
        Resolved, ResolveError);
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

    // Construct potentially hundreds of mesh-group actors over multiple frames. The old all-at-once
    // loop was one of the largest hitches when opening a dense baked world.
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
    PendingFailedGroupNodes.Empty();
    PendingInstancedGroupIndex = 0;

    for (auto It = AllNodeMap.CreateIterator(); It; ++It)
    {
        const FModelNodeData& Node = It.Value();
        const FModelMeshData* Mesh = AllMeshMap.Find(Node.MeshName);
        const bool bHasRuntimeMesh = Mesh
            && (Mesh->LOD0 != INDEX_NONE
                || Mesh->LOD1 != INDEX_NONE
                || Mesh->LOD2 != INDEX_NONE
                || Mesh->LOD3 != INDEX_NONE);
        if (It.Key().IsNone()
            || Node.MeshName.IsNone()
            || Node.Transform.ContainsNaN()
            || !bHasRuntimeMesh
            || Mesh->Size.ContainsNaN())
        {
            UE_LOG(LogTemp, Warning,
                TEXT("Invalid static node removed. Static=%s Node=%s Mesh=%s"),
                *ModelReference, *It.Key().ToString(), *Node.MeshName.ToString());
            It.RemoveCurrent();
            continue;
        }
        PendingInstancedGroups.FindOrAdd(
            StaticActorPrivate::MakeMeshGroupName(*Mesh)).Add(It.Key(), Node);
    }

    PendingInstancedGroups.GetKeys(PendingInstancedGroupNames);
    PendingInstancedGroupNames.Sort([](const FName& A, const FName& B)
    {
        return A.ToString() < B.ToString();
    });
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
        PendingFailedGroupNodes.Empty();
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
        TMap<FName, FModelNodeData>* GroupNodes = PendingInstancedGroups.Find(GroupName);
        if (!GroupNodes)
        {
            continue;
        }

        TArray<FName> GroupNodeNames;
        GroupNodes->GetKeys(GroupNodeNames);
        TMap<FName, FModelNodeData> NodesForActor = MoveTemp(*GroupNodes);
        PendingInstancedGroups.Remove(GroupName);

        FActorSpawnParameters Parameters;
        Parameters.Owner = this;
        Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AInstancedMeshActor* InstancedActor =
            FActorHelper::SpawnActorDeferred<AInstancedMeshActor>(
                World, AInstancedMeshActor::StaticClass(), GetActorTransform(), Parameters);
        const bool bInitialized = IsValid(InstancedActor)
            && InstancedActor->InitializeGroup(GroupName, MoveTemp(NodesForActor));
        if (IsValid(InstancedActor))
        {
            InstancedActor->FinishSpawning(GetActorTransform());
        }
        if (!bInitialized
            || !IsValid(InstancedActor)
            || !InstancedActor->AttachToActor(
                this, FAttachmentTransformRules::KeepWorldTransform))
        {
            for (const FName NodeName : GroupNodeNames)
            {
                PendingFailedGroupNodes.Add(NodeName);
            }
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
    for (const FName NodeName : PendingFailedGroupNodes)
    {
        AllNodeMap.Remove(NodeName);
    }
    PendingInstancedGroups.Empty();
    PendingInstancedGroupNames.Empty();
    PendingFailedGroupNodes.Empty();
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
    TSet<FName> ReferencedMeshes;
    for (const TPair<FName, TObjectPtr<AInstancedMeshActor>>& Pair :
        OwnedInstancedMeshActors)
    {
        const AInstancedMeshActor* Actor = Pair.Value.Get();
        if (!IsValid(Actor))
        {
            continue;
        }
        for (const TPair<FName, FModelNodeData>& Node : Actor->GetNodeMapRef())
        {
            ReferencedMeshes.Add(Node.Value.MeshName);
        }
    }
    for (auto It = AllMeshMap.CreateIterator(); It; ++It)
    {
        if (!ReferencedMeshes.Contains(It.Key()))
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
    Config.bAllowCPUAccess = !bRenderOnlyStreaming;
    Config.bBuildLumenCards = true;
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
    PendingStreamGroupNames.Sort([](const FName& A, const FName& B)
    {
        return A.ToString() < B.ToString();
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
    if (!bAsyncLoading || !ActiveStreamActions.IsEmpty())
    {
        return;
    }

    FVector PlayerLocation = FVector::ZeroVector;
    int32 SafeChunkSize = FMath::Max(1, ChunkSize);
    int32 GroupBudget = 2;
    float UnloadDistanceMultiplier = 1.10f;
    float EffectiveDistanceMultiplier = FMath::Max(1.0f, StreamDistance + 1.0f);
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        PlayerLocation = Manager->GetPlayerLocation();
        if (const UGameSettings* Settings = Manager->GetGameSettings())
        {
            SafeChunkSize = FMath::Min(
                SafeChunkSize, Settings->GetStreamingNodeBudgetPerFrame());
            // Reuse the scene-spawn budget for mesh-group action activation: both create bounded
            // game-thread UObject work and benefit from the same user-facing hitch/latency tradeoff.
            GroupBudget = Settings->GetStreamingSceneSpawnBudget();
            UnloadDistanceMultiplier = Settings->GetStreamingUnloadDistanceMultiplier();
            EffectiveDistanceMultiplier = Settings->GetEffectiveStreamingDistanceMultiplier();
        }
    }
    GroupBudget = FMath::Max(1, GroupBudget);

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
    while (LaunchedThisBatch < GroupBudget
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

    if (LaunchedThisBatch < GroupBudget && bPendingWaterStream)
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
        }
        else
        {
            FinishStreamingCycle();
        }
    }
}
