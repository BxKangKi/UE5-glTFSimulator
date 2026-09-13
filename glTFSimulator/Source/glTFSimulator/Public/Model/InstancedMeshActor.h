// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file InstancedMeshActor.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Model/ModelData.h"
#include "InstancedMeshActor.generated.h"

class UInstancedStaticMeshComponent;
class USceneComponent;
class UStaticMesh;

/**
 * One pre-created scene child actor for every unique GroupName.
 *
 * Node definitions are immutable after InitializeGroup(). Runtime instance slots and auxiliary
 * components are owned here, so concurrent stream actions never mutate another mesh group's state.
 */
UCLASS(NotBlueprintable, Transient)
class GLTFSIMULATOR_API AInstancedMeshActor final : public AActor
{
    GENERATED_BODY()

public:
    AInstancedMeshActor();

    bool InitializeGroup(
        FName InGroupName,
        TMap<FName, FModelNodeData>&& InNodes);

    FName GetGroupName() const { return GroupName; }
    const TMap<FName, FModelNodeData>& GetNodeMapRef() const { return NodeMap; }
    const TArray<FName>& GetReferencedMeshNames() const { return ReferencedMeshNames; }
    /** Copies only nearby 8192m/512m buckets plus loaded/always-loaded rows for one stream pass. */
    void BuildStreamNodeSnapshot(
        const FVector& PlayerWorldLocation,
        const FTransform& OwnerWorldTransform,
        float WorldRadius,
        TMap<FName, FModelNodeData>& OutNodes) const;
    bool IsNodeLoaded(FName NodeName) const { return LoadedNodes.Contains(NodeName); }
    UInstancedStaticMeshComponent* GetMeshComponent() const { return MeshComponent.Get(); }

    UInstancedStaticMeshComponent* AssignStaticMesh(UStaticMesh* Mesh);
    bool AddNodeInstance(FName NodeName, const FTransform& LocalTransform);
    bool RemoveNodeInstance(FName NodeName);

    bool HasDynamicComponents(FName NodeName) const;
    void StoreDynamicComponents(FName NodeName, FComponentGroup&& Group);
    void DestroyDynamicComponents(FName NodeName);

    void ReleaseRuntimeResources();

protected:
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Destroyed() override;

private:
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<USceneComponent> Root;

    /** Allocated in the constructor, before any UWorldSceneStreamAction is created. */
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UInstancedStaticMeshComponent> MeshComponent;

    UPROPERTY(Transient)
    TMap<FName, FModelNodeData> NodeMap;

    UPROPERTY(Transient)
    TSet<FName> LoadedNodes;

    UPROPERTY(Transient)
    TMap<FName, FComponentGroup> DynamicComponentMap;

    FName GroupName = NAME_None;
    TMap<FName, int32> NodeInstanceIndices;
    TArray<int32> FreeInstanceIndices;
    /** Native-only two-level spatial index: 8192 m coarse cell -> 512 m child cell -> node keys. */
    TMap<FIntVector, TMap<FIntVector, TArray<FName>>> SpatialChunks;
    TArray<FName> AlwaysLoadedNodeNames;
    TArray<FName> ReferencedMeshNames;
    void RebuildSpatialIndex();
    bool bRuntimeResourcesReleased = false;
};
