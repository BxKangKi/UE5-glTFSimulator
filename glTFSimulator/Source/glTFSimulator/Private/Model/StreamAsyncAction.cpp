// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Model/StreamAsyncAction.h"
#include "System/GameManagerSubSystem.h"
#include "System/ActorHelper.h"
#include "System/MacroLibrary.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Components/CapsuleComponent.h"
#include "Model/DynamicPointLightComponent.h"
#include "Engine/World.h"
#include "Engine/Texture.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "HAL/IConsoleManager.h"
#include "Model/glTFStreamActor.h"
#include "System/GlbValidation.h"
#include "System/SafeFileIO.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "System/glTFRuntimeSafety.h"
#include "Model/glTFPrefabSubSystem.h"
#include "TimerManager.h"
#include "UObject/UObjectGlobals.h"
#include "World/WaterActor.h"

namespace
{
    // glTFRuntime has an unresolved startup crash path in LoadStaticMeshLODs. Runtime streaming
    // therefore uses the stable single-mesh API unless a project explicitly opts back in.
    constexpr int32 DefaultRuntimeMultiLOD = 0;

    TAutoConsoleVariable<int32> CVarEnableRuntimeMultiLOD(
        TEXT("gltf.Streaming.EnableRuntimeMultiLOD"),
        DefaultRuntimeMultiLOD,
        TEXT("Enables glTFRuntime multi-LOD mesh assembly. 0 loads one validated LOD (prefers LOD0); 1 enables LOD1-LOD3."),
        ECVF_Default);

    /** Every UObject/component mutation in this action is intentionally game-thread-only. */
    bool EnsureStreamActionGameThread(const TCHAR* FunctionName)
    {
        return ensureMsgf(IsInGameThread(), TEXT("%s must run on the game thread"), FunctionName);
    }

    int32 FindFirstRenderableMeshIndex(UglTFRuntimeAsset* RuntimeAsset)
    {
        if (!IsValid(RuntimeAsset))
        {
            return INDEX_NONE;
        }

        const int32 MeshCount = RuntimeAsset->GetNumMeshes();
        if (MeshCount <= 0)
        {
            return INDEX_NONE;
        }

        for (const FglTFRuntimeNode& Node : RuntimeAsset->GetNodes())
        {
            if (Node.MeshIndex >= 0 && Node.MeshIndex < MeshCount)
            {
                return Node.MeshIndex;
            }
        }

        return 0;
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

void UglTFGeneratedStaticMeshWorldContext::Initialize(UWorld* InWorld)
{
    PinnedWorld = nullptr;
    World = InWorld;
}

bool UglTFGeneratedStaticMeshWorldContext::PinWorldForBuild()
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

void UglTFGeneratedStaticMeshWorldContext::ReleaseWorldPin()
{
    PinnedWorld = nullptr;
}

UWorld* UglTFGeneratedStaticMeshWorldContext::GetWorld() const
{
    if (UWorld* ActiveWorld = PinnedWorld.Get())
    {
        return ActiveWorld;
    }

    return World.Get();
}

UStreamAsyncAction *UStreamAsyncAction::StreamAsync(
    UObject *WorldContextObject,
    AglTFStreamActor *Actor,
    const FVector &InPlayerLocation,
    const FglTFRuntimeStaticMeshConfig &StaticMeshConfig,
    float InDistance,
    int32 InChunkSize,
    bool bInRenderOnly)
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::StreamAsync")))
    {
        return nullptr;
    }

    auto *Action = NewObject<UStreamAsyncAction>();
    Action->WorldContextObject = WorldContextObject;
    UWorld* RuntimeWorld = IsValid(Actor)
        ? Actor->GetWorld()
        : (IsValid(WorldContextObject) ? WorldContextObject->GetWorld() : nullptr);
    if (IsValid(RuntimeWorld) && RuntimeWorld->IsGameWorld())
    {
        UglTFGeneratedStaticMeshWorldContext* MeshWorldContext =
            NewObject<UglTFGeneratedStaticMeshWorldContext>(GetTransientPackage(), NAME_None, RF_Transient);
        if (IsValid(MeshWorldContext))
        {
            MeshWorldContext->Initialize(RuntimeWorld);
            Action->GeneratedMeshWorldContext = MeshWorldContext;
        }
    }

    if (IsValid(Actor))
    {
        Action->OwnerActor = Actor;
        Action->NodeMap = Actor->GetAllNodeMapRef();
        Action->WaterNodeMap = Actor->GetWaterNodeMapRef();
        Action->MeshMap = Actor->GetAllMeshMapRef();
        Action->DecalLight = Actor->GetDecalLight();
        Action->DynamicComponentMap = Actor->GetDynamicComponentMapRef();
        Action->UnloadBoxMap = Actor->GetUnloadBoxMapRef();
        Action->WaterActorMap = Actor->GetWaterActorMapRef();
        Action->LoadedNodes = Actor->GetLoadedNodesRef();
        Action->LoadedWaterNodes = Actor->GetLoadedWaterNodesRef();
        Action->InstanceMap = Actor->GetInstanceMapRef();
        Action->Asset = Actor->GetAsset();
        Action->PrefabReferenceName = Actor->GetModelMetadata().Prefab;
        Action->WaterClass = Actor->GetWaterClass();
    }
    Action->PlayerLocation = InPlayerLocation;
    Action->Distance = InDistance;
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


void UStreamAsyncAction::CancelAndRelease()
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::CancelAndRelease")))
    {
        return;
    }

    AbortAndRelease();
}

void UStreamAsyncAction::AbortAndRelease(UStaticMesh* OrphanedMesh)
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::AbortAndRelease")))
    {
        return;
    }

    bAbortRequested = true;

    // Remove only queued work. Active native mesh construction is not interrupted because doing so
    // could free parser or render data while the plugin worker still owns it.
    FglTFRuntimeSafety::CancelQueuedOperations(this);

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

    // Do not release the parser/action while glTFRuntime still owns an async mesh build.
    // SetStaticMesh will receive the terminal callback and complete the deferred cleanup.
    if ((bStaticMeshLoadInFlight || bPrefabAssetLoadInFlight) && OrphanedMesh == nullptr)
    {
        OwnerActor = nullptr;
        WorldContextObject = nullptr;
        return;
    }

    bStaticMeshLoadInFlight = false;
    GlTFRuntimeOperationTicket = 0;
    if (IsValid(OrphanedMesh) && !OrphanedMesh->IsAsset())
    {
        OrphanedMesh->ClearFlags(RF_Public | RF_Standalone);
    }

    TArray<FName> PrefabNodesToRelease;
    PrefabReferenceTokensByNode.GetKeys(PrefabNodesToRelease);
    for (const FName& PrefabNode : PrefabNodesToRelease)
    {
        ReleasePrefabReferenceForNode(PrefabNode);
    }
    PrefabReferenceName.Reset();
    bPrefabAssetLoadInFlight = false;
    if (PrefabReferenceTokensByNode.Num() == 0)
    {
        PrefabAsset = nullptr;
        PrefabMeshIndex = INDEX_NONE;
        PrefabFilePath.Reset();
        PrefabReferenceReleasePendingNodes.Empty();
    }

    if (IsValid(Asset))
    {
        // Final cache cleanup is a per-asset barrier. It waits for this callback's native ticket
        // instead of racing ClearCache against glTFRuntime's worker-owned parser state.
        FglTFRuntimeSafety::RequestAssetRelease(Asset);
    }

    NodeMap.Empty();
    WaterNodeMap.Empty();
    MeshMap.Empty();
    LoadedNodes.Empty();
    LoadedWaterNodes.Empty();
    InstanceMap.Empty();
    DynamicComponentMap.Empty();
    WaterActorMap.Empty();
    UnloadBoxMap.Empty();
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
    WorldContextObject = nullptr;
    MaterialReferenceGuard = nullptr;
    if (IsValid(GeneratedMeshWorldContext))
    {
        GeneratedMeshWorldContext->ReleaseWorldPin();
    }
    GeneratedMeshWorldContext = nullptr;
    DecalLight = nullptr;
    // Do not clear the active node here. A native prefab mesh request may still be inside
    // glTFRuntime; EndPrefabAssetUseForNode needs its token even after an abort clears the
    // actor/world references above.
    if (PrefabUseNodesInFlight.Num() == 0)
    {
        CurrentLoadingNode = NAME_None;
        CurrentLoadingMesh = NAME_None;
    }
    bIsLoading = false;
    bRenderOnly = false;
    SetReadyToDestroy();
}

void UStreamAsyncAction::Activate()
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::Activate")))
    {
        return;
    }

    bAbortRequested = false;
    bStaticMeshLoadInFlight = false;
    GlTFRuntimeOperationTicket = 0;

    if (!IsValid(OwnerActor) || !IsValid(WorldContextObject))
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
        const float CheckRadius = FMath::Square(MeshSize + (MeshSize * Distance));

        const float CurrentDist = FVector::DistSquared(PlayerLocation, Info.Transform.GetLocation());
        const bool bIsLoaded = LoadedNodes.Contains(NodePair.Key);

        if (bIsLoaded)
        {
            if (CurrentDist > CheckRadius)
            {
                PendingUnloadNodes.Add(NodePair.Key);
            }
        }
        else
        {
            if (CurrentDist <= CheckRadius && CurrentLoadingNode != NodePair.Key)
            {
                PendingLoadNodes.Add(NodePair.Key);
            }
        }
    }

    for (const TPair<FName, FWaterStreamNodeData>& WaterPair : WaterNodeMap)
    {
        const float CurrentDist = FVector::DistSquared(PlayerLocation, WaterPair.Value.Transform.GetLocation());
        const float CheckRadius = GetWaterStreamRadiusSq(WaterPair.Value);
        const bool bIsLoaded = LoadedWaterNodes.Contains(WaterPair.Key);

        if (bIsLoaded)
        {
            if (CurrentDist > CheckRadius)
            {
                PendingUnloadWaterNodes.Add(WaterPair.Key);
            }
        }
        else if (CurrentDist <= CheckRadius)
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

FString UStreamAsyncAction::MakePrefabReferenceToken(const FName& NodeName) const
{
    return PrefabReferenceSessionId.ToString(EGuidFormats::Digits) + TEXT("|") + NodeName.ToString();
}

bool UStreamAsyncAction::AcquirePrefabReferenceForNode(const FName& NodeName)
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::AcquirePrefabReferenceForNode")))
    {
        return false;
    }

    if (PrefabReferenceName.TrimStartAndEnd().IsEmpty())
    {
        return true;
    }

    if (PrefabReferenceTokensByNode.Contains(NodeName) && IsValid(PrefabAsset) && PrefabMeshIndex != INDEX_NONE)
    {
        return true;
    }

    if (bPrefabAssetLoadInFlight)
    {
        return false;
    }

    const AglTFStreamActor* StreamActor = Cast<AglTFStreamActor>(OwnerActor.Get());
    const FString ModelFilePath = IsValid(StreamActor) ? StreamActor->GetFilePath() : FString();
    UglTFPrefabSubSystem* PrefabManager = PrefabSubSystem.IsValid()
        ? PrefabSubSystem.Get()
        : UglTFPrefabSubSystem::Get(WorldContextObject);
    PrefabSubSystem = PrefabManager;
    if (!IsValid(PrefabManager) || ModelFilePath.IsEmpty())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("UStreamAsyncAction: prefab subsystem/model path unavailable; using source model. Node=%s Prefab=%s"),
            *NodeName.ToString(),
            *PrefabReferenceName);
        return true;
    }

    const FString ResolvedPrefabPath = PrefabManager->ResolvePrefabPath(ModelFilePath, PrefabReferenceName);
    if (ResolvedPrefabPath.IsEmpty())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("UStreamAsyncAction: invalid prefab reference; using source model. Node=%s Prefab=%s"),
            *NodeName.ToString(),
            *PrefabReferenceName);
        return true;
    }

    const FString ReferenceToken = MakePrefabReferenceToken(NodeName);
    PendingPrefabReferenceNode = NodeName;
    PrefabFilePath = ResolvedPrefabPath;
    bPrefabAssetLoadInFlight = true;
    bIsLoading = true;

    TWeakObjectPtr<UStreamAsyncAction> WeakThis(this);
    FOnPrefabRuntimeAssetReady Callback;
    Callback.BindLambda(
        [WeakThis, NodeName, ResolvedPrefabPath, ReferenceToken](
            UglTFRuntimeAsset* RuntimeAsset,
            const int32 MeshIndex,
            const bool bSuccess,
            const FString& FailureReason)
        {
            UStreamAsyncAction* StrongThis = WeakThis.Get();
            if (!IsValid(StrongThis))
            {
                return;
            }

            StrongThis->bPrefabAssetLoadInFlight = false;
            StrongThis->PendingPrefabReferenceNode = NAME_None;

            if (StrongThis->bAbortRequested)
            {
                if (UglTFPrefabSubSystem* PrefabManager = StrongThis->PrefabSubSystem.Get())
                {
                    PrefabManager->ReleasePrefabReference(ResolvedPrefabPath, ReferenceToken);
                }
                StrongThis->ReleaseActionReferences();
                StrongThis->SetReadyToDestroy();
                return;
            }

            if (!bSuccess || !IsValid(RuntimeAsset) || MeshIndex == INDEX_NONE)
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("UStreamAsyncAction: prefab '%s' could not be resolved for node '%s' (%s); using source model."),
                    *ResolvedPrefabPath,
                    *NodeName.ToString(),
                    FailureReason.IsEmpty() ? TEXT("unknown error") : *FailureReason);
                if (UglTFPrefabSubSystem* PrefabManager = StrongThis->PrefabSubSystem.Get())
                {
                    PrefabManager->ReleasePrefabReference(ResolvedPrefabPath, ReferenceToken);
                }
                StrongThis->PrefabAsset = nullptr;
                StrongThis->PrefabMeshIndex = INDEX_NONE;
                StrongThis->PrefabFilePath.Reset();
                StrongThis->bIsLoading = true;
                StrongThis->LoadStaticMeshAsync(StrongThis->CurrentLoadingMesh);
                return;
            }

            StrongThis->PrefabAsset = RuntimeAsset;
            StrongThis->PrefabMeshIndex = MeshIndex;
            StrongThis->PrefabFilePath = ResolvedPrefabPath;
            StrongThis->PrefabReferenceReleasePendingNodes.Remove(NodeName);
            StrongThis->PrefabReferenceTokensByNode.Add(NodeName, ReferenceToken);
            UE_LOG(LogTemp, Verbose,
                TEXT("UStreamAsyncAction: prefab reference acquired. Node=%s Prefab=%s MeshIndex=%d"),
                *NodeName.ToString(),
                *ResolvedPrefabPath,
                MeshIndex);

            StrongThis->LoadStaticMeshAsync(StrongThis->CurrentLoadingMesh);
        });

    PrefabManager->AcquirePrefabReference(
        ModelFilePath,
        PrefabReferenceName,
        ReferenceToken,
        MoveTemp(Callback));

    return false;
}

void UStreamAsyncAction::ReleasePrefabReferenceForNode(const FName& NodeName)
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::ReleasePrefabReferenceForNode")))
    {
        return;
    }

    FString* TokenPtr = PrefabReferenceTokensByNode.Find(NodeName);
    if (!TokenPtr || PrefabReferenceReleasePendingNodes.Contains(NodeName))
    {
        return;
    }

    const FString Token = *TokenPtr;
    const FString Path = PrefabFilePath;
    if (UglTFPrefabSubSystem* PrefabManager = PrefabSubSystem.Get())
    {
        PrefabManager->ReleasePrefabReference(Path, Token);
    }

    if (PrefabUseNodesInFlight.Contains(NodeName))
    {
        PrefabReferenceReleasePendingNodes.Add(NodeName);
        return;
    }

    PrefabReferenceTokensByNode.Remove(NodeName);
    if (PrefabReferenceTokensByNode.Num() == 0)
    {
        PrefabAsset = nullptr;
        PrefabMeshIndex = INDEX_NONE;
        PrefabFilePath.Reset();
    }
}

void UStreamAsyncAction::EndPrefabAssetUseForNode(const FName& NodeName)
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::EndPrefabAssetUseForNode")))
    {
        return;
    }

    // The action may already have released OwnerActor/WorldContextObject after an abort.
    // Keep the exact token/path captured at BeginPrefabAssetUse time so the subsystem can
    // always balance the native-use lease and eventually unload the shared prefab asset.
    const FName UseNode = !NodeName.IsNone() ? NodeName : ActivePrefabUseNode;
    const FString* TokenPtr = PrefabReferenceTokensByNode.Find(UseNode);
    const FString Token = TokenPtr ? *TokenPtr : ActivePrefabUseToken;
    const FString Path = !ActivePrefabUsePath.IsEmpty() ? ActivePrefabUsePath : PrefabFilePath;

    if (UglTFPrefabSubSystem* PrefabManager = PrefabSubSystem.Get())
    {
        if (!Token.IsEmpty() && !Path.IsEmpty())
        {
            PrefabManager->EndPrefabAssetUse(Path, Token);
        }
    }

    if (!UseNode.IsNone())
    {
        PrefabUseNodesInFlight.Remove(UseNode);

        if (PrefabReferenceReleasePendingNodes.Remove(UseNode) > 0)
        {
            PrefabReferenceTokensByNode.Remove(UseNode);
        }
    }

    ActivePrefabUseNode = NAME_None;
    ActivePrefabUsePath.Reset();
    ActivePrefabUseToken.Reset();

    if (PrefabReferenceTokensByNode.Num() == 0 && PrefabUseNodesInFlight.Num() == 0)
    {
        PrefabAsset = nullptr;
        PrefabMeshIndex = INDEX_NONE;
        PrefabFilePath.Reset();
        PrefabReferenceReleasePendingNodes.Empty();
        if (bAbortRequested)
        {
            PrefabSubSystem = nullptr;
        }
    }
}

void UStreamAsyncAction::SanitizeRuntimeMaps()
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::SanitizeRuntimeMaps")))
    {
        return;
    }

    for (auto It = NodeMap.CreateIterator(); It; ++It)
    {
        const FModelNodeData& Node = It.Value();
        if (It.Key().IsNone() || Node.MeshName.IsNone() || Node.Transform.ContainsNaN() || !MeshMap.Contains(Node.MeshName))
        {
            LoadedNodes.Remove(It.Key());
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
        if (!ReferencedMeshes.Contains(It.Key()) || Mesh.LOD0 == INDEX_NONE || Mesh.Size.ContainsNaN())
        {
            if (TObjectPtr<UInstancedStaticMeshComponent>* Component = InstanceMap.Find(It.Key()))
            {
                if (IsValid(Component->Get()))
                {
                    UInstancedStaticMeshComponent* ComponentToDestroy = Component->Get();
                    ComponentToDestroy->ClearInstances();
                    ComponentToDestroy->SetStaticMesh(nullptr);
                    FActorHelper::DestroyComponent(OwnerActor, ComponentToDestroy);
                }
            }
            InstanceMap.Remove(It.Key());
            It.RemoveCurrent();
        }
    }

    for (auto It = LoadedNodes.CreateIterator(); It; ++It)
    {
        if (!NodeMap.Contains(*It))
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

void UStreamAsyncAction::ReleaseActionReferences()
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::ReleaseActionReferences")))
    {
        return;
    }

    FglTFRuntimeSafety::CancelQueuedOperations(this);
    GlTFRuntimeOperationTicket = 0;
    UWorld* World = IsValid(OwnerActor) ? OwnerActor->GetWorld() : (IsValid(WorldContextObject) ? WorldContextObject->GetWorld() : nullptr);
    if (World)
    {
        World->GetTimerManager().ClearTimer(ProcessTimerHandle);
    }

    TArray<FName> PrefabNodesToRelease;
    PrefabReferenceTokensByNode.GetKeys(PrefabNodesToRelease);
    for (const FName& PrefabNode : PrefabNodesToRelease)
    {
        ReleasePrefabReferenceForNode(PrefabNode);
    }
    PrefabReferenceName.Reset();
    bPrefabAssetLoadInFlight = false;
    if (PrefabReferenceTokensByNode.Num() == 0 && PrefabUseNodesInFlight.Num() == 0)
    {
        PrefabReferenceReleasePendingNodes.Empty();
        PrefabAsset = nullptr;
        PrefabMeshIndex = INDEX_NONE;
        PrefabFilePath.Reset();
    }

    Asset = nullptr;
    OwnerActor = nullptr;
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
    LoadedNodes.Empty();
    LoadedWaterNodes.Empty();
    InstanceMap.Empty();
    DynamicComponentMap.Empty();
    WaterActorMap.Empty();
    UnloadBoxMap.Empty();
    DecalLight = nullptr;
    CurrentLoadingNode = NAME_None;
    CurrentLoadingMesh = NAME_None;
    bIsLoading = false;
    bStaticMeshLoadInFlight = false;
    bAbortRequested = true;
    bRenderOnly = false;
    if (PrefabReferenceTokensByNode.Num() == 0 && PrefabUseNodesInFlight.Num() == 0)
    {
        PrefabSubSystem = nullptr;
    }
}

void UStreamAsyncAction::ProcessChunk()
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::ProcessChunk")))
    {
        return;
    }

    if (bAbortRequested || !IsValid(OwnerActor))
    {
        AbortAndRelease();
        return;
    }

    if (bPrefabAssetLoadInFlight)
    {
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
            if (LoadedNodes.Contains(TargetNode))
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
        FStreamAsyncWrapper Wrapper;
        Wrapper.NodeMap = MoveTemp(NodeMap);
        Wrapper.WaterNodeMap = MoveTemp(WaterNodeMap);
        Wrapper.LoadedNodes = MoveTemp(LoadedNodes);
        Wrapper.LoadedWaterNodes = MoveTemp(LoadedWaterNodes);
        Wrapper.InstanceMap = MoveTemp(InstanceMap);
        Wrapper.UnloadBoxMap = MoveTemp(UnloadBoxMap); // Move data into the wrapper struct.
        Wrapper.DynamicComponentMap = MoveTemp(DynamicComponentMap); // Move data into the wrapper struct.
        Wrapper.WaterActorMap = MoveTemp(WaterActorMap);

        Progress.Broadcast(1.0f);
        Completed.Broadcast(Wrapper);
        ReleaseActionReferences();
        SetReadyToDestroy();
    }
    else
    {
        UWorld *World = OwnerActor->GetWorld();
        if (IsValid(World))
        {
            ProcessTimerHandle = World->GetTimerManager().SetTimerForNextTick(
                FTimerDelegate::CreateUObject(this, &UStreamAsyncAction::ProcessChunk));
        }
    }
}


float UStreamAsyncAction::GetWaterStreamRadiusSq(const FWaterStreamNodeData& Data) const
{
    const float Radius = FMath::Max3(Data.StreamRadius, Distance * 1024.0f, 2048.0f);
    return FMath::Square(Radius);
}

void UStreamAsyncAction::ProcessLoadWaterNode(const FName& Name)
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::ProcessLoadWaterNode")))
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

    AWaterActor *WaterActor = World->SpawnActor<AWaterActor>(WaterClass, WaterInfo->Transform, Params);
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

void UStreamAsyncAction::ProcessUnloadWaterNode(const FName& Name)
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::ProcessUnloadWaterNode")))
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

bool UStreamAsyncAction::ProcessLoadNode(const FName &Name)
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::ProcessLoadNode")))
    {
        return false;
    }

    if (bAbortRequested)
    {
        return false;
    }

    if (FModelNodeData *Info = NodeMap.Find(Name))
    {
        if (LoadedNodes.Contains(Name))
        {
            return false;
        }

        if (Info->MeshName.IsNone() || Info->Transform.ContainsNaN() || !MeshMap.Contains(Info->MeshName))
        {
            LoadedNodes.Remove(Name);
            NodeMap.Remove(Name);
            return false;
        }

        // Every node gets its own prefab reference token, even when the mesh component can be shared
        // with another node. This keeps the global prefab reference count equal to actual node usage.
        CurrentLoadingNode = Name;
        CurrentLoadingMesh = Info->MeshName;
        bIsLoading = true;

        if (!AcquirePrefabReferenceForNode(Name))
        {
            // The prefab subsystem is loading this node's referenced asset. Its callback resumes the
            // same node and invokes LoadStaticMeshAsync once the shared asset becomes available.
            return true;
        }

        UInstancedStaticMeshComponent *ISMC = InstanceMap.FindRef(Info->MeshName);
        if (IsValid(ISMC))
        {
            AddTrasnform(Name, ISMC);
            return true;
        }

        // Prefab acquisition either completed synchronously or was not required. Current loading
        // state already belongs to this node, so start the native mesh request directly.
        LoadStaticMeshAsync(CurrentLoadingMesh);
        return true;
    }

    return false;
}

void UStreamAsyncAction::ProcessUnloadNode(const FName &Name)
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::ProcessUnloadNode")))
    {
        return;
    }

    if (bAbortRequested || !IsValid(OwnerActor.Get()))
    {
        return;
    }

    FModelNodeData *Info = NodeMap.Find(Name);
    if (!Info)
        return;

    DestroyStreamComponents(Name);

    UInstancedStaticMeshComponent *ISMC = InstanceMap.FindRef(Info->MeshName);
    if (IsValid(ISMC))
    {
        int32 InstanceCount = ISMC->GetNumInstances();
        if (InstanceCount > 1)
        {
            for (int32 i = 0; i < InstanceCount; i++)
            {
                FTransform Transform;
                ISMC->GetInstanceTransform(i, Transform);
                if (Transform.Equals(Info->Transform, 0.01f))
                {
                    ISMC->RemoveInstance(i);
                    break;
                }
            }
        }
        else
        {
            // The glTFRuntimeAsset ReadWrite cache owns reusable mesh references. Detaching the
            // component is sufficient; duplicating ownership in a second manager caused stale roots.
            ISMC->ClearInstances();
            ISMC->SetStaticMesh(nullptr);
            FActorHelper::DestroyComponent(OwnerActor, ISMC);
            InstanceMap.Remove(Info->MeshName);
        }
        LoadedNodes.Remove(Name);
    }

    ReleasePrefabReferenceForNode(Name);

    if (bRenderOnly)
    {
        return;
    }

    // Use UnloadBoxMap to check targets and manage creation separately.
    TObjectPtr<UBoxComponent> *UnloadBoxPtr = UnloadBoxMap.Find(Name);
    if (!UnloadBoxPtr || !IsValid(*UnloadBoxPtr))
    {
        if (const FModelMeshData *Mesh = MeshMap.Find(Info->MeshName))
        {
            FVector BoxExtent = Mesh->Size + BOX_BUFFER_SIZE;
            UBoxComponent *NewBox = FActorHelper::AddBoxComponent(OwnerActor, Info->Transform, BoxExtent, TEXT("BlockAll"));
            UnloadBoxMap.Emplace(Name, NewBox);
        }
    }
}

void UStreamAsyncAction::SetStaticMesh(UStaticMesh *StaticMesh)
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::SetStaticMesh")))
    {
        return;
    }

    const uint64 CompletedTicket = GlTFRuntimeOperationTicket;
    GlTFRuntimeOperationTicket = 0;
    bStaticMeshLoadInFlight = false;
    EndPrefabAssetUseForNode(CurrentLoadingNode);
    ON_SCOPE_EXIT
    {
        // Collision finalization happens before glTFRuntime invokes this delegate. Drop the strong
        // world pin only after all callback-side component work is complete, and release the parser
        // ticket on the next game-thread task so the plugin can finish unregistering its GC guard.
        if (IsValid(GeneratedMeshWorldContext))
        {
            GeneratedMeshWorldContext->ReleaseWorldPin();
        }
        FglTFRuntimeSafety::CompleteOperationAfterCallback(CompletedTicket);
    };
    if (bAbortRequested || !IsValid(OwnerActor))
    {
        AbortAndRelease(StaticMesh);
        return;
    }
    if (!IsValid(StaticMesh))
    {
        // OwnerActor is intentionally stored as AActor because the rest of this action only needs
        // generic actor services. Read the source GLB path only after verifying the concrete type.
        const AglTFStreamActor* StreamActor = Cast<AglTFStreamActor>(OwnerActor.Get());
        const FString FailedPath = IsValid(StreamActor) ? StreamActor->GetFilePath() : FString();
        FglTFRuntimeSafety::ReportRecoverableFailure(
            FailedPath,
            FString::Printf(TEXT("glTFRuntime rejected or failed static mesh '%s' for node '%s'"),
                *CurrentLoadingMesh.ToString(), *CurrentLoadingNode.ToString()));
        LoadedNodes.Remove(CurrentLoadingNode);
        NodeMap.Remove(CurrentLoadingNode);
        ResetLoadState();
        return;
    }

    // ReadWrite caching is owned by this UglTFRuntimeAsset. No second root-managed mesh registry
    // is needed, and all components for this actor receive the exact cached mesh instance.
    UStaticMesh* MeshToUse = StaticMesh;
    const FModelMeshData* ModelMeshData = MeshMap.Find(CurrentLoadingMesh);
    const bool bWantsCollision = !bRenderOnly && ModelMeshData &&
        (ModelMeshData->Data.bComplexCollision || ModelMeshData->Data.bSimpleCollision);

    UInstancedStaticMeshComponent *ISMC = InstanceMap.FindRef(CurrentLoadingMesh);
    if (!IsValid(ISMC))
    {
        ISMC = FActorHelper::AddStaticMeshComponent<UInstancedStaticMeshComponent>(
            OwnerActor,
            OwnerActor->GetTransform(),
            MeshToUse,
            bWantsCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision,
            bWantsCollision ? ECR_Block : ECR_Ignore);
        if (IsValid(ISMC))
        {
            ISMC->SetRenderCustomDepth(true);
            ISMC->SetCustomDepthStencilValue(1);
            InstanceMap.Emplace(CurrentLoadingMesh, ISMC);
        }
    }

    ConfigureGeneratedMeshCollision(
        ISMC,
        ModelMeshData ? &ModelMeshData->Data : nullptr,
        bRenderOnly);

    if (IsValid(ISMC))
    {
        // SetStaticMesh happened before registration, and AddInstance creates the per-instance body.
        // A second RecreatePhysicsState here can overlap Chaos scene insertion in packaged builds.
        AddTrasnform(CurrentLoadingNode, ISMC);
    }
    else
    {
        ReleasePrefabReferenceForNode(CurrentLoadingNode);
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

void UStreamAsyncAction::LoadStaticMeshAsync(const FName &MeshName)
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::LoadStaticMeshAsync")))
    {
        return;
    }

    if (bAbortRequested || !IsValid(Asset))
    {
        AbortAndRelease();
        return;
    }

    const bool bUsePrefabRuntimeMesh = IsValid(PrefabAsset) && PrefabMeshIndex != INDEX_NONE;
    UglTFRuntimeAsset* MeshSourceAsset = bUsePrefabRuntimeMesh ? PrefabAsset.Get() : Asset.Get();
    if (!IsValid(MeshSourceAsset))
    {
        ResetLoadState();
        return;
    }

    FModelMeshData* Mesh = MeshMap.Find(MeshName);
    if (!Mesh)
    {
        ResetLoadState();
        return;
    }

    const int32 RuntimeMeshCount = MeshSourceAsset->GetNumMeshes();
    TArray<int32> LocalIndices;
    LocalIndices.Reserve(4);

    if (bUsePrefabRuntimeMesh)
    {
        if (PrefabMeshIndex >= 0 && PrefabMeshIndex < RuntimeMeshCount)
        {
            LocalIndices.Add(PrefabMeshIndex);
        }
    }

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

    const bool bEnableRuntimeMultiLOD = !bUsePrefabRuntimeMesh &&
        CVarEnableRuntimeMultiLOD.GetValueOnGameThread() != 0;
    if (!bUsePrefabRuntimeMesh && bEnableRuntimeMultiLOD)
    {
        AddValidLOD(Mesh->LOD0, TEXT("LOD0"));
        AddValidLOD(Mesh->LOD1, TEXT("LOD1"));
        AddValidLOD(Mesh->LOD2, TEXT("LOD2"));
        AddValidLOD(Mesh->LOD3, TEXT("LOD3"));
    }
    else if (!bUsePrefabRuntimeMesh)
    {
        // Prefer LOD0, but tolerate malformed/legacy metadata by selecting the first valid fallback.
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
        LoadedNodes.Remove(CurrentLoadingNode);
        NodeMap.Remove(CurrentLoadingNode);
        ReleasePrefabReferenceForNode(CurrentLoadingNode);
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
        LoadedNodes.Remove(CurrentLoadingNode);
        NodeMap.Remove(CurrentLoadingNode);
        ReleasePrefabReferenceForNode(CurrentLoadingNode);
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
    const FName RequestedNode = CurrentLoadingNode;
    const FName RequestedMesh = CurrentLoadingMesh;
    const FString RequestedPrefabPath = PrefabFilePath;
    const FString* PrefabTokenPtr = PrefabReferenceTokensByNode.Find(RequestedNode);
    const FString RequestedPrefabToken = PrefabTokenPtr ? *PrefabTokenPtr : FString();
    const bool bRequestUsesPrefab = bUsePrefabRuntimeMesh && !RequestedPrefabPath.IsEmpty() && !RequestedPrefabToken.IsEmpty();
    TWeakObjectPtr<UStreamAsyncAction> WeakThis(this);
    bStaticMeshLoadInFlight = true;
    const uint64 SubmittedTicket = FglTFRuntimeSafety::EnqueueOperation(
        this,
        MeshSourceAsset,
        FString::Printf(TEXT("Stream mesh %s for node %s%s"),
            *RequestedMesh.ToString(),
            *RequestedNode.ToString(),
            bUsePrefabRuntimeMesh ? TEXT(" (prefab)") : TEXT("")),
        [WeakThis, RequestedIndices, RequestedConfig, RequestedNode, RequestedMesh, RequestedPrefabPath, RequestedPrefabToken, bRequestUsesPrefab](const uint64 Ticket)
        {
            UStreamAsyncAction* StrongThis = WeakThis.Get();
            if (!IsValid(StrongThis) || StrongThis->bAbortRequested || !IsValid(StrongThis->Asset))
            {
                if (IsValid(StrongThis))
                {
                    StrongThis->GlTFRuntimeOperationTicket = 0;
                    StrongThis->bStaticMeshLoadInFlight = false;
                    StrongThis->AbortAndRelease();
                }
                // Native mesh construction has not started in this branch, so immediate ticket
                // completion is safe and allows a pending asset release to proceed.
                FglTFRuntimeSafety::CompleteOperation(Ticket);
                return;
            }

            UglTFRuntimeAsset* CallbackMeshSourceAsset =
                IsValid(StrongThis->PrefabAsset) && StrongThis->PrefabMeshIndex != INDEX_NONE
                    ? StrongThis->PrefabAsset.Get()
                    : StrongThis->Asset.Get();
            if (!IsValid(CallbackMeshSourceAsset))
            {
                StrongThis->GlTFRuntimeOperationTicket = 0;
                StrongThis->bStaticMeshLoadInFlight = false;
                StrongThis->ReleasePrefabReferenceForNode(RequestedNode);
                StrongThis->ResetLoadState();
                FglTFRuntimeSafety::CompleteOperation(Ticket);
                return;
            }

            const bool bRequestedCollision =
                RequestedConfig.bBuildComplexCollision || RequestedConfig.bBuildSimpleCollision;
            if (bRequestedCollision &&
                (!IsValid(StrongThis->GeneratedMeshWorldContext) ||
                 !StrongThis->GeneratedMeshWorldContext->PinWorldForBuild()))
            {
                UE_LOG(LogTemp, Error,
                    TEXT("Runtime collision build lost its game world before native execution. Mesh=%s Node=%s"),
                    *RequestedMesh.ToString(),
                    *RequestedNode.ToString());
                StrongThis->GlTFRuntimeOperationTicket = 0;
                StrongThis->bStaticMeshLoadInFlight = false;
                StrongThis->LoadedNodes.Remove(RequestedNode);
                StrongThis->NodeMap.Remove(RequestedNode);
                StrongThis->ReleasePrefabReferenceForNode(RequestedNode);
                StrongThis->ResetLoadState();
                FglTFRuntimeSafety::CompleteOperation(Ticket);
                return;
            }

            if (bRequestUsesPrefab)
            {
                UglTFPrefabSubSystem* PrefabSubSystem = UglTFPrefabSubSystem::Get(StrongThis->WorldContextObject);
                if (!IsValid(PrefabSubSystem) || !PrefabSubSystem->BeginPrefabAssetUse(RequestedPrefabPath, RequestedPrefabToken))
                {
                    StrongThis->GlTFRuntimeOperationTicket = 0;
                    StrongThis->bStaticMeshLoadInFlight = false;
                    StrongThis->LoadedNodes.Remove(RequestedNode);
                    StrongThis->ReleasePrefabReferenceForNode(RequestedNode);
                    if (IsValid(StrongThis->GeneratedMeshWorldContext))
                    {
                        StrongThis->GeneratedMeshWorldContext->ReleaseWorldPin();
                    }
                    StrongThis->ResetLoadState();
                    FglTFRuntimeSafety::CompleteOperation(Ticket);
                    return;
                }
                StrongThis->PrefabUseNodesInFlight.Add(RequestedNode);
                StrongThis->ActivePrefabUseNode = RequestedNode;
                StrongThis->ActivePrefabUsePath = RequestedPrefabPath;
                StrongThis->ActivePrefabUseToken = RequestedPrefabToken;
            }

            StrongThis->GlTFRuntimeOperationTicket = Ticket;
            FglTFRuntimeStaticMeshAsync Callback;
            Callback.BindDynamic(StrongThis, &UStreamAsyncAction::SetStaticMesh);

            UE_LOG(LogTemp, Verbose,
                TEXT("Submitting runtime mesh build. Mesh=%s Node=%s LODCount=%d ComplexCollision=%s SimpleCollision=%s"),
                *RequestedMesh.ToString(),
                *RequestedNode.ToString(),
                RequestedIndices.Num(),
                RequestedConfig.bBuildComplexCollision ? TEXT("true") : TEXT("false"),
                RequestedConfig.bBuildSimpleCollision ? TEXT("true") : TEXT("false"));

            if (RequestedIndices.Num() == 1)
            {
                // The single-mesh API avoids the plugin's separate multi-LOD assembly path.
                CallbackMeshSourceAsset->LoadStaticMeshAsync(RequestedIndices[0], Callback, RequestedConfig);
            }
            else
            {
                CallbackMeshSourceAsset->LoadStaticMeshLODsAsync(RequestedIndices, Callback, RequestedConfig);
            }
        },
        [WeakThis, RequestedNode](const FString& Reason)
        {
            UStreamAsyncAction* StrongThis = WeakThis.Get();
            if (!IsValid(StrongThis))
            {
                return;
            }

            StrongThis->GlTFRuntimeOperationTicket = 0;
            StrongThis->bStaticMeshLoadInFlight = false;
            if (IsValid(StrongThis->GeneratedMeshWorldContext))
            {
                StrongThis->GeneratedMeshWorldContext->ReleaseWorldPin();
            }
            if (StrongThis->bAbortRequested)
            {
                // A queued request can be cancelled before the native callback ever runs.
                StrongThis->AbortAndRelease();
                return;
            }

            StrongThis->EndPrefabAssetUseForNode(RequestedNode);
            UE_LOG(LogTemp, Warning,
                TEXT("Stream mesh request rejected. Node=%s Reason=%s"),
                *RequestedNode.ToString(),
                *Reason);
            StrongThis->LoadedNodes.Remove(RequestedNode);
            StrongThis->NodeMap.Remove(RequestedNode);
            StrongThis->ResetLoadState();
        });

    // A cache hit may invoke the plugin callback before EnqueueOperation returns. Preserve the
    // callback's terminal state instead of restoring a stale ticket afterwards.
    if (bStaticMeshLoadInFlight && GlTFRuntimeOperationTicket == 0)
    {
        GlTFRuntimeOperationTicket = SubmittedTicket;
    }
}

void UStreamAsyncAction::AddTrasnform(const FName &Name, UInstancedStaticMeshComponent *ISMC)
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::AddTrasnform")))
    {
        return;
    }

    if (bAbortRequested || !IsValid(OwnerActor.Get()))
    {
        ResetLoadState();
        return;
    }

    if (FModelNodeData *NodeInfo = NodeMap.Find(Name))
    {
        if (!IsValid(ISMC) || NodeInfo->Transform.ContainsNaN() || NodeInfo->MeshName.IsNone())
        {
            LoadedNodes.Remove(Name);
            NodeMap.Remove(Name);
            ReleasePrefabReferenceForNode(Name);
            ResetLoadState();
            return;
        }
        const FTransform Transform = NodeInfo->Transform;
        ISMC->AddInstance(Transform);
        LoadedNodes.Emplace(Name);
        BroadcastProgress();

        if (const FModelMeshData *MeshData = MeshMap.Find(NodeInfo->MeshName))
        {
            SpawnStreamComponents(Name, *NodeInfo, MeshData->Data);
        }

        // The node is loaded, so remove and clean up any existing unload box.
        TObjectPtr<UBoxComponent> *UnloadBoxPtr = UnloadBoxMap.Find(Name);
        if (UnloadBoxPtr && IsValid(*UnloadBoxPtr))
        {
            FActorHelper::DestroyComponent(OwnerActor, *UnloadBoxPtr);
            UnloadBoxMap.Remove(Name);
        }
    }
    else
    {
        LoadedNodes.Remove(Name);
    }

    ResetLoadState();
}

void UStreamAsyncAction::SpawnStreamComponents(const FName &NodeName, const FModelNodeData &NodeInfo, const FMeshData &Data)
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::SpawnStreamComponents")))
    {
        return;
    }

    FComponentGroup *ExistingGroupPtr = DynamicComponentMap.Find(NodeName);
    if (ExistingGroupPtr && (ExistingGroupPtr->Colliders.Num() > 0 || ExistingGroupPtr->Lights.Num() > 0))
    {
        return;
    }

    FComponentGroup Group;
    UWorld *World = OwnerActor->GetWorld();
    if (!World)
        return;

    if (!bRenderOnly && Data.bSimpleCollision)
    {
        for (const FModelCollider &ColliderData : Data.Colliders)
        {
            UShapeComponent *NewShape = nullptr;
            FTransform ComponentWorldTransform = FTransform::Identity * NodeInfo.Transform;

            if (ColliderData.Collider == EColliderType::Box)
            {
                UBoxComponent *BoxComp = NewObject<UBoxComponent>(OwnerActor);
                if (BoxComp)
                {
                    BoxComp->SetBoxExtent(ColliderData.Size);
                    NewShape = BoxComp;
                }
            }
            else if (ColliderData.Collider == EColliderType::Sphere)
            {
                USphereComponent *SphereComp = NewObject<USphereComponent>(OwnerActor);
                if (SphereComp)
                {
                    SphereComp->SetSphereRadius(ColliderData.Size.X);
                    NewShape = SphereComp;
                }
            }
            else if (ColliderData.Collider == EColliderType::Capsule)
            {
                UCapsuleComponent *CapsuleComp = NewObject<UCapsuleComponent>(OwnerActor);
                if (CapsuleComp)
                {
                    CapsuleComp->SetCapsuleSize(ColliderData.Size.X, ColliderData.Size.Y);
                    NewShape = CapsuleComp;
                }
            }

            if (NewShape)
            {
                OwnerActor->AddInstanceComponent(NewShape);
                NewShape->AttachToComponent(OwnerActor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
                NewShape->SetWorldTransform(ComponentWorldTransform);
                NewShape->SetCollisionProfileName(TEXT("BlockAll"));
                NewShape->RegisterComponent();
                Group.Colliders.Add(NewShape);
            }
        }
    }

    for (const FLightData &LightData : Data.Lights)
    {
        UDynamicPointLightComponent *PointLight = NewObject<UDynamicPointLightComponent>(OwnerActor);
        if (PointLight)
        {
            OwnerActor->AddInstanceComponent(PointLight);
            PointLight->AttachToComponent(OwnerActor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
            FVector LightWorldLocation = NodeInfo.Transform.TransformPosition(LightData.Location);
            PointLight->SetWorldLocation(LightWorldLocation);
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
        DynamicComponentMap.Emplace(NodeName, Group);
    }
}

void UStreamAsyncAction::DestroyStreamComponents(const FName &NodeName)
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::DestroyStreamComponents")))
    {
        return;
    }

    FComponentGroup *GroupPtr = DynamicComponentMap.Find(NodeName);
    if (!GroupPtr)
        return;

    for (UShapeComponent *Shape : GroupPtr->Colliders)
    {
        if (IsValid(Shape))
        {
            FActorHelper::DestroyComponent(OwnerActor, Shape);
        }
    }
    GroupPtr->Colliders.Empty();

    for (ULightComponent *Light : GroupPtr->Lights)
    {
        if (IsValid(Light))
        {
            FActorHelper::DestroyComponent(OwnerActor, Light);
        }
    }
    GroupPtr->Lights.Empty();

    // Remove dynamic components immediately because unload-box checks are no longer inside the struct.
    DynamicComponentMap.Remove(NodeName);
}

void UStreamAsyncAction::ResetLoadState()
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::ResetLoadState")))
    {
        return;
    }

    bIsLoading = false;
    bStaticMeshLoadInFlight = false;
    GlTFRuntimeOperationTicket = 0;
    CurrentLoadingNode = NAME_None;
    CurrentLoadingMesh = NAME_None;
    BroadcastProgress();
}

void UStreamAsyncAction::BroadcastProgress()
{
    if (!EnsureStreamActionGameThread(TEXT("UStreamAsyncAction::BroadcastProgress")))
    {
        return;
    }

    if (TotalOperationCount <= 0)
    {
        Progress.Broadcast(1.0f);
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
    Progress.Broadcast(FMath::Clamp(
        static_cast<float>(CompletedOperations) / static_cast<float>(TotalOperationCount),
        0.0f,
        1.0f));
}
