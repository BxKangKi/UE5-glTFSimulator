// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Model/StreamAsyncAction.h"
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
#include "Async/ParallelFor.h"
#include "Model/glTFStreamActor.h"
#include "System/AssetManageSubSystem.h"
#include "TimerManager.h"
#include "World/WaterActor.h"

UStreamAsyncAction *UStreamAsyncAction::StreamAsync(
    UObject *WorldContextObject,
    AglTFStreamActor *Actor,
    const FVector &InPlayerLocation,
    const FglTFRuntimeStaticMeshConfig &StaticMeshConfig,
    float InDistance,
    int32 InChunkSize,
    bool bInRenderOnly)
{
    auto *Action = NewObject<UStreamAsyncAction>();
    Action->WorldContextObject = WorldContextObject;
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
    }
    Action->PlayerLocation = InPlayerLocation;
    Action->Distance = InDistance;
    Action->ChunkSize = InChunkSize;
    Action->StaticMeshConfig = StaticMeshConfig;
    Action->bRenderOnly = bInRenderOnly;
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}


void UStreamAsyncAction::CancelAndRelease()
{
    AbortAndRelease();
}

void UStreamAsyncAction::AbortAndRelease(UStaticMesh* OrphanedMesh)
{
    bAbortRequested = true;

    if (IsValid(OrphanedMesh) && !OrphanedMesh->IsAsset())
    {
        OrphanedMesh->ClearFlags(RF_Standalone);
        OrphanedMesh->MarkAsGarbage();
    }

    if (IsValid(Asset))
    {
        Asset->ClearCache();
    }

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

    Asset = nullptr;
    OwnerActor = nullptr;
    WorldContextObject = nullptr;
    PendingComp = nullptr;
    DecalLight = nullptr;
    CurrentLoadingNode = NAME_None;
    CurrentLoadingMesh = NAME_None;
    bIsLoading = false;
    bRenderOnly = false;
    SetReadyToDestroy();
}

void UStreamAsyncAction::Activate()
{
    bAbortRequested = false;

    if (!IsValid(OwnerActor) || !IsValid(WorldContextObject) || (NodeMap.Num() == 0 && WaterNodeMap.Num() == 0))
    {
        AbortAndRelease();
        return;
    }

    ChunkSize = FMath::Max(1, FMath::Min(FMath::Max(1, NodeMap.Num() + WaterNodeMap.Num()), ChunkSize));

    PendingLoadNodes.Reset();
    PendingUnloadNodes.Reset();
    PendingLoadWaterNodes.Reset();
    PendingUnloadWaterNodes.Reset();
    PendingLoadNodes.Reserve(ChunkSize);
    PendingUnloadNodes.Reserve(ChunkSize);
    PendingLoadWaterNodes.Reserve(FMath::Min(ChunkSize, WaterNodeMap.Num()));
    PendingUnloadWaterNodes.Reserve(FMath::Min(ChunkSize, WaterNodeMap.Num()));

    FCriticalSection Mutex;
    const auto &NodesArray = NodeMap.Array();

    ParallelFor(NodesArray.Num(), [&](int32 Index)
                {
        const auto& NodePair = NodesArray[Index];
        const FModelNodeData& Info = NodePair.Value;

        const FModelMeshData* MeshPtr = MeshMap.Find(Info.MeshName);
        if (!MeshPtr) return;

        const float MeshSize = MeshPtr->Size.Size();
        const float CheckRadius = FMath::Square(MeshSize + (MeshSize * Distance));

        const float CurrentDist = FVector::DistSquared(PlayerLocation, Info.Transform.GetLocation());
        const bool bIsLoaded = LoadedNodes.Contains(NodePair.Key);

        if (bIsLoaded)
        {
            if (CurrentDist > CheckRadius)
            {
                FScopeLock Lock(&Mutex);
                PendingUnloadNodes.Add(NodePair.Key);
            }
        }
        else
        {
            if (CurrentDist <= CheckRadius && CurrentLoadingNode != NodePair.Key)
            {
                FScopeLock Lock(&Mutex);
                PendingLoadNodes.Add(NodePair.Key);
            }
        } });

    const auto WaterArray = WaterNodeMap.Array();
    ParallelFor(WaterArray.Num(), [&](int32 Index)
                {
        const auto& WaterPair = WaterArray[Index];
        const float CurrentDist = FVector::DistSquared(PlayerLocation, WaterPair.Value.Transform.GetLocation());
        const float CheckRadius = GetWaterStreamRadiusSq(WaterPair.Value);
        const bool bIsLoaded = LoadedWaterNodes.Contains(WaterPair.Key);

        if (bIsLoaded)
        {
            if (CurrentDist > CheckRadius)
            {
                FScopeLock Lock(&Mutex);
                PendingUnloadWaterNodes.Add(WaterPair.Key);
            }
        }
        else if (CurrentDist <= CheckRadius)
        {
            FScopeLock Lock(&Mutex);
            PendingLoadWaterNodes.Add(WaterPair.Key);
        } });

    CurrentLoadIndex = 0;
    CurrentUnloadIndex = 0;
    CurrentLoadWaterIndex = 0;
    CurrentUnloadWaterIndex = 0;
    TotalOperationCount = PendingLoadNodes.Num() + PendingUnloadNodes.Num() + PendingLoadWaterNodes.Num() + PendingUnloadWaterNodes.Num();
    bIsLoading = false;
    BroadcastProgress();

    ProcessChunk();
}

void UStreamAsyncAction::ReleaseActionReferences()
{
    UWorld* World = IsValid(OwnerActor) ? OwnerActor->GetWorld() : (IsValid(WorldContextObject) ? WorldContextObject->GetWorld() : nullptr);
    if (World)
    {
        World->GetTimerManager().ClearTimer(ProcessTimerHandle);
    }

    Asset = nullptr;
    OwnerActor = nullptr;
    WorldContextObject = nullptr;
    PendingComp = nullptr;
    PendingLoadNodes.Empty();
    PendingUnloadNodes.Empty();
    PendingLoadWaterNodes.Empty();
    PendingUnloadWaterNodes.Empty();
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
    bAbortRequested = true;
    bRenderOnly = false;
}

void UStreamAsyncAction::ProcessChunk()
{
    if (bAbortRequested || !IsValid(OwnerActor))
    {
        AbortAndRelease();
        return;
    }

    const int32 WaterUnloadEnd = FMath::Min(CurrentUnloadWaterIndex + ChunkSize, PendingUnloadWaterNodes.Num());
    for (int32 i = CurrentUnloadWaterIndex; i < WaterUnloadEnd; ++i)
    {
        ProcessUnloadWaterNode(PendingUnloadWaterNodes[i]);
        BroadcastProgress();
    }
    CurrentUnloadWaterIndex = WaterUnloadEnd;

    int32 UnloadEnd = FMath::Min(CurrentUnloadIndex + ChunkSize, PendingUnloadNodes.Num());
    for (int32 i = CurrentUnloadIndex; i < UnloadEnd; ++i)
    {
        ProcessUnloadNode(PendingUnloadNodes[i]);
        BroadcastProgress();
    }
    CurrentUnloadIndex = UnloadEnd;

    const int32 WaterLoadEnd = FMath::Min(CurrentLoadWaterIndex + ChunkSize, PendingLoadWaterNodes.Num());
    for (int32 i = CurrentLoadWaterIndex; i < WaterLoadEnd; ++i)
    {
        ProcessLoadWaterNode(PendingLoadWaterNodes[i]);
        BroadcastProgress();
    }
    CurrentLoadWaterIndex = WaterLoadEnd;

    BroadcastProgress();

    if (!bIsLoading && CurrentLoadIndex < PendingLoadNodes.Num())
    {
        int32 EndIndex = FMath::Min(CurrentLoadIndex + ChunkSize, PendingLoadNodes.Num());
        for (int32 i = CurrentLoadIndex; i < EndIndex; ++i)
        {
            FName TargetNode = PendingLoadNodes[i];
            if (LoadedNodes.Contains(TargetNode))
            {
                CurrentLoadIndex++;
                continue;
            }
            if (ProcessLoadNode(TargetNode))
            {
                break;
            }
            CurrentLoadIndex++;
        }
    }

    if (CurrentLoadIndex >= PendingLoadNodes.Num() &&
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

    AWaterActor* WaterActor = World->SpawnActor<AWaterActor>(AWaterActor::StaticClass(), WaterInfo->Transform, Params);
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
    if (bAbortRequested)
    {
        return false;
    }

    if (FModelNodeData *Info = NodeMap.Find(Name))
    {
        if (LoadedNodes.Contains(Name))
            return false;

        UInstancedStaticMeshComponent *ISMC = InstanceMap.FindRef(Info->MeshName);
        if (IsValid(ISMC))
        {
            AddTrasnform(Name, ISMC);
            return false;
        }
        else
        {
            if (bIsLoading)
                return true;
            CurrentLoadingNode = Name;
            CurrentLoadingMesh = Info->MeshName;
            bIsLoading = true;
            LoadStaticMeshAsync(CurrentLoadingMesh);
            return true;
        }
    }
    return false;
}

void UStreamAsyncAction::ProcessUnloadNode(const FName &Name)
{
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
            if (UAssetManageSubSystem* AssetManager = UAssetManageSubSystem::Get(OwnerActor))
            {
                AssetManager->ReleaseStaticMesh(OwnerActor, ISMC->GetStaticMesh());
            }
            FActorHelper::DestroyComponent(OwnerActor, ISMC);
            InstanceMap.Remove(Info->MeshName);
        }
        LoadedNodes.Remove(Name);
    }

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
    if (bAbortRequested || !IsValid(OwnerActor) || !IsValid(StaticMesh))
    {
        AbortAndRelease(StaticMesh);
        return;
    }

    UStaticMesh* MeshToUse = StaticMesh;
    if (UAssetManageSubSystem* AssetManager = UAssetManageSubSystem::Get(OwnerActor))
    {
        MeshToUse = AssetManager->AcquireStaticMesh(OwnerActor, CurrentLoadingMesh, StaticMesh);
    }

    UInstancedStaticMeshComponent *ISMC = InstanceMap.FindRef(CurrentLoadingMesh);
    if (!IsValid(ISMC))
    {
        ISMC = FActorHelper::AddStaticMeshComponent<UInstancedStaticMeshComponent>(
            OwnerActor, OwnerActor->GetTransform(), MeshToUse);
        if (IsValid(ISMC))
        {
            ISMC->SetRenderCustomDepth(true);
            ISMC->SetCustomDepthStencilValue(1);
            if (bRenderOnly)
            {
                ISMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
                ISMC->SetGenerateOverlapEvents(false);
            }
            InstanceMap.Emplace(CurrentLoadingMesh, ISMC);
        }
    }

    if (IsValid(ISMC) && bRenderOnly)
    {
        ISMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        ISMC->SetGenerateOverlapEvents(false);
    }

    if (IsValid(ISMC))
    {
        AddTrasnform(CurrentLoadingNode, ISMC);
    }
    else
    {
        if (MeshToUse != StaticMesh)
        {
            if (UAssetManageSubSystem* AssetManager = UAssetManageSubSystem::Get(OwnerActor))
            {
                AssetManager->ReleaseStaticMesh(OwnerActor, MeshToUse);
            }
        }
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
    if (bAbortRequested || !IsValid(Asset))
    {
        AbortAndRelease();
        return;
    }

    if (FModelMeshData *Mesh = MeshMap.Find(MeshName))
    {
        TArray<int32> LocalIndices;
        if (Mesh->LOD0 != INDEX_NONE)
            LocalIndices.Add(Mesh->LOD0);
        if (Mesh->LOD1 != INDEX_NONE)
            LocalIndices.Add(Mesh->LOD1);
        if (Mesh->LOD2 != INDEX_NONE)
            LocalIndices.Add(Mesh->LOD2);
        if (Mesh->LOD3 != INDEX_NONE)
            LocalIndices.Add(Mesh->LOD3);

        int32 Count = LocalIndices.Num();
        TMap<int32, float> LODScreenSize;
        for (int32 i = 0; i < Count; i++)
        {
            LODScreenSize.Add(i, CalculateLODScreenSize(i, Count));
        }

        FglTFRuntimeStaticMeshConfig Config = StaticMeshConfig;
        Config.bBuildComplexCollision = !bRenderOnly && Mesh->Data.bComplexCollision;
        Config.bBuildSimpleCollision = !bRenderOnly && Mesh->Data.bSimpleCollision;
        Config.bBuildNavCollision = !bRenderOnly && Config.bBuildNavCollision;
        Config.bBuildLumenCards = !bRenderOnly && Config.bBuildLumenCards;
        if (bRenderOnly)
        {
            Config.CollisionComplexity = ECollisionTraceFlag::CTF_UseDefault;
        }
        Config.LODScreenSize = LODScreenSize;
        Config.LODScreenSizeMultiplier = 1.0f;

        FglTFRuntimeStaticMeshAsync Callback;
        Callback.BindDynamic(this, &UStreamAsyncAction::SetStaticMesh);
        Asset->LoadStaticMeshLODsAsync(LocalIndices, Callback, Config);
    }
    else
    {
        ResetLoadState();
    }
}

void UStreamAsyncAction::AddTrasnform(const FName &Name, UInstancedStaticMeshComponent *ISMC)
{
    if (bAbortRequested || !IsValid(OwnerActor.Get()))
    {
        ResetLoadState();
        return;
    }

    if (FModelNodeData *NodeInfo = NodeMap.Find(Name))
    {
        FTransform Transform = NodeInfo->Transform;
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
        UE_LOG(LogTemp, Warning, TEXT("AddTrasnform: Node '%s' not found in NodeMap"), *Name.ToString());
    }

    ResetLoadState();
}

void UStreamAsyncAction::SpawnStreamComponents(const FName &NodeName, const FModelNodeData &NodeInfo, const FMeshData &Data)
{
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
    bIsLoading = false;
    BroadcastProgress();
}

void UStreamAsyncAction::BroadcastProgress()
{
    if (TotalOperationCount <= 0)
    {
        Progress.Broadcast(1.0f);
        return;
    }

    const int32 CompletedOperations = FMath::Clamp(CurrentUnloadIndex + CurrentLoadIndex + CurrentUnloadWaterIndex + CurrentLoadWaterIndex, 0, TotalOperationCount);
    Progress.Broadcast(FMath::Clamp(static_cast<float>(CompletedOperations) / static_cast<float>(TotalOperationCount), 0.0f, 1.0f));
}
