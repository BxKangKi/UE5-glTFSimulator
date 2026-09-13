// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WeaponActor.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WeaponActor.generated.h"

class UWorldBakedModelAsset;
struct FResolvedRuntimeModel;
class UStaticMeshComponent;
class UStaticMesh;
class USceneComponent;
class AWeaponProjectileActor;
class AController;

USTRUCT(BlueprintType)
struct FWeaponConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
    FString Version = TEXT("1.0.0");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
    FName AttachSocketName = TEXT("rightHand");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
    FTransform HoldTransform = FTransform(FRotator(0.0f, 0.0f, 0.0f), FVector(45.0f, 18.0f, -18.0f), FVector(1.0f));

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
    FTransform RightHandIK = FTransform(FRotator::ZeroRotator, FVector(20.0f, 8.0f, -4.0f), FVector::OneVector);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
    FTransform LeftHandIK = FTransform(FRotator::ZeroRotator, FVector(65.0f, -9.0f, -4.0f), FVector::OneVector);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
    FVector MuzzleOffset = FVector(95.0f, 0.0f, 0.0f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
    float Range = 20000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
    float Damage = 20.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
    float ImpactImpulse = 24000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
    float FireInterval = 0.12f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
    float TraceRadius = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
    bool bProjectile = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
    float ProjectileSpeed = 6500.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Weapon")
    float ProjectileLifeSeconds = 5.0f;
};

UCLASS(BlueprintType)
class GLTFSIMULATOR_API AWeaponActor : public AActor
{
    GENERATED_BODY()

public:
    AWeaponActor();

    /** Equips a model addressed by its immutable gwd:// reference. */
    UFUNCTION(BlueprintCallable, Category="Weapon")
    bool EquipFromModel(const FString& InModelReference, USceneComponent* AttachTarget);

    /** Legacy Blueprint name; accepts only a built gwd:// reference and never opens a GLB. */
    UFUNCTION(BlueprintCallable, Category="Weapon",
        meta=(DeprecatedFunction, DeprecationMessage="Use EquipFromModel with a gworld reference"))
    bool EquipFromFile(const FString& InFilePath, USceneComponent* AttachTarget);

    UFUNCTION(BlueprintCallable, Category="Weapon")
    bool EquipDefault(USceneComponent* AttachTarget);

    UFUNCTION(BlueprintCallable, Category="Weapon")
    void Fire(AController* InstigatorController);

    UFUNCTION(BlueprintPure, Category="Weapon")
    FString GetModelReference() const { return ModelReference; }

    /** Legacy Blueprint getter; the value is a gwd:// reference, not a source path. */
    UFUNCTION(BlueprintPure, Category="Weapon",
        meta=(DeprecatedFunction, DeprecationMessage="Use GetModelReference"))
    FString GetSourceFilePath() const { return ModelReference; }

    UFUNCTION(BlueprintPure, Category="Weapon")
    FWeaponConfig GetWeaponConfig() const { return Config; }

    UFUNCTION(BlueprintPure, Category="Weapon|IK")
    FTransform GetRightHandIKWorldTransform() const;

    UFUNCTION(BlueprintPure, Category="Weapon|IK")
    FTransform GetLeftHandIKWorldTransform() const;

    UFUNCTION(BlueprintPure, Category="Weapon|IK")
    FVector GetMuzzleWorldLocation() const;

protected:
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Destroyed() override;

private:
    bool bRuntimeResourcesReleased = false;

    /** Runtime default mesh resolved on demand from the central Asset Registry. */
    UPROPERTY(Transient)
    TObjectPtr<UStaticMesh> DefaultWeaponMesh = nullptr;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<USceneComponent> Root;

    UPROPERTY()
    /** Range-reading facade for one gwd:// model; it never contains or opens a source GLB. */
    TObjectPtr<UWorldBakedModelAsset> BakedAsset;

    UPROPERTY()
    TArray<TObjectPtr<UStaticMeshComponent>> MeshComponents;

    UPROPERTY()
    TMap<int32, TObjectPtr<UStaticMesh>> MeshCache;

    UPROPERTY()
    FString ModelReference;

    UPROPERTY(Transient)
    TSubclassOf<AWeaponProjectileActor> ProjectileClass;

    FWeaponConfig Config;
    double LastFireTime = -1000.0;

    void ResolveCentralWeaponAssets();
    bool LoadConfigJson(const FString& DefinitionJson);
    bool LoadWeaponMesh(const FResolvedRuntimeModel& Model);
    bool CreateDefaultBoxMesh();
    UStaticMesh* LoadMeshByIndex(int32 MeshIndex);
    void AttachToTarget(USceneComponent* AttachTarget);
    void ClearLoadedComponents();
    void ReleaseRuntimeResources();
};
