// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file WorldSceneStreamAction.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeCounter.h"
#include "Model/ModelData.h"
#include "glTFRuntimeAsset.h"
#include "WorldSceneStreamAction.generated.h"

class AStaticActor;
class AInstancedMeshActor;
class UMaterialDefaultRuntimeCache;
class UStaticMesh;
class UInstancedStaticMeshComponent;
class UBoxComponent;
class UShapeComponent;
class ULightComponent;
class AWaterActor;
class UWorld;
class UWorldBakedModelAsset;
struct FWorldSceneStreamPlan;

/**
 * Stable async-build outer for runtime static meshes.
 *
 * glTFRuntime queries StaticMesh->GetWorld() while finalizing complex collision. A weak world
 * reference is retained for completed meshes, while PinWorldForBuild() temporarily holds a strong
 * reference for the exact lifetime of one native collision build. This prevents map teardown or GC
 * from nulling the world halfway through Chaos cooking without making completed meshes retain it.
 */
UCLASS(Transient)
class GLTFSIMULATOR_API UWorldGeneratedStaticMeshContext : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(UWorld* InWorld);
    bool PinWorldForBuild();
    void ReleaseWorldPin();
    virtual UWorld* GetWorld() const override;

private:
    UPROPERTY(Transient)
    TObjectPtr<UWorld> PinnedWorld;

    UPROPERTY(Transient)
    TWeakObjectPtr<UWorld> World;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FWorldSceneStreamCompleted,
    const FWorldSceneStreamResult&, Result);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FWorldSceneStreamProgress,
    FName, GroupName,
    float, Progress);

UCLASS()
class GLTFSIMULATOR_API UWorldSceneStreamAction : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintAssignable)
    FWorldSceneStreamCompleted Completed;

    UPROPERTY(BlueprintAssignable)
    FWorldSceneStreamProgress Progress;

    UFUNCTION(BlueprintCallable, meta = (WorldContext = "WorldContextObject"))
    static UWorldSceneStreamAction *StreamAsync(
        UObject *WorldContextObject,
        AStaticActor *Actor,
        AInstancedMeshActor *InMeshActor,
        const FVector &InPlayerLocation,
        const FglTFRuntimeStaticMeshConfig &StaticMeshConfig,
        float InDistance = 65536.0f,
        int32 InChunkSize = 256,
        bool bInRenderOnly = false,
        bool bInWaterGroup = false,
        float InUnloadDistanceMultiplier = 1.0f);

    UFUNCTION(BlueprintCallable, Category="World Streaming")
    void Activate();

    void CancelAndRelease();

private:
    UPROPERTY()
    TObjectPtr<UObject> WorldContextObject;

    /** Shared GC guard retained until all native glTFRuntime callbacks have drained. */
    UPROPERTY(Transient)
    TObjectPtr<UMaterialDefaultRuntimeCache> MaterialReferenceGuard;

    /** World-aware outer retained for every static mesh build owned by this action. */
    UPROPERTY(Transient)
    TObjectPtr<UWorldGeneratedStaticMeshContext> GeneratedMeshWorldContext;
    UPROPERTY()
    TMap<FName, FModelNodeData> NodeMap;
    UPROPERTY()
    TMap<FName, FWaterStreamNodeData> WaterNodeMap;
    UPROPERTY()
    TMap<FName, FModelMeshData> MeshMap;
    UPROPERTY()
    TSet<FName> LoadedWaterNodes;
    UPROPERTY()
    TSubclassOf<AWaterActor> WaterClass;

    UPROPERTY()
    TMap<FName, TObjectPtr<AWaterActor>> WaterActorMap;

    UPROPERTY()
    TObjectPtr<UMaterialInterface> DecalLight;
    UPROPERTY()
    TArray<FName> PendingLoadNodes;
    UPROPERTY()
    TArray<FName> PendingUnloadNodes;
    UPROPERTY()
    TArray<FName> PendingLoadWaterNodes;
    UPROPERTY()
    TArray<FName> PendingUnloadWaterNodes;
    UPROPERTY()
    TObjectPtr<UWorldBakedModelAsset> Asset;
    UPROPERTY()
    TObjectPtr<AStaticActor> OwnerActor;
    UPROPERTY()
    TObjectPtr<AInstancedMeshActor> MeshActor;
    bool bIsLoading = false;
    bool bAbortRequested = false;
    bool bStaticMeshLoadInFlight = false;
    bool bPreparationInFlight = false;
    uint32 PreparationSerial = 0;

    bool bRenderOnly = false;
    FName GroupName = NAME_None;
    bool bWaterGroup = false;
    bool bGroupFailed = false;
    FName CurrentLoadingNode;
    FName CurrentLoadingMesh;

    int32 CurrentLoadIndex;
    int32 CurrentUnloadIndex;
    int32 CurrentLoadWaterIndex = 0;
    int32 CurrentUnloadWaterIndex = 0;
    int32 ChunkSize;
    /** All model/water nodes evaluated this pass, including no-op and sanitized nodes. */
    int32 TotalOperationCount = 0;
    /** Nodes that require no UObject work still advance as progress-only work over several ticks. */
    int32 TotalSkippedOperationCount = 0;
    int32 CurrentSkippedOperationIndex = 0;
    int32 SkippedProgressChunkSize = 1;

    FTimerHandle ProcessTimerHandle;
    FglTFRuntimeStaticMeshConfig StaticMeshConfig;
    FVector PlayerLocation;
    float Distance;
    float UnloadDistanceMultiplier = 1.0f;

    UFUNCTION()
    void SetStaticMesh(UStaticMesh *StaticMesh);

    void ProcessChunk();
    void StartStreamPlanAsync();
    void ApplyPreparedPlan(FWorldSceneStreamPlan&& Plan, uint32 Serial);
    void AbortAndRelease(UStaticMesh* OrphanedMesh = nullptr);
    void ResetLoadState();
    void LoadStaticMeshAsync(const FName &Name);
    void AddTransform(const FName &Name);
    void ProcessUnloadNode(const FName &Name);
    bool ProcessLoadNode(const FName &Name);
    void ProcessUnloadWaterNode(const FName &Name);
    void ProcessLoadWaterNode(const FName &Name);

    void SpawnStreamComponents(const FName &NodeName, const FModelNodeData &NodeInfo, const FMeshData &Data);
    void DestroyStreamComponents(const FName &NodeName);
    void BroadcastProgress();
    void ReleaseActionReferences();
};
