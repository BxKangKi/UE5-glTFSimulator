// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file InstancedEntityRenderActor.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
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
