// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "World/PlacementTypes.h"
#include "PrefabActor.generated.h"

class UBoxComponent;
class UglTFRuntimeAsset;
class UStaticMesh;


USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FPrefabActorConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Prefab")
    FString DisplayName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Prefab")
    bool bOverrideLocalTransform = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Prefab")
    FTransform LocalTransform = FTransform::Identity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Prefab")
    bool bEnableCollision = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Prefab")
    FString CollisionProfileName = TEXT("BlockAll");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Prefab")
    bool bSimulatePhysics = false;

    /** Optional authored rigid-body mass. A value <= 0 keeps Chaos auto-calculated mass. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Prefab", meta=(ClampMin="0.0"))
    float MassKg = 0.0f;
};

UCLASS(BlueprintType)
class GLTFSIMULATOR_API APrefabActor : public AActor
{
    GENERATED_BODY()

public:
    APrefabActor();

    UFUNCTION(BlueprintCallable, Category="Prefab")
    bool LoadPrefab(const FString& InFilePath, const FString& InObjectName);

    UFUNCTION(BlueprintCallable, Category="Prefab")
    void SetRenderOnlyMode(bool bInRenderOnlyMode);

    UFUNCTION(BlueprintPure, Category="Prefab")
    bool IsRenderOnlyMode() const { return bRenderOnlyMode; }

    UFUNCTION(BlueprintCallable, Category="Prefab")
    FPlacedObjectRecord ToPlacementRecord() const;

    UFUNCTION(BlueprintCallable, Category="Prefab")
    FString GetObjectName() const { return ObjectName; }

    UFUNCTION(BlueprintCallable, Category="Prefab")
    FString GetSourceFilePath() const { return SourceFilePath; }

    UFUNCTION(BlueprintCallable, Category="Prefab")
    FString GetBaseName() const { return BaseName; }

    UFUNCTION(BlueprintCallable, Category="Prefab")
    FString GetDisplayName() const { return Config.DisplayName.IsEmpty() ? BaseName : Config.DisplayName; }

    UFUNCTION(BlueprintCallable, Category="Prefab")
    FPrefabActorConfig GetPrefabConfig() const { return Config; }

    UFUNCTION(BlueprintCallable, Category="Prefab")
    bool IsPrefabLoaded() const { return bLoaded; }

protected:
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Destroyed() override;

private:

    /** Lightweight per-entity physics/collision proxy. Rendering is owned by UInstancedEntitySubsystem. */
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UBoxComponent> Root;

    UPROPERTY()
    TObjectPtr<UglTFRuntimeAsset> GltfAsset;


    UPROPERTY()
    TMap<int32, TObjectPtr<UStaticMesh>> MeshCache;


    UPROPERTY(ReplicatedUsing=OnRep_PrefabReplicationData)
    FString ReplicatedSourceFilePath;

    UPROPERTY(ReplicatedUsing=OnRep_PrefabReplicationData)
    FString ReplicatedObjectName;

    UFUNCTION()
    void OnRep_PrefabReplicationData();

    UPROPERTY()
    FString SourceFilePath;

    UPROPERTY()
    FString ObjectName;

    UPROPERTY()
    FString BaseName;

    UPROPERTY()
    FPrefabActorConfig Config;

    int32 InstancedRegistrationId = INDEX_NONE;
    FBox LoadedLocalBounds = FBox(ForceInit);
    bool bLoaded = false;
    bool bRenderOnlyMode = false;

    bool LoadConfigJson(const FString& JsonPath);
    void ApplyConfigToPhysicsProxy();
    UStaticMesh* LoadMeshByIndex(int32 MeshIndex);
    void ClearLoadedComponents();
    void ReleaseRuntimeResources();
};
