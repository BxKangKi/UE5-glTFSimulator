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
#include "Async/ParallelFor.h"
#include "Model/StreamActor.h"
#include "Model/SpawnActor.h"
#include "TimerManager.h"

UStreamAsyncAction *UStreamAsyncAction::StreamAsync(
    UObject *WorldContextObject,
    AStreamActor *Actor,
    const FVector &InPlayerLocation,
    const FglTFRuntimeStaticMeshConfig &StaticMeshConfig,
    float InDistance,
    int32 InChunkSize)
{
    auto *Action = NewObject<UStreamAsyncAction>();
    Action->WorldContextObject = WorldContextObject;
    if (IsValid(Actor))
    {
        Action->OwnerActor = Actor;
        Action->NodeMap = Actor->NodeMap;
        ASpawnActor *SpawnActor = Actor->SpawnActor;
        if (IsValid(SpawnActor))
        {
            Action->MeshMap = SpawnActor->GetAllMeshMap();
        }
        Action->DecalLight = Actor->DecalLight;
        Action->DynamicComponentMap = Actor->DynamicComponentMap;
        Action->UnloadBoxMap = Actor->UnloadBoxMap; // Actor로부터 UnloadBoxMap 연동
        Action->LoadedNodes = Actor->LoadedNodes;
        Action->InstanceMap = Actor->InstanceMap;
        Action->Asset = Actor->Asset;
    }
    Action->PlayerLocation = InPlayerLocation;
    Action->Distance = InDistance;
    Action->ChunkSize = InChunkSize;
    Action->StaticMeshConfig = StaticMeshConfig;
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}

void UStreamAsyncAction::Activate()
{
    if (!IsValid(OwnerActor) || !IsValid(WorldContextObject) || NodeMap.Num() == 0)
    {
        SetReadyToDestroy();
        return;
    }

    ChunkSize = FMath::Min(NodeMap.Num(), ChunkSize);

    PendingLoadNodes.Reset();
    PendingUnloadNodes.Reset();
    PendingLoadNodes.Reserve(ChunkSize);
    PendingUnloadNodes.Reserve(ChunkSize);

    FCriticalSection Mutex;
    const auto &NodesArray = NodeMap.Array();

    ParallelFor(NodesArray.Num(), [&](int32 Index)
                {
        const auto& NodePair = NodesArray[Index];
        const FModelNodeData& Info = NodePair.Value;

        const FModelMeshData* MeshPtr = MeshMap.Find(Info.MeshName);
        if (!MeshPtr) return;

        float MeshSize = MeshPtr->Size.Size();
        float CheckRadius = FMath::Square(MeshSize + (MeshSize * Distance));

        float CurrentDist = FVector::DistSquared(PlayerLocation, Info.Transform.GetLocation());
        bool bIsLoaded = LoadedNodes.Contains(NodePair.Key);

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

    CurrentLoadIndex = 0;
    CurrentUnloadIndex = 0;
    bIsLoading = false;

    ProcessChunk();
}

void UStreamAsyncAction::ProcessChunk()
{
    if (!IsValid(OwnerActor))
    {
        SetReadyToDestroy();
        return;
    }

    int32 UnloadEnd = FMath::Min(CurrentUnloadIndex + ChunkSize, PendingUnloadNodes.Num());
    for (int32 i = CurrentUnloadIndex; i < UnloadEnd; ++i)
    {
        ProcessUnloadNode(PendingUnloadNodes[i]);
    }
    CurrentUnloadIndex = UnloadEnd;

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
        !bIsLoading)
    {
        UWorld *World = OwnerActor->GetWorld();
        if (IsValid(World))
        {
            World->GetTimerManager().ClearTimer(ProcessTimerHandle);
        }
        FStreamAsyncWrapper Wrapper;
        Wrapper.NodeMap = MoveTemp(NodeMap);
        Wrapper.LoadedNodes = MoveTemp(LoadedNodes);
        Wrapper.InstanceMap = MoveTemp(InstanceMap);
        Wrapper.UnloadBoxMap = MoveTemp(UnloadBoxMap); // Wrapper 구조체로 데이터 이관
        Wrapper.DynamicComponentMap = MoveTemp(DynamicComponentMap); // Wrapper 구조체로 데이터 이관

        Completed.Broadcast(Wrapper);
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

bool UStreamAsyncAction::ProcessLoadNode(const FName &Name)
{
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
    FModelNodeData *Info = NodeMap.Find(Name);
    if (!Info)
        return;

    DestroyRuntimeComponents(Name);

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
            FActorHelper::DestroyComponent(OwnerActor, ISMC);
            InstanceMap.Remove(Info->MeshName);
        }
        LoadedNodes.Remove(Name);
    }

    // 분리 수정: UnloadBoxMap에서 대상 확인 및 생성 관리
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
    if (!IsValid(OwnerActor) || !IsValid(StaticMesh))
    {
        ResetLoadState();
        return;
    }

    UInstancedStaticMeshComponent *ISMC = InstanceMap.FindRef(CurrentLoadingMesh);
    if (!IsValid(ISMC))
    {
        ISMC = FActorHelper::AddStaticMeshComponent<UInstancedStaticMeshComponent>(
            OwnerActor, OwnerActor->GetTransform(), StaticMesh);
        if (IsValid(ISMC))
        {
            ISMC->SetRenderCustomDepth(true);
            ISMC->SetCustomDepthStencilValue(1);
            InstanceMap.Emplace(CurrentLoadingMesh, ISMC);
        }
    }

    if (IsValid(ISMC))
    {
        AddTrasnform(CurrentLoadingNode, ISMC);
    }
    else
    {
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

static bool ShouldInjectTerrainTextureArrayForMesh(const FName& MeshName)
{
    const FString MeshNameString = MeshName.ToString();
    return MeshNameString.Equals(TEXT("terrain"), ESearchCase::IgnoreCase)
        || MeshNameString.Contains(TEXT("terrain"), ESearchCase::IgnoreCase);
}

static UTexture* FindTerrainTextureArrayParam(const FglTFRuntimeStaticMeshConfig& Config)
{
    if (UTexture* const* Texture = Config.MaterialsConfig.CustomTextureParams.Find(TEXT("TerrainTextures")))
    {
        return *Texture;
    }
    return nullptr;
}

void UStreamAsyncAction::LoadStaticMeshAsync(const FName &MeshName)
{
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
        Config.bBuildComplexCollision = Mesh->Data.bComplexCollision;
        Config.bBuildSimpleCollision = Mesh->Data.bSimpleCollision;
        Config.LODScreenSize = LODScreenSize;
        Config.LODScreenSizeMultiplier = 1.0f;

        UTexture* TerrainTextureArray = FindTerrainTextureArrayParam(StaticMeshConfig);
        if (TerrainTextureArray && ShouldInjectTerrainTextureArrayForMesh(MeshName))
        {
            Config.MaterialsConfig.CustomTextureParams.Add(TEXT("baseColor"), TerrainTextureArray);
            Config.MaterialsConfig.CustomTextureParams.Add(TEXT("BaseColor"), TerrainTextureArray);
        }
        else
        {
            Config.MaterialsConfig.CustomTextureParams.Remove(TEXT("baseColor"));
            Config.MaterialsConfig.CustomTextureParams.Remove(TEXT("BaseColor"));
        }

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
    if (FModelNodeData *NodeInfo = NodeMap.Find(Name))
    {
        FTransform Transform = NodeInfo->Transform;
        ISMC->AddInstance(Transform);
        LoadedNodes.Emplace(Name);

        if (const FModelMeshData *MeshData = MeshMap.Find(NodeInfo->MeshName))
        {
            SpawnRuntimeComponents(Name, *NodeInfo, MeshData->Data);
        }

        // 분리 수정: 노드가 로드되었으므로 기존 UnloadBox가 있다면 제거 및 청소
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

void UStreamAsyncAction::SpawnRuntimeComponents(const FName &NodeName, const FModelNodeData &NodeInfo, const FRuntimeMeshData &Data)
{
    FRuntimeComponentGroup *ExistingGroupPtr = DynamicComponentMap.Find(NodeName);
    if (ExistingGroupPtr && (ExistingGroupPtr->Colliders.Num() > 0 || ExistingGroupPtr->Lights.Num() > 0))
    {
        return;
    }

    FRuntimeComponentGroup Group;
    UWorld *World = OwnerActor->GetWorld();
    if (!World)
        return;

    if (Data.bSimpleCollision)
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

    for (const FRuntimeLightData &LightData : Data.Lights)
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

void UStreamAsyncAction::DestroyRuntimeComponents(const FName &NodeName)
{
    FRuntimeComponentGroup *GroupPtr = DynamicComponentMap.Find(NodeName);
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

    // 구조체 내부에 UnloadBox 판별 로직이 빠졌으므로 동적 컴포넌트 정리 시 즉시 제거합니다.
    DynamicComponentMap.Remove(NodeName);
}

void UStreamAsyncAction::ResetLoadState()
{
    bIsLoading = false;
}
