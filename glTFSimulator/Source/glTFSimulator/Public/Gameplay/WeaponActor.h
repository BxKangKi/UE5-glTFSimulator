// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WeaponActor.generated.h"

class UglTFRuntimeAsset;
class UStaticMeshComponent;
class UStaticMesh;
class USceneComponent;
class UCameraComponent;

USTRUCT(BlueprintType)
struct FWeaponConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FTransform HoldTransform = FTransform(FRotator(0.0f, 0.0f, 0.0f), FVector(45.0f, 18.0f, -18.0f), FVector(1.0f));

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FVector MuzzleOffset = FVector(70.0f, 0.0f, 0.0f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float Range = 20000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float Damage = 20.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float FireInterval = 0.12f;
};

UCLASS(BlueprintType)
class GLTFSIMULATOR_API AWeaponActor : public AActor
{
    GENERATED_BODY()

public:
    AWeaponActor();

    UFUNCTION(BlueprintCallable, Category="Weapon")
    bool EquipFromFile(const FString& InFilePath, USceneComponent* AttachTarget);

    UFUNCTION(BlueprintCallable, Category="Weapon")
    void Fire(AController* InstigatorController);

    UFUNCTION(BlueprintPure, Category="Weapon")
    FString GetSourceFilePath() const { return SourceFilePath; }

protected:
    virtual void Destroyed() override;

private:
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<USceneComponent> Root;

    UPROPERTY()
    TObjectPtr<UglTFRuntimeAsset> GltfAsset;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> MeshComponents;

    UPROPERTY()
    TMap<int32, TObjectPtr<UStaticMesh>> MeshCache;

    UPROPERTY()
    FString SourceFilePath;

    FWeaponConfig Config;
    double LastFireTime = -1000.0;

    bool LoadConfigJson(const FString& JsonPath);
    bool LoadWeaponMesh();
    UStaticMesh* LoadMeshByIndex(int32 MeshIndex);
    FVector GetMuzzleWorldLocation() const;
    void ClearLoadedComponents();
};
