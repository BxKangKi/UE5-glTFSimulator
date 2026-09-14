// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file DynamicActor.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DynamicActor.generated.h"

class UBoxComponent;
class UWorldBakedModelAsset;
class UStaticMesh;


USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FDynamicActorConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamic")
    FString DisplayName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamic")
    bool bOverrideLocalTransform = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamic")
    FTransform LocalTransform = FTransform::Identity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamic")
    bool bEnableCollision = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamic")
    FString CollisionProfileName = TEXT("BlockAll");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamic")
    bool bSimulatePhysics = false;

    /** Optional authored rigid-body mass. A value <= 0 keeps Chaos auto-calculated mass. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dynamic", meta=(ClampMin="0.0"))
    float MassKg = 0.0f;
};

UCLASS(BlueprintType)
class GLTFSIMULATOR_API ADynamicActor : public AActor
{
    GENERATED_BODY()

public:
    ADynamicActor();

    /** Runtime-only load. InModelReference must resolve to an immutable .gwd member. */
    UFUNCTION(BlueprintCallable, Category="Dynamic")
    bool LoadDynamic(const FString& InModelReference, const FString& InObjectName);

    UFUNCTION(BlueprintCallable, Category="Dynamic")
    void SetRenderOnlyMode(bool bInRenderOnlyMode);

    UFUNCTION(BlueprintPure, Category="Dynamic")
    bool IsRenderOnlyMode() const { return bRenderOnlyMode; }

    UFUNCTION(BlueprintCallable, Category="Dynamic")
    FString GetObjectName() const { return ObjectName; }

    UFUNCTION(BlueprintCallable, Category="Dynamic")
    FString GetModelReference() const { return ModelReference; }

    /** Legacy Blueprint getter; the value is a gwd:// reference, not a source path. */
    UFUNCTION(BlueprintCallable, Category="Dynamic",
        meta=(DeprecatedFunction, DeprecationMessage="Use GetModelReference"))
    FString GetSourceFilePath() const { return ModelReference; }

    UFUNCTION(BlueprintCallable, Category="Dynamic")
    FString GetBaseName() const { return BaseName; }

    UFUNCTION(BlueprintCallable, Category="Dynamic")
    FString GetDisplayName() const { return Config.DisplayName.IsEmpty() ? BaseName : Config.DisplayName; }

    UFUNCTION(BlueprintCallable, Category="Dynamic")
    FDynamicActorConfig GetDynamicConfig() const { return Config; }

    UFUNCTION(BlueprintCallable, Category="Dynamic")
    bool IsDynamicLoaded() const { return bLoaded; }

protected:
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Destroyed() override;

private:

    /** Lightweight per-entity physics/collision proxy. Rendering is owned by UInstancedEntitySubsystem. */
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UBoxComponent> Root;

    UPROPERTY()
    /** Range-reading facade for one gwd:// model; it never contains or opens a source GLB. */
    TObjectPtr<UWorldBakedModelAsset> BakedAsset;


    UPROPERTY()
    TMap<int32, TObjectPtr<UStaticMesh>> MeshCache;

protected:
    // Kept non-private for UE 5.8 replication registration (DOREPLIFETIME accessibility check).
    UPROPERTY(ReplicatedUsing=OnRep_DynamicReplicationData)
    FString ReplicatedModelReference;

    UPROPERTY(ReplicatedUsing=OnRep_DynamicReplicationData)
    FString ReplicatedObjectName;

private:
    UFUNCTION()
    void OnRep_DynamicReplicationData();

    UPROPERTY()
    FString ModelReference;

    UPROPERTY()
    FString ObjectName;

    UPROPERTY()
    FString BaseName;

    UPROPERTY()
    FDynamicActorConfig Config;

    int32 InstancedRegistrationId = INDEX_NONE;
    FBox LoadedLocalBounds = FBox(ForceInit);
    bool bLoaded = false;
    bool bRenderOnlyMode = false;
    bool bRuntimeResourcesReleased = false;
    uint64 AsyncMeshLoadGeneration = 0;
    int32 PendingAsyncMeshLoads = 0;
    bool bAsyncMeshLoadFailed = false;
    bool bResumingAsyncMeshLoad = false;
    bool bDispatchingAsyncMeshPreload = false;
    FString PendingAsyncModelReference;
    FString PendingAsyncObjectName;

    bool LoadConfigJson(const FString& DefinitionJson);
    void ApplyConfigToPhysicsProxy();
    UStaticMesh* LoadMeshByIndex(int32 MeshIndex);
    bool BeginAsyncMeshPreload();
    void HandleAsyncMeshPreloadResult(uint64 Generation, int32 MeshIndex, UStaticMesh* Mesh);
    void FinishAsyncMeshPreload(uint64 Generation);
    void ClearLoadedComponents();
    void ReleaseRuntimeResources();
};
