// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RuntimeWeaponActor.generated.h"

class UglTFRuntimeAsset;
class UStaticMeshComponent;
class UStaticMesh;
class USceneComponent;
class UCameraComponent;

USTRUCT(BlueprintType)
struct FRuntimeWeaponConfig
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
class GLTFSIMULATOR_API ARuntimeWeaponActor : public AActor
{
    GENERATED_BODY()

public:
    ARuntimeWeaponActor();

    UFUNCTION(BlueprintCallable, Category="Runtime Weapon")
    bool EquipFromFile(const FString& InFilePath, USceneComponent* AttachTarget);

    UFUNCTION(BlueprintCallable, Category="Runtime Weapon")
    void Fire(AController* InstigatorController);

    UFUNCTION(BlueprintPure, Category="Runtime Weapon")
    FString GetSourceFilePath() const { return SourceFilePath; }

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

    FRuntimeWeaponConfig Config;
    double LastFireTime = -1000.0;

    bool LoadConfigJson(const FString& JsonPath);
    bool LoadWeaponMesh();
    UStaticMesh* LoadMeshByIndex(int32 MeshIndex);
    FVector GetMuzzleWorldLocation() const;
    void ClearLoadedComponents();
};
