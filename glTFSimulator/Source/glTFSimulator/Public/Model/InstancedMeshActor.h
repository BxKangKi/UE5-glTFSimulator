// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file InstancedMeshActor.h
 * 역할: 월드 씬의 동일 그룹 메시 인스턴스를 관리합니다.
 * 핵심 기능: ISM 슬롯, 그룹별 메시·부가 컴포넌트 수명.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
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
