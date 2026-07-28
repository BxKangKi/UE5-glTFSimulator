// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Model/glTFMaterialAssetReferences.h"
#include "World/PlacementTypes.h"
#include "PrefabActor.generated.h"

class UglTFRuntimeAsset;
class UStaticMeshComponent;
class UStaticMesh;
class USceneComponent;


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
    /** glTFRuntime material assets assigned directly in the owning Blueprint/class defaults. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Prefab|Assets", meta=(AllowPrivateAccess="true"))
    FglTFMaterialAssetReferences MaterialAssets;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<USceneComponent> Root;

    UPROPERTY()
    TObjectPtr<UglTFRuntimeAsset> GltfAsset;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> MeshComponents;

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

    bool bLoaded = false;
    bool bRenderOnlyMode = false;

    bool LoadConfigJson(const FString& JsonPath);
    void ApplyConfigToMeshComponent(UStaticMeshComponent* MeshComponent) const;
    UStaticMesh* LoadMeshByIndex(int32 MeshIndex);
    void ClearLoadedComponents();
    void ReleaseRuntimeResources();
};
