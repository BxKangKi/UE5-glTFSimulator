// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file InstancedMeshActor.cpp
 * 역할: 월드 씬의 동일 그룹 메시 인스턴스를 관리합니다.
 * 핵심 기능: ISM 슬롯, 그룹별 메시·부가 컴포넌트 수명.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "Model/InstancedMeshActor.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/ShapeComponent.h"
#include "Engine/StaticMesh.h"

AInstancedMeshActor::AInstancedMeshActor()
{
    PrimaryActorTick.bCanEverTick = false;
    SetReplicates(false);

    Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    Root->SetMobility(EComponentMobility::Movable);
    SetRootComponent(Root);

    MeshComponent = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("InstancedMesh"));
    MeshComponent->SetupAttachment(Root);
    MeshComponent->SetMobility(EComponentMobility::Movable);
    MeshComponent->SetGenerateOverlapEvents(false);
    MeshComponent->SetCanEverAffectNavigation(false);
    MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    MeshComponent->SetCollisionResponseToAllChannels(ECR_Ignore);
}

bool AInstancedMeshActor::InitializeGroup(
    const FName InGroupName,
    TMap<FName, FModelNodeData>&& InNodes)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("InitializeGroup must run on the game thread"))
        || InGroupName.IsNone() || InNodes.IsEmpty())
    {
        return false;
    }

    GroupName = InGroupName;
    NodeMap = MoveTemp(InNodes);
    LoadedNodes.Empty();
    NodeInstanceIndices.Empty();
    FreeInstanceIndices.Empty();
    DynamicComponentMap.Empty();
    RebuildSpatialIndex();
    bRuntimeResourcesReleased = false;
    return true;
}

void AInstancedMeshActor::RebuildSpatialIndex()
{
    SpatialChunks.Reset();
    AlwaysLoadedNodeNames.Reset();
    ReferencedMeshNames.Reset();
    TSet<FName> MeshNames;
    for (const TPair<FName, FModelNodeData>& Pair : NodeMap)
    {
        const FModelNodeData& Node = Pair.Value;
        if (!Node.MeshName.IsNone()) MeshNames.Add(Node.MeshName);
        if (Node.bAlwaysLoaded)
        {
            AlwaysLoadedNodeNames.Add(Pair.Key);
            continue;
        }
        if (Node.FineChunk.X < 0 || Node.FineChunk.X >= 16
            || Node.FineChunk.Y < 0 || Node.FineChunk.Y >= 16
            || Node.FineChunk.Z < 0 || Node.FineChunk.Z >= 16)
        {
            // V4 metadata validation should prevent this. Keep malformed in always-loaded rather
            // than silently losing geometry if an in-memory authoring row reaches this actor.
            AlwaysLoadedNodeNames.Add(Pair.Key);
            continue;
        }
        SpatialChunks.FindOrAdd(Node.CoarseChunk).FindOrAdd(Node.FineChunk).Add(Pair.Key);
    }
    ReferencedMeshNames = MeshNames.Array();
    ReferencedMeshNames.Sort([](const FName A, const FName B) { return A.LexicalLess(B); });
    AlwaysLoadedNodeNames.Sort([](const FName A, const FName B) { return A.LexicalLess(B); });
}

void AInstancedMeshActor::BuildStreamNodeSnapshot(
    const FVector& PlayerWorldLocation,
    const FTransform& OwnerWorldTransform,
    const float WorldRadius,
    TMap<FName, FModelNodeData>& OutNodes) const
{
    check(IsInGameThread());
    OutNodes.Reset();
    if (!FMath::IsFinite(WorldRadius) || WorldRadius < 0.0f) return;

    // Loaded nodes must remain candidates even after leaving a bucket so they can be unloaded.
    OutNodes.Reserve(LoadedNodes.Num() + AlwaysLoadedNodeNames.Num() + 256);
    for (const FName Name : LoadedNodes)
        if (const FModelNodeData* Node = NodeMap.Find(Name)) OutNodes.Add(Name, *Node);
    for (const FName Name : AlwaysLoadedNodeNames)
        if (const FModelNodeData* Node = NodeMap.Find(Name)) OutNodes.Add(Name, *Node);

    constexpr double CoarseCm = 8192.0 * 100.0;
    constexpr double FineCm = 512.0 * 100.0;
    if (OwnerWorldTransform.ContainsNaN() || PlayerWorldLocation.ContainsNaN())
    {
        // Corrupt transforms must not silently disappear from the scene. Falling back to the full
        // node set costs one pass but keeps geometry recoverable and avoids undefined chunk math.
        OutNodes = NodeMap;
        return;
    }
    const FVector AbsScale = OwnerWorldTransform.GetScale3D().GetAbs();
    const double RawMinScale = FMath::Min3(AbsScale.X, AbsScale.Y, AbsScale.Z);
    if (!FMath::IsFinite(RawMinScale) || RawMinScale <= UE_SMALL_NUMBER)
    {
        OutNodes = NodeMap;
        return;
    }
    const double LocalRadius = static_cast<double>(WorldRadius) / RawMinScale;
    const FVector LocalPlayer = OwnerWorldTransform.InverseTransformPosition(PlayerWorldLocation);
    if (LocalPlayer.ContainsNaN() || !FMath::IsFinite(LocalRadius))
    {
        OutNodes = NodeMap;
        return;
    }

    auto FloorCell = [](const double Value, const double Cell)
    {
        return static_cast<int32>(FMath::Clamp<double>(FMath::FloorToDouble(Value / Cell), MIN_int32, MAX_int32));
    };
    const FIntVector MinCoarse(FloorCell(LocalPlayer.X - LocalRadius, CoarseCm),
        FloorCell(LocalPlayer.Y - LocalRadius, CoarseCm), FloorCell(LocalPlayer.Z - LocalRadius, CoarseCm));
    const FIntVector MaxCoarse(FloorCell(LocalPlayer.X + LocalRadius, CoarseCm),
        FloorCell(LocalPlayer.Y + LocalRadius, CoarseCm), FloorCell(LocalPlayer.Z + LocalRadius, CoarseCm));
    const FIntVector MinFine(FloorCell(LocalPlayer.X - LocalRadius, FineCm),
        FloorCell(LocalPlayer.Y - LocalRadius, FineCm), FloorCell(LocalPlayer.Z - LocalRadius, FineCm));
    const FIntVector MaxFine(FloorCell(LocalPlayer.X + LocalRadius, FineCm),
        FloorCell(LocalPlayer.Y + LocalRadius, FineCm), FloorCell(LocalPlayer.Z + LocalRadius, FineCm));

    const auto AddCoarseBucket = [this, &OutNodes, &MinFine, &MaxFine](
        const FIntVector& Coarse,
        const TMap<FIntVector, TArray<FName>>& FineBuckets)
    {
        constexpr int64 LocalFinePerCoarse = 16;
        for (const TPair<FIntVector, TArray<FName>>& FinePair : FineBuckets)
        {
            const int64 GlobalFineX = static_cast<int64>(Coarse.X) * LocalFinePerCoarse + FinePair.Key.X;
            const int64 GlobalFineY = static_cast<int64>(Coarse.Y) * LocalFinePerCoarse + FinePair.Key.Y;
            const int64 GlobalFineZ = static_cast<int64>(Coarse.Z) * LocalFinePerCoarse + FinePair.Key.Z;
            if (GlobalFineX < static_cast<int64>(MinFine.X) || GlobalFineX > static_cast<int64>(MaxFine.X)
                || GlobalFineY < static_cast<int64>(MinFine.Y) || GlobalFineY > static_cast<int64>(MaxFine.Y)
                || GlobalFineZ < static_cast<int64>(MinFine.Z) || GlobalFineZ > static_cast<int64>(MaxFine.Z)) continue;
            for (const FName Name : FinePair.Value)
                if (const FModelNodeData* Node = NodeMap.Find(Name)) OutNodes.Add(Name, *Node);
        }
    };

    const int64 SpanX = static_cast<int64>(MaxCoarse.X) - MinCoarse.X + 1;
    const int64 SpanY = static_cast<int64>(MaxCoarse.Y) - MinCoarse.Y + 1;
    const int64 SpanZ = static_cast<int64>(MaxCoarse.Z) - MinCoarse.Z + 1;
    const bool bCoordinateSpanIsSafe = SpanX > 0 && SpanY > 0 && SpanZ > 0
        && SpanX <= 64 && SpanY <= 64 && SpanZ <= 64
        && SpanX * SpanY * SpanZ <= 4096;

    if (bCoordinateSpanIsSafe)
    {
        // The normal streaming radius touches only a handful of 8192 m buckets. Direct hash
        // lookups make the query cost proportional to the requested area instead of world size.
        for (int64 X = MinCoarse.X; X <= static_cast<int64>(MaxCoarse.X); ++X)
        for (int64 Y = MinCoarse.Y; Y <= static_cast<int64>(MaxCoarse.Y); ++Y)
        for (int64 Z = MinCoarse.Z; Z <= static_cast<int64>(MaxCoarse.Z); ++Z)
        {
            const FIntVector Coarse(static_cast<int32>(X), static_cast<int32>(Y), static_cast<int32>(Z));
            if (const TMap<FIntVector, TArray<FName>>* FineBuckets = SpatialChunks.Find(Coarse))
                AddCoarseBucket(Coarse, *FineBuckets);
        }
    }
    else
    {
        // Extremely large radii can make coordinate enumeration expensive. In that unusual case,
        // inspect only occupied buckets and retain the same fine-cell filter.
        for (const TPair<FIntVector, TMap<FIntVector, TArray<FName>>>& CoarsePair : SpatialChunks)
        {
            const FIntVector& Coarse = CoarsePair.Key;
            if (Coarse.X < MinCoarse.X || Coarse.X > MaxCoarse.X
                || Coarse.Y < MinCoarse.Y || Coarse.Y > MaxCoarse.Y
                || Coarse.Z < MinCoarse.Z || Coarse.Z > MaxCoarse.Z) continue;
            AddCoarseBucket(Coarse, CoarsePair.Value);
        }
    }
}

UInstancedStaticMeshComponent* AInstancedMeshActor::AssignStaticMesh(UStaticMesh* Mesh)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("AssignStaticMesh must run on the game thread"))
        || !IsValid(MeshComponent) || !IsValid(Mesh))
    {
        return nullptr;
    }

    UStaticMesh* ExistingMesh = MeshComponent->GetStaticMesh();
    if (IsValid(ExistingMesh) && ExistingMesh != Mesh && !LoadedNodes.IsEmpty())
    {
        return nullptr;
    }
    if (ExistingMesh != Mesh)
    {
        MeshComponent->SetStaticMesh(Mesh);
    }
    return MeshComponent;
}

bool AInstancedMeshActor::AddNodeInstance(const FName NodeName, const FTransform& LocalTransform)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("AddNodeInstance must run on the game thread"))
        || !IsValid(MeshComponent) || !IsValid(MeshComponent->GetStaticMesh())
        || NodeName.IsNone() || LocalTransform.ContainsNaN() || !NodeMap.Contains(NodeName))
    {
        return false;
    }
    if (LoadedNodes.Contains(NodeName))
    {
        return true;
    }

    int32 InstanceIndex = INDEX_NONE;
    while (!FreeInstanceIndices.IsEmpty() && InstanceIndex == INDEX_NONE)
    {
        const int32 Candidate = FreeInstanceIndices.Pop(EAllowShrinking::No);
        if (Candidate >= 0 && Candidate < MeshComponent->GetNumInstances())
        {
            InstanceIndex = Candidate;
        }
    }

    if (InstanceIndex == INDEX_NONE)
    {
        InstanceIndex = MeshComponent->AddInstance(LocalTransform);
    }
    else if (!MeshComponent->UpdateInstanceTransform(
        InstanceIndex, LocalTransform, false, true, true))
    {
        FreeInstanceIndices.Add(InstanceIndex);
        return false;
    }

    if (InstanceIndex == INDEX_NONE)
    {
        return false;
    }
    NodeInstanceIndices.Add(NodeName, InstanceIndex);
    LoadedNodes.Add(NodeName);
    return true;
}

bool AInstancedMeshActor::RemoveNodeInstance(const FName NodeName)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("RemoveNodeInstance must run on the game thread"))
        || !IsValid(MeshComponent))
    {
        return false;
    }

    const int32* ExistingIndex = NodeInstanceIndices.Find(NodeName);
    if (!ExistingIndex)
    {
        LoadedNodes.Remove(NodeName);
        return false;
    }

    const int32 InstanceIndex = *ExistingIndex;
    FTransform HiddenTransform = FTransform::Identity;
    HiddenTransform.SetScale3D(FVector::ZeroVector);
    const bool bInstanceHidden = InstanceIndex >= 0 && InstanceIndex < MeshComponent->GetNumInstances()
        && MeshComponent->UpdateInstanceTransform(InstanceIndex, HiddenTransform, false, true, true);
    if (!bInstanceHidden)
    {
        return false;
    }

    NodeInstanceIndices.Remove(NodeName);
    LoadedNodes.Remove(NodeName);
    FreeInstanceIndices.Add(InstanceIndex);

    if (LoadedNodes.IsEmpty())
    {
        MeshComponent->ClearInstances();
        MeshComponent->SetStaticMesh(nullptr);
        NodeInstanceIndices.Empty();
        FreeInstanceIndices.Empty();
    }
    return true;
}

bool AInstancedMeshActor::HasDynamicComponents(const FName NodeName) const
{
    const FComponentGroup* Group = DynamicComponentMap.Find(NodeName);
    return Group && (!Group->Colliders.IsEmpty() || !Group->Lights.IsEmpty());
}

void AInstancedMeshActor::StoreDynamicComponents(const FName NodeName, FComponentGroup&& Group)
{
    check(IsInGameThread());
    if (!NodeName.IsNone())
    {
        DynamicComponentMap.Add(NodeName, MoveTemp(Group));
    }
}

void AInstancedMeshActor::DestroyDynamicComponents(const FName NodeName)
{
    check(IsInGameThread());
    FComponentGroup* Group = DynamicComponentMap.Find(NodeName);
    if (!Group)
    {
        return;
    }

    const auto DestroyOwnedComponent = [this](UActorComponent* Component)
    {
        if (IsValid(Component))
        {
            RemoveInstanceComponent(Component);
            Component->UnregisterComponent();
            Component->DestroyComponent();
        }
    };
    for (UShapeComponent* Collider : Group->Colliders)
    {
        DestroyOwnedComponent(Collider);
    }
    for (ULightComponent* Light : Group->Lights)
    {
        DestroyOwnedComponent(Light);
    }
    DynamicComponentMap.Remove(NodeName);
}

void AInstancedMeshActor::ReleaseRuntimeResources()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("InstancedMeshActor release must run on the game thread"))
        || bRuntimeResourcesReleased)
    {
        return;
    }
    bRuntimeResourcesReleased = true;

    TArray<FName> DynamicNodes;
    DynamicComponentMap.GetKeys(DynamicNodes);
    for (const FName NodeName : DynamicNodes)
    {
        DestroyDynamicComponents(NodeName);
    }
    if (IsValid(MeshComponent))
    {
        MeshComponent->ClearInstances();
        for (int32 Index = 0; Index < MeshComponent->GetNumMaterials(); ++Index)
        {
            MeshComponent->SetMaterial(Index, nullptr);
        }
        MeshComponent->SetStaticMesh(nullptr);
    }
    NodeMap.Empty();
    SpatialChunks.Empty();
    AlwaysLoadedNodeNames.Empty();
    ReferencedMeshNames.Empty();
    LoadedNodes.Empty();
    NodeInstanceIndices.Empty();
    FreeInstanceIndices.Empty();
    GroupName = NAME_None;
}

void AInstancedMeshActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ReleaseRuntimeResources();
    Super::EndPlay(EndPlayReason);
}

void AInstancedMeshActor::Destroyed()
{
    ReleaseRuntimeResources();
    Super::Destroyed();
}
