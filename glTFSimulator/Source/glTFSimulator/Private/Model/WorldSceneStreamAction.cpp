// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file WorldSceneStreamAction.cpp
 * 역할: 월드 씬 메시의 비동기 생성 요청을 처리합니다.
 * 핵심 기능: 메시 생성·충돌 최종화, 월드 참조 보호, 완료·취소 통지.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "Model/WorldSceneStreamAction.h"
#include "System/GameManagerSubSystem.h"
#include "System/MacroLibrary.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Components/CapsuleComponent.h"
#include "Model/DynamicPointLightComponent.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Model/StaticActor.h"
#include "Model/InstancedMeshActor.h"
#include "System/StreamingMovementGateSubsystem.h"
#include "System/WorldBakedModelAsset.h"
#include "Misc/ScopeExit.h"
#include "System/glTFRuntimeSafety.h"
#include "TimerManager.h"
#include "UObject/UObjectGlobals.h"
#include "World/WaterActor.h"

namespace
{
    // glTFRuntime has an unresolved startup crash path in LoadStaticMeshLODs. Runtime streaming
    // therefore uses the stable single-mesh API unless a project explicitly opts back in.
    constexpr int32 DefaultRuntimeMultiLOD = 0;

    TAutoConsoleVariable<int32> CVarEnableRuntimeMultiLOD(
        TEXT("gworld.Streaming.EnableRuntimeMultiLOD"),
        DefaultRuntimeMultiLOD,
        TEXT("Enables glTFRuntime multi-LOD mesh assembly. 0 loads one validated LOD (prefers LOD0); 1 enables LOD1-LOD3."),
        ECVF_Default);

    /** Every UObject/component mutation in this action is intentionally game-thread-only. */
    bool EnsureStreamActionGameThread(const TCHAR* FunctionName)
    {
        return ensureMsgf(IsInGameThread(), TEXT("%s must run on the game thread"), FunctionName);
    }

    bool ConfigureGeneratedMeshCollision(
        UInstancedStaticMeshComponent* Component,
        const FMeshData* MeshData,
        const bool bRenderOnly)
    {
        if (!IsValid(Component))
        {
            return false;
        }

        const bool bEnableCollision =
            !bRenderOnly && MeshData && (MeshData->bComplexCollision || MeshData->bSimpleCollision);

        Component->SetGenerateOverlapEvents(false);
        Component->SetCanEverAffectNavigation(bEnableCollision);
        if (!bEnableCollision)
        {
            Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Component->SetCollisionResponseToAllChannels(ECR_Ignore);
            return false;
        }

        Component->SetCollisionProfileName(TEXT("BlockAll"));
        Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        Component->SetCollisionObjectType(ECC_WorldStatic);
        Component->SetCollisionResponseToAllChannels(ECR_Block);
        return true;
    }
}

void UWorldGeneratedStaticMeshContext::Initialize(UWorld* InWorld)
{
    PinnedWorld = nullptr;
    World = InWorld;
}

bool UWorldGeneratedStaticMeshContext::PinWorldForBuild()
{
    UWorld* RuntimeWorld = World.Get();
    if (!IsValid(RuntimeWorld) || !RuntimeWorld->IsGameWorld())
    {
        PinnedWorld = nullptr;
        return false;
    }

    PinnedWorld = RuntimeWorld;
    return true;
}

void UWorldGeneratedStaticMeshContext::ReleaseWorldPin()
{
    PinnedWorld = nullptr;
}

UWorld* UWorldGeneratedStaticMeshContext::GetWorld() const
{
    if (UWorld* ActiveWorld = PinnedWorld.Get())
    {
        return ActiveWorld;
    }

    return World.Get();
}

UWorldSceneStreamAction *UWorldSceneStreamAction::StreamAsync(
    UObject *WorldContextObject,
    AStaticActor *Actor,
    AInstancedMeshActor *InMeshActor,
    const FVector &InPlayerLocation,
    const FglTFRuntimeStaticMeshConfig &StaticMeshConfig,
    float InDistance,
    int32 InChunkSize,
    bool bInRenderOnly,
    bool bInWaterGroup,
    float InUnloadDistanceMultiplier)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::StreamAsync")))
    {
        return nullptr;
    }

    if (!IsValid(Actor)
        || (bInWaterGroup ? IsValid(InMeshActor) : !IsValid(InMeshActor))
        || (!bInWaterGroup && InMeshActor->GetOwner() != Actor))
    {
        return nullptr;
    }

    auto *Action = NewObject<UWorldSceneStreamAction>();
    Action->WorldContextObject = WorldContextObject;
    UWorld* RuntimeWorld = IsValid(Actor)
        ? Actor->GetWorld()
        : (IsValid(WorldContextObject) ? WorldContextObject->GetWorld() : nullptr);
    if (IsValid(RuntimeWorld) && RuntimeWorld->IsGameWorld())
    {
        UWorldGeneratedStaticMeshContext* MeshWorldContext =
            NewObject<UWorldGeneratedStaticMeshContext>(GetTransientPackage(), NAME_None, RF_Transient);
        if (IsValid(MeshWorldContext))
        {
            MeshWorldContext->Initialize(RuntimeWorld);
            Action->GeneratedMeshWorldContext = MeshWorldContext;
        }
    }

    Action->OwnerActor = Actor;
    Action->MeshActor = InMeshActor;
    Action->DecalLight = Actor->GetDecalLight();
    Action->Asset = Actor->GetBakedAsset();
    Action->WaterClass = Actor->GetWaterClass();
    Action->bWaterGroup = bInWaterGroup;
    if (bInWaterGroup)
    {
        Action->GroupName = NAME_None;
        Action->WaterNodeMap = Actor->GetWaterNodeMapRef();
        Action->LoadedWaterNodes = Actor->GetLoadedWaterNodesRef();
        Action->WaterActorMap = Actor->GetWaterActorMapRef();
    }
    else
    {
        Action->GroupName = InMeshActor->GetGroupName();
        float MaxWorldRadius = 0.0f;
        for (const FName MeshName : InMeshActor->GetReferencedMeshNames())
        {
            const FModelMeshData* MeshData = Actor->GetAllMeshMapRef().Find(MeshName);
            if (!MeshData) return nullptr;
            Action->MeshMap.Add(MeshName, *MeshData);
            const float MeshSize = MeshData->Size.Size();
            MaxWorldRadius = FMath::Max(MaxWorldRadius, MeshSize + MeshSize * FMath::Max(0.0f, InDistance));
        }
        InMeshActor->BuildStreamNodeSnapshot(InPlayerLocation, Actor->GetActorTransform(),
            MaxWorldRadius, Action->NodeMap);
    }
    Action->PlayerLocation = InPlayerLocation;
    Action->Distance = InDistance;
    Action->UnloadDistanceMultiplier = FMath::Clamp(InUnloadDistanceMultiplier, 1.0f, 2.0f);
    Action->ChunkSize = InChunkSize;
    Action->StaticMeshConfig = StaticMeshConfig;
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(WorldContextObject))
    {
        Action->MaterialReferenceGuard = GameManager->AcquireMaterialDefaultReferenceGuard();
    }
    Action->bRenderOnly = bInRenderOnly;
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}


void UWorldSceneStreamAction::CancelAndRelease()
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::CancelAndRelease")))
    {
        return;
    }

    AbortAndRelease();
}

void UWorldSceneStreamAction::AbortAndRelease(UStaticMesh* OrphanedMesh)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::AbortAndRelease")))
    {
        return;
    }

    bAbortRequested = true;

    UWorld* World = nullptr;
    if (IsValid(OwnerActor))
    {
        World = OwnerActor->GetWorld();
    }
    else if (IsValid(WorldContextObject))
    {
        World = WorldContextObject->GetWorld();
    }

    if (World)
    {
        World->GetTimerManager().ClearTimer(ProcessTimerHandle);
    }

    Progress.Clear();
    Completed.Clear();

    // Do not release the facade/action while its .dat read or mesh finalizer is still active.
    // SetStaticMesh receives the terminal callback and completes deferred cleanup.
    if (bStaticMeshLoadInFlight && OrphanedMesh == nullptr)
    {
        OwnerActor = nullptr;
        WorldContextObject = nullptr;
        return;
    }

    bStaticMeshLoadInFlight = false;
    if (IsValid(OrphanedMesh) && !OrphanedMesh->IsAsset())
    {
        OrphanedMesh->ClearFlags(RF_Public | RF_Standalone);
    }

    NodeMap.Empty();
    WaterNodeMap.Empty();
    MeshMap.Empty();
    LoadedWaterNodes.Empty();
    WaterActorMap.Empty();
    PendingLoadNodes.Empty();
    PendingUnloadNodes.Empty();
    PendingLoadWaterNodes.Empty();
    PendingUnloadWaterNodes.Empty();
    TotalOperationCount = 0;
    TotalSkippedOperationCount = 0;
    CurrentSkippedOperationIndex = 0;
    SkippedProgressChunkSize = 1;

    Asset = nullptr;
    OwnerActor = nullptr;
    MeshActor = nullptr;
    WorldContextObject = nullptr;
    MaterialReferenceGuard = nullptr;
    if (IsValid(GeneratedMeshWorldContext))
    {
        GeneratedMeshWorldContext->ReleaseWorldPin();
    }
    GeneratedMeshWorldContext = nullptr;
    DecalLight = nullptr;
    CurrentLoadingNode = NAME_None;
    CurrentLoadingMesh = NAME_None;
    bIsLoading = false;
    bRenderOnly = false;
    SetReadyToDestroy();
}

void UWorldSceneStreamAction::Activate()
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::Activate")))
    {
        return;
    }

    bAbortRequested = false;
    bGroupFailed = false;
    bStaticMeshLoadInFlight = false;

    if (!IsValid(OwnerActor) || !IsValid(WorldContextObject)
        || (!bWaterGroup && !IsValid(MeshActor)))
    {
        AbortAndRelease();
        return;
    }

    // Preserve the original node count before sanitization. Invalid, already-correct, and
    // distance-skipped nodes are still completed loading work and remain in the denominator.
    const int32 OriginalNodeWorkCount = NodeMap.Num() + WaterNodeMap.Num();
    SanitizeRuntimeMaps();
    ChunkSize = FMath::Max(1, FMath::Min(FMath::Max(1, NodeMap.Num() + WaterNodeMap.Num()), ChunkSize));

    PendingLoadNodes.Reset();
    PendingUnloadNodes.Reset();
    PendingLoadWaterNodes.Reset();
    PendingUnloadWaterNodes.Reset();
    PendingLoadNodes.Reserve(ChunkSize);
    PendingUnloadNodes.Reserve(ChunkSize);
    PendingLoadWaterNodes.Reserve(FMath::Min(ChunkSize, WaterNodeMap.Num()));
    PendingUnloadWaterNodes.Reserve(FMath::Min(ChunkSize, WaterNodeMap.Num()));

    // These checks are a few scalar operations each. A direct map walk avoids copying both maps,
    // dispatching task-graph jobs, and serializing every result through one mutex.
    for (const TPair<FName, FModelNodeData>& NodePair : NodeMap)
    {
        const FModelNodeData& Info = NodePair.Value;

        const FModelMeshData* MeshPtr = MeshMap.Find(Info.MeshName);
        if (!MeshPtr)
        {
            continue;
        }

        const float MeshSize = MeshPtr->Size.Size();
        const float LoadRadius = MeshSize + (MeshSize * Distance);
        const float LoadRadiusSq = FMath::Square(LoadRadius);
        const float UnloadRadiusSq = FMath::Square(
            LoadRadius * FMath::Max(1.0f, UnloadDistanceMultiplier));

        const FVector NodeWorldLocation =
            (Info.Transform * OwnerActor->GetActorTransform()).GetLocation();
        const float CurrentDist = FVector::DistSquared(PlayerLocation, NodeWorldLocation);
        const bool bIsLoaded = IsValid(MeshActor) && MeshActor->IsNodeLoaded(NodePair.Key);

        if (bIsLoaded)
        {
            if (!Info.bAlwaysLoaded && CurrentDist > UnloadRadiusSq)
            {
                PendingUnloadNodes.Add(NodePair.Key);
            }
        }
        else
        {
            if (UWorld* World = OwnerActor->GetWorld())
            {
                if (UStreamingMovementGateSubsystem* Gate = World->GetSubsystem<UStreamingMovementGateSubsystem>())
                {
                    const FVector Extent = (MeshPtr->Size * 0.5f + BOX_BUFFER_SIZE).GetAbs();
                    Gate->SetModelRegionAvailable(OwnerActor.Get(), NodePair.Key,
                        FBox(-Extent, Extent).TransformBy(Info.Transform * OwnerActor->GetActorTransform()), false);
                }
            }
            if ((Info.bAlwaysLoaded || CurrentDist <= LoadRadiusSq) && CurrentLoadingNode != NodePair.Key)
            {
                PendingLoadNodes.Add(NodePair.Key);
            }
        }
    }

    for (const TPair<FName, FWaterStreamNodeData>& WaterPair : WaterNodeMap)
    {
        const FVector WaterWorldLocation =
            (WaterPair.Value.Transform * OwnerActor->GetActorTransform()).GetLocation();
        const float CurrentDist = FVector::DistSquared(PlayerLocation, WaterWorldLocation);
        const float LoadRadiusSq = GetWaterStreamRadiusSq(WaterPair.Value);
        const float UnloadRadiusSq = LoadRadiusSq * FMath::Square(
            FMath::Max(1.0f, UnloadDistanceMultiplier));
        const bool bIsLoaded = LoadedWaterNodes.Contains(WaterPair.Key);

        if (bIsLoaded)
        {
            if (CurrentDist > UnloadRadiusSq)
            {
                PendingUnloadWaterNodes.Add(WaterPair.Key);
            }
        }
        else if (CurrentDist <= LoadRadiusSq)
        {
            PendingLoadWaterNodes.Add(WaterPair.Key);
        }
    }

    CurrentLoadIndex = 0;
    CurrentUnloadIndex = 0;
    CurrentLoadWaterIndex = 0;
    CurrentUnloadWaterIndex = 0;
    const int32 PendingOperationCount = PendingLoadNodes.Num() + PendingUnloadNodes.Num() +
        PendingLoadWaterNodes.Num() + PendingUnloadWaterNodes.Num();
    TotalOperationCount = OriginalNodeWorkCount;
    TotalSkippedOperationCount = FMath::Max(0, TotalOperationCount - PendingOperationCount);
    CurrentSkippedOperationIndex = 0;

    // No-op, invalid, already-correct, and distance-skipped nodes remain visible loading work.
    // Advancing them in a bounded number of next-tick steps lets the UI actually render progress
    // instead of receiving a single jump from the initial value to 100 percent.
    constexpr int32 DesiredSkippedProgressUpdates = 16;
    const int32 SkippedNodesPerUpdate = TotalSkippedOperationCount > 0
        ? (TotalSkippedOperationCount + DesiredSkippedProgressUpdates - 1) / DesiredSkippedProgressUpdates
        : 1;
    SkippedProgressChunkSize = FMath::Max(1, SkippedNodesPerUpdate);

    bIsLoading = false;
    BroadcastProgress();

    ProcessChunk();
}

void UWorldSceneStreamAction::SanitizeRuntimeMaps()
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::SanitizeRuntimeMaps")))
    {
        return;
    }

    for (auto It = NodeMap.CreateIterator(); It; ++It)
    {
        const FModelNodeData& Node = It.Value();
        if (It.Key().IsNone() || Node.MeshName.IsNone() || Node.Transform.ContainsNaN() || !MeshMap.Contains(Node.MeshName))
        {
            It.RemoveCurrent();
        }
    }

    for (auto It = WaterNodeMap.CreateIterator(); It; ++It)
    {
        const FWaterStreamNodeData& Water = It.Value();
        if (It.Key().IsNone() || Water.Transform.ContainsNaN() || !FMath::IsFinite(Water.StreamRadius) || Water.StreamRadius <= 0.0f)
        {
            LoadedWaterNodes.Remove(It.Key());
            if (TObjectPtr<AWaterActor>* WaterActor = WaterActorMap.Find(It.Key()))
            {
                if (IsValid(WaterActor->Get()))
                {
                    WaterActor->Get()->Destroy();
                }
            }
            WaterActorMap.Remove(It.Key());
            It.RemoveCurrent();
        }
    }

    TSet<FName> ReferencedMeshes;
    ReferencedMeshes.Reserve(NodeMap.Num());
    for (const TPair<FName, FModelNodeData>& Pair : NodeMap)
    {
        ReferencedMeshes.Add(Pair.Value.MeshName);
    }

    for (auto It = MeshMap.CreateIterator(); It; ++It)
    {
        const FModelMeshData& Mesh = It.Value();
        const bool bHasRuntimeMesh = Mesh.LOD0 != INDEX_NONE || Mesh.LOD1 != INDEX_NONE
            || Mesh.LOD2 != INDEX_NONE || Mesh.LOD3 != INDEX_NONE;
        if (!ReferencedMeshes.Contains(It.Key()) || !bHasRuntimeMesh || Mesh.Size.ContainsNaN())
        {
            It.RemoveCurrent();
        }
    }

    for (auto It = LoadedWaterNodes.CreateIterator(); It; ++It)
    {
        if (!WaterNodeMap.Contains(*It))
        {
            It.RemoveCurrent();
        }
    }
}

void UWorldSceneStreamAction::ReleaseActionReferences()
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::ReleaseActionReferences")))
    {
        return;
    }

    UWorld* World = IsValid(OwnerActor) ? OwnerActor->GetWorld() : (IsValid(WorldContextObject) ? WorldContextObject->GetWorld() : nullptr);
    if (World)
    {
        World->GetTimerManager().ClearTimer(ProcessTimerHandle);
    }

    Asset = nullptr;
    OwnerActor = nullptr;
    MeshActor = nullptr;
    WorldContextObject = nullptr;
    MaterialReferenceGuard = nullptr;
    if (IsValid(GeneratedMeshWorldContext))
    {
        GeneratedMeshWorldContext->ReleaseWorldPin();
    }
    GeneratedMeshWorldContext = nullptr;
    PendingLoadNodes.Empty();
    PendingUnloadNodes.Empty();
    PendingLoadWaterNodes.Empty();
    PendingUnloadWaterNodes.Empty();
    TotalOperationCount = 0;
    TotalSkippedOperationCount = 0;
    CurrentSkippedOperationIndex = 0;
    SkippedProgressChunkSize = 1;
    NodeMap.Empty();
    WaterNodeMap.Empty();
    MeshMap.Empty();
    LoadedWaterNodes.Empty();
    WaterActorMap.Empty();
    DecalLight = nullptr;
    CurrentLoadingNode = NAME_None;
    CurrentLoadingMesh = NAME_None;
    bIsLoading = false;
    bStaticMeshLoadInFlight = false;
    bAbortRequested = true;
    bRenderOnly = false;
}

void UWorldSceneStreamAction::ProcessChunk()
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::ProcessChunk")))
    {
        return;
    }

    if (bAbortRequested || !IsValid(OwnerActor))
    {
        AbortAndRelease();
        return;
    }

    if (CurrentSkippedOperationIndex < TotalSkippedOperationCount)
    {
        CurrentSkippedOperationIndex = FMath::Min(
            CurrentSkippedOperationIndex + SkippedProgressChunkSize,
            TotalSkippedOperationCount);
        BroadcastProgress();
    }

    const int32 WaterUnloadEnd = FMath::Min(CurrentUnloadWaterIndex + ChunkSize, PendingUnloadWaterNodes.Num());
    for (int32 i = CurrentUnloadWaterIndex; i < WaterUnloadEnd; ++i)
    {
        ProcessUnloadWaterNode(PendingUnloadWaterNodes[i]);
        CurrentUnloadWaterIndex = i + 1;
        BroadcastProgress();
    }

    const int32 UnloadEnd = FMath::Min(CurrentUnloadIndex + ChunkSize, PendingUnloadNodes.Num());
    for (int32 i = CurrentUnloadIndex; i < UnloadEnd; ++i)
    {
        ProcessUnloadNode(PendingUnloadNodes[i]);
        CurrentUnloadIndex = i + 1;
        BroadcastProgress();
    }

    const int32 WaterLoadEnd = FMath::Min(CurrentLoadWaterIndex + ChunkSize, PendingLoadWaterNodes.Num());
    for (int32 i = CurrentLoadWaterIndex; i < WaterLoadEnd; ++i)
    {
        ProcessLoadWaterNode(PendingLoadWaterNodes[i]);
        CurrentLoadWaterIndex = i + 1;
        BroadcastProgress();
    }

    if (!bIsLoading && CurrentLoadIndex < PendingLoadNodes.Num())
    {
        const int32 EndIndex = FMath::Min(CurrentLoadIndex + ChunkSize, PendingLoadNodes.Num());
        for (int32 i = CurrentLoadIndex; i < EndIndex; ++i)
        {
            const FName TargetNode = PendingLoadNodes[i];
            CurrentLoadIndex = i + 1;
            if (IsValid(MeshActor) && MeshActor->IsNodeLoaded(TargetNode))
            {
                BroadcastProgress();
                continue;
            }
            if (ProcessLoadNode(TargetNode))
            {
                // The active node is subtracted by BroadcastProgress until its terminal callback.
                BroadcastProgress();
                break;
            }
            BroadcastProgress();
        }
    }

    if (CurrentSkippedOperationIndex >= TotalSkippedOperationCount &&
        CurrentLoadIndex >= PendingLoadNodes.Num() &&
        CurrentUnloadIndex >= PendingUnloadNodes.Num() &&
        CurrentLoadWaterIndex >= PendingLoadWaterNodes.Num() &&
        CurrentUnloadWaterIndex >= PendingUnloadWaterNodes.Num() &&
        !bIsLoading)
    {
        UWorld *World = OwnerActor->GetWorld();
        if (IsValid(World))
        {
            World->GetTimerManager().ClearTimer(ProcessTimerHandle);
        }
        FWorldSceneStreamResult Result;
        Result.GroupName = GroupName;
        Result.bWaterGroup = bWaterGroup;
        Result.bGroupFailed = bGroupFailed;
        Result.WaterNodeMap = MoveTemp(WaterNodeMap);
        Result.LoadedWaterNodes = MoveTemp(LoadedWaterNodes);
        Result.WaterActorMap = MoveTemp(WaterActorMap);

        Progress.Broadcast(GroupName, 1.0f);
        Completed.Broadcast(Result);
        ReleaseActionReferences();
        SetReadyToDestroy();
    }
    else
    {
        UWorld *World = OwnerActor->GetWorld();
        if (IsValid(World))
        {
            ProcessTimerHandle = World->GetTimerManager().SetTimerForNextTick(
                FTimerDelegate::CreateUObject(this, &UWorldSceneStreamAction::ProcessChunk));
        }
    }
}


float UWorldSceneStreamAction::GetWaterStreamRadiusSq(const FWaterStreamNodeData& Data) const
{
    const float Radius = FMath::Max3(Data.StreamRadius, Distance * 1024.0f, 2048.0f);
    return FMath::Square(Radius);
}

void UWorldSceneStreamAction::ProcessLoadWaterNode(const FName& Name)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::ProcessLoadWaterNode")))
    {
        return;
    }

    if (bAbortRequested || LoadedWaterNodes.Contains(Name) || !IsValid(OwnerActor.Get()))
    {
        return;
    }

    const FWaterStreamNodeData* WaterInfo = WaterNodeMap.Find(Name);
    if (!WaterInfo)
    {
        return;
    }

    UWorld* World = OwnerActor->GetWorld();
    if (!World)
    {
        return;
    }

    FActorSpawnParameters Params;
    Params.Owner = OwnerActor.Get();
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    UClass* WaterSpawnClass = WaterClass ? WaterClass.Get() : AWaterActor::StaticClass();
    AWaterActor* WaterActor = World->SpawnActor<AWaterActor>(WaterSpawnClass, WaterInfo->Transform, Params);
    if (!IsValid(WaterActor) && WaterSpawnClass != AWaterActor::StaticClass())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Configured water actor class failed for streamed ;WATER node; retrying native AWaterActor. Class=%s Node=%s"),
            *GetNameSafe(WaterSpawnClass), *Name.ToString());
        WaterActor = World->SpawnActor<AWaterActor>(AWaterActor::StaticClass(), WaterInfo->Transform, Params);
    }
    if (IsValid(WaterActor))
    {
        if (bRenderOnly)
        {
            WaterActor->SetActorEnableCollision(false);
        }
        WaterActorMap.Emplace(Name, WaterActor);
        LoadedWaterNodes.Emplace(Name);
    }
}

void UWorldSceneStreamAction::ProcessUnloadWaterNode(const FName& Name)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::ProcessUnloadWaterNode")))
    {
        return;
    }

    if (TObjectPtr<AWaterActor>* WaterPtr = WaterActorMap.Find(Name))
    {
        if (IsValid(WaterPtr->Get()))
        {
            WaterPtr->Get()->Destroy();
        }
    }

    WaterActorMap.Remove(Name);
    LoadedWaterNodes.Remove(Name);
}

bool UWorldSceneStreamAction::ProcessLoadNode(const FName &Name)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::ProcessLoadNode")))
    {
        return false;
    }

    if (bAbortRequested)
    {
        return false;
    }

    if (FModelNodeData *Info = NodeMap.Find(Name))
    {
        if (IsValid(MeshActor) && MeshActor->IsNodeLoaded(Name))
        {
            return false;
        }

        if (Info->MeshName.IsNone() || Info->Transform.ContainsNaN() || !MeshMap.Contains(Info->MeshName))
        {
            NodeMap.Remove(Name);
            return false;
        }

        CurrentLoadingNode = Name;
        CurrentLoadingMesh = Info->MeshName;
        bIsLoading = true;

        UInstancedStaticMeshComponent* ISMC = IsValid(MeshActor)
            ? MeshActor->GetMeshComponent()
            : nullptr;
        if (IsValid(ISMC) && IsValid(ISMC->GetStaticMesh()))
        {
            AddTransform(Name);
            // No native load was submitted. Keep walking this chunk so one action can add many
            // instances of its already-loaded shared mesh in the same frame.
            return false;
        }

        LoadStaticMeshAsync(CurrentLoadingMesh);
        return true;
    }

    return false;
}

void UWorldSceneStreamAction::ProcessUnloadNode(const FName &Name)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::ProcessUnloadNode")))
    {
        return;
    }

    if (bAbortRequested || !IsValid(OwnerActor.Get()) || !IsValid(MeshActor.Get()))
    {
        return;
    }

    FModelNodeData *Info = NodeMap.Find(Name);
    if (!Info)
        return;

    DestroyStreamComponents(Name);

    if (!MeshActor->RemoveNodeInstance(Name))
    {
        NodeMap.Empty();
        bGroupFailed = true;
        return;
    }
    if (bRenderOnly)
    {
        return;
    }

    if (const FModelMeshData *Mesh = MeshMap.Find(Info->MeshName))
    {
        if (UWorld* World = OwnerActor->GetWorld())
        {
            if (UStreamingMovementGateSubsystem* Gate = World->GetSubsystem<UStreamingMovementGateSubsystem>())
            {
                const FTransform WorldTransform = Info->Transform * OwnerActor->GetActorTransform();
                const FVector Extent = (Mesh->Size * 0.5f + BOX_BUFFER_SIZE).GetAbs();
                Gate->SetModelRegionAvailable(OwnerActor.Get(), Name,
                    FBox(-Extent, Extent).TransformBy(WorldTransform), false);
            }
        }
    }
}

void UWorldSceneStreamAction::SetStaticMesh(UStaticMesh *StaticMesh)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::SetStaticMesh")))
    {
        return;
    }

    bStaticMeshLoadInFlight = false;
    ON_SCOPE_EXIT
    {
        // Drop the strong world pin only after all callback-side component work is complete.
        if (IsValid(GeneratedMeshWorldContext))
        {
            GeneratedMeshWorldContext->ReleaseWorldPin();
        }
    };
    if (bAbortRequested || !IsValid(OwnerActor) || !IsValid(MeshActor))
    {
        AbortAndRelease(StaticMesh);
        return;
    }
    if (!IsValid(StaticMesh))
    {
        const FString FailedPath = OwnerActor->GetModelReference();
        FglTFRuntimeSafety::ReportRecoverableFailure(
            FailedPath,
            FString::Printf(TEXT("glTFRuntime rejected or failed static mesh '%s' for node '%s'"),
                *CurrentLoadingMesh.ToString(), *CurrentLoadingNode.ToString()));
        NodeMap.Empty();
        bGroupFailed = true;
        ResetLoadState();
        return;
    }

    const FModelMeshData* ModelMeshData = MeshMap.Find(CurrentLoadingMesh);

    // Every instance in this group receives the same mesh object created from its streamed bundle.
    UInstancedStaticMeshComponent* ISMC = MeshActor->AssignStaticMesh(StaticMesh);
    if (IsValid(ISMC))
    {
        ISMC->SetRenderCustomDepth(true);
        ISMC->SetCustomDepthStencilValue(1);
    }

    ConfigureGeneratedMeshCollision(
        ISMC,
        ModelMeshData ? &ModelMeshData->Data : nullptr,
        bRenderOnly);

    if (IsValid(ISMC))
    {
        // AddInstance creates the per-instance body. Avoid a second explicit physics-state rebuild;
        // it can overlap Chaos scene insertion while the generated mesh is being finalized.
        AddTransform(CurrentLoadingNode);
    }
    else
    {
        NodeMap.Empty();
        bGroupFailed = true;
        ResetLoadState();
    }
}

FORCEINLINE float CalculateLODScreenSize(int32 i, int32 N)
{
    if (N <= 1)
        return 0.0f;
    float StartValue = FMath::Min(0.5f + (N * 0.1f), 0.95f);
    float EndValue = 0.3f;
    float Alpha = (float)i / (float)(N - 1);
    return FMath::Lerp(StartValue, EndValue, Alpha);
}

void UWorldSceneStreamAction::LoadStaticMeshAsync(const FName &MeshName)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::LoadStaticMeshAsync")))
    {
        return;
    }

    if (bAbortRequested || !IsValid(Asset))
    {
        AbortAndRelease();
        return;
    }

    UWorldBakedModelAsset* MeshSourceAsset = Asset.Get();
    if (!IsValid(MeshSourceAsset))
    {
        NodeMap.Empty();
        bGroupFailed = true;
        ResetLoadState();
        return;
    }

    FModelMeshData* Mesh = MeshMap.Find(MeshName);
    if (!Mesh)
    {
        bGroupFailed = true;
        NodeMap.Empty();
        ResetLoadState();
        return;
    }

    const int32 RuntimeMeshCount = MeshSourceAsset->GetNumMeshes();
    TArray<int32> LocalIndices;
    LocalIndices.Reserve(4);

    const auto AddValidLOD = [this, &LocalIndices, RuntimeMeshCount, MeshName](
        const int32 MeshIndex,
        const TCHAR* LODLabel)
    {
        if (MeshIndex == INDEX_NONE)
        {
            return;
        }
        if (MeshIndex < 0 || MeshIndex >= RuntimeMeshCount)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("Ignoring invalid runtime mesh index. Mesh=%s Node=%s LOD=%s Index=%d MeshCount=%d"),
                *MeshName.ToString(),
                *CurrentLoadingNode.ToString(),
                LODLabel,
                MeshIndex,
                RuntimeMeshCount);
            return;
        }
        LocalIndices.AddUnique(MeshIndex);
    };

    const bool bEnableRuntimeMultiLOD = CVarEnableRuntimeMultiLOD.GetValueOnGameThread() != 0;
    if (bEnableRuntimeMultiLOD)
    {
        AddValidLOD(Mesh->LOD0, TEXT("LOD0"));
        AddValidLOD(Mesh->LOD1, TEXT("LOD1"));
        AddValidLOD(Mesh->LOD2, TEXT("LOD2"));
        AddValidLOD(Mesh->LOD3, TEXT("LOD3"));
    }
    else
    {
        // Prefer LOD0. A group authored with only a higher LOD tag still needs one render mesh when
        // multi-LOD assembly is disabled, so select its first validated entry.
        AddValidLOD(Mesh->LOD0, TEXT("LOD0"));
        if (LocalIndices.IsEmpty())
        {
            AddValidLOD(Mesh->LOD1, TEXT("LOD1"));
        }
        if (LocalIndices.IsEmpty())
        {
            AddValidLOD(Mesh->LOD2, TEXT("LOD2"));
        }
        if (LocalIndices.IsEmpty())
        {
            AddValidLOD(Mesh->LOD3, TEXT("LOD3"));
        }

        if (Mesh->LOD1 != INDEX_NONE || Mesh->LOD2 != INDEX_NONE || Mesh->LOD3 != INDEX_NONE)
        {
            UE_LOG(LogTemp, VeryVerbose,
                TEXT("Runtime multi-LOD assembly disabled; loading one validated LOD only. Mesh=%s Node=%s"),
                *MeshName.ToString(),
                *CurrentLoadingNode.ToString());
        }
    }

    const int32 Count = LocalIndices.Num();
    if (Count <= 0)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Runtime mesh request skipped because no valid mesh index remains. Mesh=%s Node=%s MeshCount=%d"),
            *MeshName.ToString(),
            *CurrentLoadingNode.ToString(),
            RuntimeMeshCount);
        NodeMap.Empty();
        bGroupFailed = true;
        ResetLoadState();
        return;
    }

    TMap<int32, float> LODScreenSize;
    for (int32 Index = 0; Index < Count; ++Index)
    {
        LODScreenSize.Add(Index, CalculateLODScreenSize(Index, Count));
    }

    const bool bBuildComplexCollision = !bRenderOnly && Mesh->Data.bComplexCollision;
    const bool bBuildSimpleCollision = !bRenderOnly && Mesh->Data.bSimpleCollision;
    const bool bNeedsCollision = bBuildComplexCollision || bBuildSimpleCollision;
    UWorld* CollisionWorld = IsValid(GeneratedMeshWorldContext)
        ? GeneratedMeshWorldContext->GetWorld()
        : nullptr;
    if (bNeedsCollision && (!IsValid(CollisionWorld) || !CollisionWorld->IsGameWorld()))
    {
        UE_LOG(LogTemp, Error,
            TEXT("Runtime mesh collision build rejected because no active game-world outer is available. Mesh=%s Node=%s"),
            *MeshName.ToString(),
            *CurrentLoadingNode.ToString());
        NodeMap.Empty();
        bGroupFailed = true;
        ResetLoadState();
        return;
    }

    FglTFRuntimeStaticMeshConfig Config = StaticMeshConfig;
    Config.Outer = GeneratedMeshWorldContext.Get();
    Config.bBuildComplexCollision = bBuildComplexCollision;
    Config.bBuildSimpleCollision = bBuildSimpleCollision;
    Config.bBuildNavCollision = !bRenderOnly && Config.bBuildNavCollision;
    Config.bAllowCPUAccess = !bRenderOnly && (Config.bAllowCPUAccess || bBuildComplexCollision);
    Config.CollisionComplexity = bBuildComplexCollision
        ? ECollisionTraceFlag::CTF_UseComplexAsSimple
        : ECollisionTraceFlag::CTF_UseDefault;
    Config.LODScreenSize = LODScreenSize;
    Config.LODScreenSizeMultiplier = 1.0f;

    const TArray<int32> RequestedIndices = LocalIndices;
    const FglTFRuntimeStaticMeshConfig RequestedConfig = Config;
    const bool bRequestedCollision =
        RequestedConfig.bBuildComplexCollision || RequestedConfig.bBuildSimpleCollision;
    if (bRequestedCollision && (!IsValid(GeneratedMeshWorldContext)
        || !GeneratedMeshWorldContext->PinWorldForBuild()))
    {
        NodeMap.Empty();
        bGroupFailed = true;
        ResetLoadState();
        return;
    }
    bStaticMeshLoadInFlight = true;
    FglTFRuntimeStaticMeshAsync Callback;
    Callback.BindDynamic(this, &UWorldSceneStreamAction::SetStaticMesh);
    if (RequestedIndices.Num() == 1)
        MeshSourceAsset->LoadStaticMeshAsync(RequestedIndices[0], Callback, RequestedConfig);
    else
        MeshSourceAsset->LoadStaticMeshLODsAsync(RequestedIndices, Callback, RequestedConfig);
}

void UWorldSceneStreamAction::AddTransform(const FName &Name)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::AddTransform")))
    {
        return;
    }

    if (bAbortRequested || !IsValid(OwnerActor.Get()) || !IsValid(MeshActor.Get()))
    {
        ResetLoadState();
        return;
    }

    if (FModelNodeData *NodeInfo = NodeMap.Find(Name))
    {
        if (NodeInfo->Transform.ContainsNaN() || NodeInfo->MeshName.IsNone()
            || !MeshActor->AddNodeInstance(Name, NodeInfo->Transform))
        {
            NodeMap.Empty();
            bGroupFailed = true;
            ResetLoadState();
            return;
        }
        if (const FModelMeshData *MeshData = MeshMap.Find(NodeInfo->MeshName))
        {
            SpawnStreamComponents(Name, *NodeInfo, MeshData->Data);
        }

        if (UWorld* World = OwnerActor->GetWorld())
        {
            if (UStreamingMovementGateSubsystem* Gate = World->GetSubsystem<UStreamingMovementGateSubsystem>())
                Gate->SetModelRegionAvailable(OwnerActor.Get(), Name, FBox(ForceInit), true);
        }
    }
    ResetLoadState();
}

void UWorldSceneStreamAction::SpawnStreamComponents(const FName &NodeName, const FModelNodeData &NodeInfo, const FMeshData &Data)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::SpawnStreamComponents")))
    {
        return;
    }

    if (!IsValid(MeshActor) || MeshActor->HasDynamicComponents(NodeName))
    {
        return;
    }

    FComponentGroup Group;
    UWorld *World = MeshActor->GetWorld();
    if (!World)
        return;

    if (!bRenderOnly && Data.bSimpleCollision)
    {
        for (const FModelCollider &ColliderData : Data.Colliders)
        {
            UShapeComponent *NewShape = nullptr;
            FTransform ComponentLocalTransform = NodeInfo.Transform;
            // Collider centers in model JSON are expressed in the mesh node's local space.
            // Apply the offset through the node transform so rotation/scale remain consistent.
            ComponentLocalTransform.SetLocation(NodeInfo.Transform.TransformPosition(ColliderData.Center));

            if (ColliderData.Collider == EColliderType::Box)
            {
                UBoxComponent *BoxComp = NewObject<UBoxComponent>(MeshActor);
                if (BoxComp)
                {
                    BoxComp->SetBoxExtent(ColliderData.Size);
                    NewShape = BoxComp;
                }
            }
            else if (ColliderData.Collider == EColliderType::Sphere)
            {
                USphereComponent *SphereComp = NewObject<USphereComponent>(MeshActor);
                if (SphereComp)
                {
                    SphereComp->SetSphereRadius(ColliderData.Size.X);
                    NewShape = SphereComp;
                }
            }
            else if (ColliderData.Collider == EColliderType::Capsule)
            {
                UCapsuleComponent *CapsuleComp = NewObject<UCapsuleComponent>(MeshActor);
                if (CapsuleComp)
                {
                    CapsuleComp->SetCapsuleSize(ColliderData.Size.X, ColliderData.Size.Y);
                    NewShape = CapsuleComp;
                }
            }

            if (NewShape)
            {
                MeshActor->AddInstanceComponent(NewShape);
                NewShape->AttachToComponent(MeshActor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
                NewShape->SetRelativeTransform(ComponentLocalTransform);
                NewShape->SetCollisionProfileName(TEXT("BlockAll"));
                NewShape->RegisterComponent();
                Group.Colliders.Add(NewShape);
            }
        }
    }

    for (const FLightData &LightData : Data.Lights)
    {
        UDynamicPointLightComponent *PointLight = NewObject<UDynamicPointLightComponent>(MeshActor);
        if (PointLight)
        {
            MeshActor->AddInstanceComponent(PointLight);
            PointLight->AttachToComponent(MeshActor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
            const FVector LightLocalLocation = NodeInfo.Transform.TransformPosition(LightData.Location);
            PointLight->SetRelativeLocation(LightLocalLocation);
            PointLight->SetSourceRadius(LightData.SourceRadius);
            PointLight->SetSoftSourceRadius(LightData.SoftSourceRadius);
            PointLight->SetSourceLength(LightData.Length);
            PointLight->SetAttenuationRadius(LightData.AttenuationRadius);
            PointLight->SetIntensityUnits(LightData.Unit);
            PointLight->SetIntensity(LightData.Intensity);
            PointLight->SetLightDecal(DecalLight);
            PointLight->RegisterComponent();
            Group.Lights.Add(PointLight);
        }
    }

    if (Group.Colliders.Num() > 0 || Group.Lights.Num() > 0)
    {
        MeshActor->StoreDynamicComponents(NodeName, MoveTemp(Group));
    }
}

void UWorldSceneStreamAction::DestroyStreamComponents(const FName &NodeName)
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::DestroyStreamComponents")))
    {
        return;
    }

    if (IsValid(MeshActor))
    {
        MeshActor->DestroyDynamicComponents(NodeName);
    }
}

void UWorldSceneStreamAction::ResetLoadState()
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::ResetLoadState")))
    {
        return;
    }

    bIsLoading = false;
    bStaticMeshLoadInFlight = false;
    CurrentLoadingNode = NAME_None;
    CurrentLoadingMesh = NAME_None;
    BroadcastProgress();
}

void UWorldSceneStreamAction::BroadcastProgress()
{
    if (!EnsureStreamActionGameThread(TEXT("UWorldSceneStreamAction::BroadcastProgress")))
    {
        return;
    }

    if (TotalOperationCount <= 0)
    {
        Progress.Broadcast(GroupName, 1.0f);
        return;
    }

    const int32 CompletedLoadNodes = FMath::Max(0, CurrentLoadIndex - (bIsLoading ? 1 : 0));
    const int32 CompletedOperations = FMath::Clamp(
        CurrentSkippedOperationIndex +
        CurrentUnloadIndex +
        CompletedLoadNodes +
        CurrentUnloadWaterIndex +
        CurrentLoadWaterIndex,
        0,
        TotalOperationCount);
    Progress.Broadcast(GroupName, FMath::Clamp(
        static_cast<float>(CompletedOperations) / static_cast<float>(TotalOperationCount),
        0.0f,
        1.0f));
}
