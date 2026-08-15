// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Model/glTFMaterialAssetReferences.h"
#include "MaterialDefaultAsset.generated.h"

class UMaterialInterface;

/**
 * Shared GC guard for the resolved material set.
 *
 * The game subsystem owns one instance for the active world. Async glTF requests keep only one
 * pointer to this object, so material packages stay alive until native worker callbacks finish.
 * Actors do not own persistent duplicate tables; glTFRuntime still receives the request-local raw
 * pointer maps required by its config API. A new world gets a new guard object, and teardown never
 * mutates a guard that an old request may still be using.
 */
UCLASS(Transient)
class GLTFSIMULATOR_API UMaterialDefaultRuntimeCache : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY(Transient)
    FglTFMaterialAssetReferences References;
};

/**
 * Soft references authored once for the whole game system.
 *
 * The data asset itself does not force any material package into memory. The active
 * UGameManagerSubSystem resolves these references once during world bootstrap and owns the
 * resulting strong references while streamed/runtime meshes are using them.
 */
USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FglTFMaterialSoftAssetReferences
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="glTF Material Assets|By Type")
    TSoftObjectPtr<UMaterialInterface> Opaque;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="glTF Material Assets|By Type")
    TSoftObjectPtr<UMaterialInterface> Masked;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="glTF Material Assets|By Type")
    TSoftObjectPtr<UMaterialInterface> TwoSided;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="glTF Material Assets|By Type")
    TSoftObjectPtr<UMaterialInterface> TwoSidedMasked;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="glTF Material Assets|By Type")
    TSoftObjectPtr<UMaterialInterface> Translucent;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="glTF Material Assets|By Type")
    TSoftObjectPtr<UMaterialInterface> TwoSidedTranslucent;

    /** Optional overrides keyed by the material name stored inside the glTF document. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="glTF Material Assets|By glTF Name")
    TMap<FString, TSoftObjectPtr<UMaterialInterface>> ByMaterialName;

    /** Resolves configured soft references on the game thread. Missing assets are skipped safely. */
    bool Resolve(FglTFMaterialAssetReferences& OutReferences, TArray<FString>& OutFailures) const;

    int32 NumConfiguredReferences() const;
};

/**
 * One project/game-system level material configuration for all streamed glTF assets, prefabs,
 * vehicles, and weapons. Author these values on a Data Only Blueprint subclass, then assign that
 * class on AGameManagerActor. The game subsystem creates one transient runtime instance after the
 * manager actor begins play.
 */
UCLASS(BlueprintType, Blueprintable)
class GLTFSIMULATOR_API UMaterialDefaultAsset : public UDataAsset
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="glTF Materials")
    FglTFMaterialSoftAssetReferences Materials;

    bool ResolveMaterials(FglTFMaterialAssetReferences& OutReferences, TArray<FString>& OutFailures) const;
};
