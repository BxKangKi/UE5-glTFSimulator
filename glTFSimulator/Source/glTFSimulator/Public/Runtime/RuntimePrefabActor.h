// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Runtime/RuntimePlacementTypes.h"
#include "RuntimePrefabActor.generated.h"

class UglTFRuntimeAsset;
class UStaticMeshComponent;
class UStaticMesh;
class USceneComponent;


USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FRuntimePrefabActorConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Prefab")
    FString DisplayName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Prefab")
    bool bOverrideLocalTransform = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Prefab")
    FTransform LocalTransform = FTransform::Identity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Prefab")
    bool bEnableCollision = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Prefab")
    FString CollisionProfileName = TEXT("BlockAll");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Prefab")
    bool bSimulatePhysics = false;
};

UCLASS(BlueprintType)
class GLTFSIMULATOR_API ARuntimePrefabActor : public AActor
{
    GENERATED_BODY()

public:
    ARuntimePrefabActor();

    UFUNCTION(BlueprintCallable, Category="Runtime Prefab")
    bool LoadPrefab(const FString& InFilePath, const FString& InRuntimeName);

    UFUNCTION(BlueprintCallable, Category="Runtime Prefab")
    FRuntimePlacedObjectRecord ToPlacementRecord() const;

    UFUNCTION(BlueprintCallable, Category="Runtime Prefab")
    FString GetRuntimeName() const { return RuntimeName; }

    UFUNCTION(BlueprintCallable, Category="Runtime Prefab")
    FString GetSourceFilePath() const { return SourceFilePath; }

    UFUNCTION(BlueprintCallable, Category="Runtime Prefab")
    FString GetBaseName() const { return BaseName; }

    UFUNCTION(BlueprintCallable, Category="Runtime Prefab")
    FString GetDisplayName() const { return Config.DisplayName.IsEmpty() ? BaseName : Config.DisplayName; }

    UFUNCTION(BlueprintCallable, Category="Runtime Prefab")
    FRuntimePrefabActorConfig GetPrefabConfig() const { return Config; }

    UFUNCTION(BlueprintCallable, Category="Runtime Prefab")
    bool IsPrefabLoaded() const { return bLoaded; }

protected:
    virtual void Destroyed() override;

private:
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<USceneComponent> Root;

    UPROPERTY()
    TObjectPtr<UglTFRuntimeAsset> RuntimeAsset;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> MeshComponents;

    UPROPERTY()
    TMap<int32, TObjectPtr<UStaticMesh>> MeshCache;

    UPROPERTY()
    FString SourceFilePath;

    UPROPERTY()
    FString RuntimeName;

    UPROPERTY()
    FString BaseName;

    UPROPERTY()
    FRuntimePrefabActorConfig Config;

    bool bLoaded = false;

    bool LoadConfigJson(const FString& JsonPath);
    void ApplyConfigToMeshComponent(UStaticMeshComponent* MeshComponent) const;
    UStaticMesh* LoadMeshByIndex(int32 MeshIndex);
    void ClearLoadedComponents();
};
