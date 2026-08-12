// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Model/InstancedEntityRenderActor.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"

AInstancedEntityRenderActor::AInstancedEntityRenderActor()
{
    PrimaryActorTick.bCanEverTick = false;
    SetActorEnableCollision(false);
    SetReplicates(false);

    Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    Root->SetMobility(EComponentMobility::Movable);
    SetRootComponent(Root);
}

void AInstancedEntityRenderActor::InitializeResource(const FString& InResourcePath)
{
    ResourcePath = InResourcePath;
    SetActorTransform(FTransform::Identity, false, nullptr, ETeleportType::TeleportPhysics);
}

UInstancedStaticMeshComponent* AInstancedEntityRenderActor::FindOrCreateMeshComponent(
    int32 MeshKey,
    UStaticMesh* Mesh,
    int32 StartCullDistance,
    int32 EndCullDistance)
{
    if (!IsValid(Mesh) || MeshKey == INDEX_NONE)
    {
        return nullptr;
    }

    if (TObjectPtr<UInstancedStaticMeshComponent>* Existing = MeshComponents.Find(MeshKey))
    {
        UInstancedStaticMeshComponent* Component = Existing->Get();
        if (IsValid(Component))
        {
            const int32 SafeStartCull = FMath::Max(0, StartCullDistance);
            const int32 SafeEndCull = FMath::Max(SafeStartCull, EndCullDistance);
            Component->SetCullDistances(SafeStartCull, SafeEndCull);
            return Component;
        }
    }

    UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(
        this,
        *FString::Printf(TEXT("InstancedMesh_%d"), MeshKey));
    if (!IsValid(Component))
    {
        return nullptr;
    }

    AddInstanceComponent(Component);
    Component->SetupAttachment(Root);
    Component->SetMobility(EComponentMobility::Movable);
    Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Component->SetGenerateOverlapEvents(false);
    Component->SetCanEverAffectNavigation(false);
    Component->SetCastShadow(true);
    Component->SetStaticMesh(Mesh);
    const int32 SafeStartCull = FMath::Max(0, StartCullDistance);
    Component->SetCullDistances(SafeStartCull, FMath::Max(SafeStartCull, EndCullDistance));
    Component->RegisterComponent();

    MeshComponents.Add(MeshKey, Component);
    CachedWorldTransforms.FindOrAdd(MeshKey);
    ActiveInstanceSlots.FindOrAdd(MeshKey);
    FreeInstanceIndices.FindOrAdd(MeshKey);
    ActiveInstanceCounts.FindOrAdd(MeshKey) = 0;
    return Component;
}

int32 AInstancedEntityRenderActor::AddMeshInstance(
    int32 MeshKey,
    UStaticMesh* Mesh,
    const FTransform& WorldTransform,
    int32 StartCullDistance,
    int32 EndCullDistance)
{
    if (WorldTransform.ContainsNaN())
    {
        return INDEX_NONE;
    }

    UInstancedStaticMeshComponent* Component = FindOrCreateMeshComponent(
        MeshKey,
        Mesh,
        StartCullDistance,
        EndCullDistance);
    if (!IsValid(Component))
    {
        return INDEX_NONE;
    }

    TArray<FTransform>& Transforms = CachedWorldTransforms.FindOrAdd(MeshKey);
    TArray<bool>& ActiveSlots = ActiveInstanceSlots.FindOrAdd(MeshKey);
    TArray<int32>& FreeSlots = FreeInstanceIndices.FindOrAdd(MeshKey);

    int32 InstanceIndex = INDEX_NONE;
    while (FreeSlots.Num() > 0 && InstanceIndex == INDEX_NONE)
    {
        const int32 CandidateIndex = FreeSlots.Pop(EAllowShrinking::No);
        if (Transforms.IsValidIndex(CandidateIndex)
            && ActiveSlots.IsValidIndex(CandidateIndex)
            && !ActiveSlots[CandidateIndex])
        {
            InstanceIndex = CandidateIndex;
        }
    }

    if (InstanceIndex != INDEX_NONE)
    {
        Transforms[InstanceIndex] = WorldTransform;
        ActiveSlots[InstanceIndex] = true;
        DirtyMeshKeys.Add(MeshKey);
    }
    else
    {
        InstanceIndex = Component->AddInstance(WorldTransform, true);
        if (InstanceIndex == INDEX_NONE)
        {
            return INDEX_NONE;
        }

        if (InstanceIndex != Transforms.Num())
        {
            // AddInstance is expected to append. Reject an unexpected index rather than corrupting
            // the entity-to-instance mapping used by the shared renderer.
            Component->RemoveInstance(InstanceIndex);
            return INDEX_NONE;
        }
        Transforms.Add(WorldTransform);
        ActiveSlots.Add(true);
    }

    ActiveInstanceCounts.FindOrAdd(MeshKey) += 1;
    return InstanceIndex;
}


bool AInstancedEntityRenderActor::RemoveMeshInstance(int32 MeshKey, int32 InstanceIndex)
{
    TObjectPtr<UInstancedStaticMeshComponent>* ComponentPtr = MeshComponents.Find(MeshKey);
    TArray<FTransform>* Transforms = CachedWorldTransforms.Find(MeshKey);
    TArray<bool>* ActiveSlots = ActiveInstanceSlots.Find(MeshKey);
    if (!ComponentPtr || !IsValid(ComponentPtr->Get()) || !Transforms || !ActiveSlots
        || !Transforms->IsValidIndex(InstanceIndex) || !ActiveSlots->IsValidIndex(InstanceIndex)
        || !(*ActiveSlots)[InstanceIndex])
    {
        return false;
    }

    // Keep ISM indices stable. Removing an Unreal instance can compact or swap storage depending
    // on engine settings, which would invalidate every other entity binding. A zero-scale free slot
    // is invisible and is reused by the next entity that needs this mesh.
    (*ActiveSlots)[InstanceIndex] = false;
    FTransform HiddenTransform = (*Transforms)[InstanceIndex];
    HiddenTransform.SetScale3D(FVector::ZeroVector);
    (*Transforms)[InstanceIndex] = HiddenTransform;
    FreeInstanceIndices.FindOrAdd(MeshKey).Add(InstanceIndex);
    ActiveInstanceCounts.FindOrAdd(MeshKey) = FMath::Max(0, ActiveInstanceCounts.FindRef(MeshKey) - 1);
    DirtyMeshKeys.Add(MeshKey);
    return true;
}


bool AInstancedEntityRenderActor::SetCachedInstanceTransform(
    int32 MeshKey,
    int32 InstanceIndex,
    const FTransform& WorldTransform)
{
    if (WorldTransform.ContainsNaN())
    {
        return false;
    }

    TArray<FTransform>* Transforms = CachedWorldTransforms.Find(MeshKey);
    const TArray<bool>* ActiveSlots = ActiveInstanceSlots.Find(MeshKey);
    if (!Transforms || !ActiveSlots || !Transforms->IsValidIndex(InstanceIndex)
        || !ActiveSlots->IsValidIndex(InstanceIndex) || !(*ActiveSlots)[InstanceIndex])
    {
        return false;
    }

    (*Transforms)[InstanceIndex] = WorldTransform;
    DirtyMeshKeys.Add(MeshKey);
    return true;
}


void AInstancedEntityRenderActor::FlushDirtyTransforms()
{
    if (DirtyMeshKeys.Num() == 0)
    {
        return;
    }

    for (const int32 MeshKey : DirtyMeshKeys)
    {
        const TObjectPtr<UInstancedStaticMeshComponent>* ComponentPtr = MeshComponents.Find(MeshKey);
        UInstancedStaticMeshComponent* Component = ComponentPtr ? ComponentPtr->Get() : nullptr;
        const TArray<FTransform>* Transforms = CachedWorldTransforms.Find(MeshKey);
        if (!IsValid(Component) || !Transforms || Transforms->Num() == 0)
        {
            continue;
        }

        Component->BatchUpdateInstancesTransforms(
            0,
            *Transforms,
            true,
            true,
            false);
    }

    DirtyMeshKeys.Empty();
}

UStaticMesh* AInstancedEntityRenderActor::FindMesh(int32 MeshKey) const
{
    const TObjectPtr<UInstancedStaticMeshComponent>* ComponentPtr = MeshComponents.Find(MeshKey);
    return ComponentPtr && IsValid(ComponentPtr->Get()) ? ComponentPtr->Get()->GetStaticMesh() : nullptr;
}

int32 AInstancedEntityRenderActor::GetInstanceCount(int32 MeshKey) const
{
    return ActiveInstanceCounts.FindRef(MeshKey);
}


int32 AInstancedEntityRenderActor::GetTotalInstanceCount() const
{
    int32 Total = 0;
    for (const TPair<int32, int32>& Pair : ActiveInstanceCounts)
    {
        Total += Pair.Value;
    }
    return Total;
}


void AInstancedEntityRenderActor::ReleaseRuntimeResources()
{
    for (TPair<int32, TObjectPtr<UInstancedStaticMeshComponent>>& Pair : MeshComponents)
    {
        UInstancedStaticMeshComponent* Component = Pair.Value.Get();
        if (!IsValid(Component))
        {
            continue;
        }

        if (UStaticMesh* Mesh = Component->GetStaticMesh())
        {
            Component->SetStaticMesh(nullptr);
            if (!Mesh->IsAsset())
            {
                Mesh->ClearFlags(RF_Public | RF_Standalone);
            }
        }

        Component->ClearInstances();
        Component->UnregisterComponent();
        Component->DestroyComponent();
    }

    MeshComponents.Empty();
    CachedWorldTransforms.Empty();
    ActiveInstanceSlots.Empty();
    FreeInstanceIndices.Empty();
    ActiveInstanceCounts.Empty();
    DirtyMeshKeys.Empty();
}

void AInstancedEntityRenderActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ReleaseRuntimeResources();
    Super::EndPlay(EndPlayReason);
}
