// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file InstancedEntityRenderActor.h
 * 역할: 동일 모델의 동적 객체를 ISM으로 모아 렌더링합니다.
 * 핵심 기능: 모델별 인스턴스 추가·갱신·제거, 렌더 컴포넌트 관리.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "InstancedEntityRenderActor.generated.h"

class UInstancedStaticMeshComponent;
class USceneComponent;
class UStaticMesh;

/**
 * One render-only actor per built Static/model UUID.
 *
 * Every unique static mesh is represented by one plain ISM component and all entities using the
 * same built model contribute instances to those components. Plain ISM is intentional here so
 * visibility does not depend on a hierarchical cluster tree. Physics remains on lightweight entity
 * actors; this actor is exclusively responsible for batched rendering.
 */
UCLASS(NotBlueprintable, Transient)
class GLTFSIMULATOR_API AInstancedEntityRenderActor : public AActor
{
    GENERATED_BODY()

public:
    AInstancedEntityRenderActor();

    void InitializeResource(const FString& InResourcePath);

    int32 AddMeshInstance(
        int32 MeshKey,
        UStaticMesh* Mesh,
        const FTransform& WorldTransform,
        int32 StartCullDistance,
        int32 EndCullDistance);

    bool RemoveMeshInstance(int32 MeshKey, int32 InstanceIndex);
    bool SetCachedInstanceTransform(int32 MeshKey, int32 InstanceIndex, const FTransform& WorldTransform);
    void FlushDirtyTransforms();

    UStaticMesh* FindMesh(int32 MeshKey) const;
    int32 GetInstanceCount(int32 MeshKey) const;
    int32 GetTotalInstanceCount() const;
    const FString& GetResourcePath() const { return ResourcePath; }

    void ReleaseRuntimeResources();

protected:
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<USceneComponent> Root;

    UPROPERTY(Transient)
    TMap<int32, TObjectPtr<UInstancedStaticMeshComponent>> MeshComponents;

    TMap<int32, TArray<FTransform>> CachedWorldTransforms;
    TMap<int32, TArray<bool>> ActiveInstanceSlots;
    TMap<int32, TArray<int32>> FreeInstanceIndices;
    TMap<int32, int32> ActiveInstanceCounts;
    TSet<int32> DirtyMeshKeys;
    FString ResourcePath;

    UInstancedStaticMeshComponent* FindOrCreateMeshComponent(
        int32 MeshKey,
        UStaticMesh* Mesh,
        int32 StartCullDistance,
        int32 EndCullDistance);
    void TrimInactiveTail(int32 MeshKey, UInstancedStaticMeshComponent* Component);
};
