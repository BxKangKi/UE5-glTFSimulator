// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WeaponActor.generated.h"

class UglTFRuntimeAsset;
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

    UFUNCTION(BlueprintCallable, Category="Weapon")
    bool EquipFromFile(const FString& InFilePath, USceneComponent* AttachTarget);

    UFUNCTION(BlueprintCallable, Category="Weapon")
    bool EquipDefault(USceneComponent* AttachTarget);

    UFUNCTION(BlueprintCallable, Category="Weapon")
    void Fire(AController* InstigatorController);

    UFUNCTION(BlueprintPure, Category="Weapon")
    FString GetSourceFilePath() const { return SourceFilePath; }

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

    /** Mesh used by EquipDefault and as the visual fallback when an external weapon file cannot be loaded. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Weapon|Assets", meta=(AllowPrivateAccess="true"))
    TObjectPtr<UStaticMesh> DefaultWeaponMesh = nullptr;

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

    UPROPERTY(EditDefaultsOnly, Category="Weapon")
    TSubclassOf<AWeaponProjectileActor> ProjectileClass;

    FWeaponConfig Config;
    double LastFireTime = -1000.0;

    bool LoadConfigJson(const FString& JsonPath);
    bool SaveDefaultConfigJson(const FString& JsonPath) const;
    bool LoadWeaponMesh();
    bool CreateDefaultBoxMesh();
    UStaticMesh* LoadMeshByIndex(int32 MeshIndex);
    void AttachToTarget(USceneComponent* AttachTarget);
    void ClearLoadedComponents();
    void ReleaseRuntimeResources();
};
