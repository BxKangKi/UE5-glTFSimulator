// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file StaticActor.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Model/ModelData.h"
#include "glTFRuntimeParser.h"
#include "StaticActor.generated.h"

class AInstancedMeshActor;
class AWaterActor;
class FGWorldArchiveReader;
class UGameUpdateSubSystem;
class UMaterialInterface;
class USceneComponent;
class UWorldBakedModelAsset;
class UWorldSceneStreamAction;
struct FGWorldModelMetadata;
struct FGWorldModelManifest;

/**
 * Runtime representation of one Static model stored in the active .gworld.
 *
 * This actor has no filename-loading or source-build mode. Init accepts only a gworld:// UUID,
 * range-reads baked metadata, and schedules independently stored mesh/material/texture .dat
 * members as the player moves. Source GLBs are owned exclusively by UWorldSourceModelBuilder.
 */
UCLASS()
class GLTFSIMULATOR_API AStaticActor : public AActor
{
    GENERATED_BODY()

public:
    AStaticActor();

    /** Assigns the immutable gworld:// model reference before FinishSpawning for WorldStream use. */
    void Init(const FString& ModelReference);

    /** Loads a placeable Static model. Uses the same immutable .gworld payload as WorldStream. */
    UFUNCTION(BlueprintCallable, Category="Static")
    bool LoadStatic(const FString& InModelReference, const FString& InObjectName);

    UFUNCTION(BlueprintPure, Category="Static")
    FString GetObjectName() const { return ObjectName; }

    UFUNCTION(BlueprintPure, Category="Static")
    FString GetBaseName() const { return BaseName; }

    UFUNCTION(BlueprintPure, Category="World|Streaming")
    bool GetIsLoaded() const { return bIsLoaded; }

    UFUNCTION(BlueprintPure, Category="World|Streaming")
    FString GetModelReference() const { return ModelReference; }

    UFUNCTION(BlueprintPure, Category="World|Streaming")
    float GetLoadingStatus() const { return LoadingStatus; }

    UWorldBakedModelAsset* GetBakedAsset() const { return BakedAsset.Get(); }
    void ReleaseRuntimeResourcesForWorldExit();
    void SetRenderOnlyStreaming(bool bRenderOnly);
    bool IsRenderOnlyStreaming() const { return bRenderOnlyStreaming; }

    const TMap<FName, FModelMeshData>& GetAllMeshMapRef() const { return AllMeshMap; }
    const TMap<FName, FWaterStreamNodeData>& GetWaterNodeMapRef() const { return WaterNodeMap; }
    const TSet<FName>& GetLoadedWaterNodesRef() const { return LoadedWaterNodes; }
    const TMap<FName, TObjectPtr<AWaterActor>>& GetWaterActorMapRef() const { return WaterActorMap; }
    UMaterialInterface* GetDecalLight() const { return DecalLight.Get(); }
    TSubclassOf<AWaterActor> GetWaterClass() const { return WaterClass; }

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="World|Streaming", meta=(ClampMin="1"))
    int32 ChunkSize = 256;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="World|Streaming", meta=(ClampMin="1.0"))
    float StreamDistance = 64.0f;

    UPROPERTY(Transient, BlueprintReadOnly, Category="World|Rendering")
    TObjectPtr<UMaterialInterface> DecalLight;

    UPROPERTY(Transient, BlueprintReadOnly, Category="World|Rendering")
    TSubclassOf<AWaterActor> WaterClass;

    UFUNCTION()
    void OnStreamProgress(FName GroupName, float Progress);

    UFUNCTION()
    void OnStreamCompleted(const FWorldSceneStreamResult& Result);

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Destroyed() override;

private:
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<USceneComponent> Root;

    /** Lightweight .gworld facade. It contains node/range tables but no source bytes. */
    UPROPERTY(Transient)
    TObjectPtr<UWorldBakedModelAsset> BakedAsset;

    UPROPERTY(Transient)
    TMap<FName, TObjectPtr<UWorldSceneStreamAction>> ActiveStreamActions;

    UPROPERTY(Transient)
    TMap<FName, TObjectPtr<AInstancedMeshActor>> OwnedInstancedMeshActors;

    UPROPERTY(Transient)
    TMap<FName, FModelNodeData> AllNodeMap;

    UPROPERTY(Transient)
    TMap<FName, FModelMeshData> AllMeshMap;

    UPROPERTY(Transient)
    TMap<FName, FWaterStreamNodeData> WaterNodeMap;

    UPROPERTY(Transient)
    TSet<FName> LoadedWaterNodes;

    UPROPERTY(Transient)
    TMap<FName, TObjectPtr<AWaterActor>> WaterActorMap;

    /** Worker-prepared groups awaiting bounded game-thread actor construction. */
    TMap<FName, FInstancedMeshGroupInitData> PendingInstancedGroups;
    TArray<FName> PendingInstancedGroupNames;
    int32 PendingInstancedGroupIndex = 0;

    /** One streaming update is launched in bounded mesh-group batches to avoid a single-frame UObject burst. */
    TArray<FName> PendingStreamGroupNames;
    int32 PendingStreamGroupIndex = 0;
    bool bPendingWaterStream = false;

    UPROPERTY(Transient)
    FModelData ModelMetadata;

    TMap<FName, float> StreamGroupProgress;
    FString ModelReference;
    FString ObjectName;
    FString BaseName;
    uint64 MetadataRequestSerial = 0;
    int32 GameUpdateTickHandle = INDEX_NONE;
    float LoadingStatus = 0.0f;
    bool bIsLoaded = false;
    bool bAsyncLoading = false;
    bool bRenderOnlyStreaming = false;
    bool bHasModelMetadata = false;
    bool bIsDestroyed = false;
    bool bRuntimeResourcesReleased = false;

    void StartBuiltLoad();
    void LoadBuiltMetadataAsync(const FGuid& UUID);
    void OnBuiltMetadataLoaded(
        uint64 RequestSerial,
        const FString& ExpectedReference,
        const FGuid& UUID,
        const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe>& Reader,
        bool bSuccess,
        FGWorldModelMetadata&& Metadata,
        FGWorldModelManifest&& Manifest,
        FString&& Error);
    void CancelActiveStreamActions();
    void ReleaseStreamingResources();
    void RegisterGameUpdate();
    void UnregisterGameUpdate();
    void UpdateStreaming(float DeltaSeconds);
    void StartStreaming();
    void StartStreamingStep();
    void LaunchNextStreamingBatch();
    void FinishStreamingCycle();
    void BeginBuildInstancedMeshActors();
    void OnInstancedMeshGroupsPrepared(
        uint64 RequestSerial,
        const FString& ExpectedReference,
        TMap<FName, FInstancedMeshGroupInitData>&& Groups,
        int32 InvalidNodeCount);
    void BuildInstancedMeshActorsStep();
    void FinishBuildInstancedMeshActors();
    void RemoveInstancedMeshGroup(FName GroupName);
    void PruneUnreferencedMeshMetadata();
    void ReleaseInstancedMeshActors();
    bool IsPlayerInsideModelRange() const;
    void WriteLogAsync(const FString& Message) const;
    FglTFRuntimeStaticMeshConfig BuildStreamingMeshConfig();
};
