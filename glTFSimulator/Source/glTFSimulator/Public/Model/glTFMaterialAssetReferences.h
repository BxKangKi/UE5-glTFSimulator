// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "glTFMaterialAssetReferences.generated.h"

class UMaterialInterface;

/**
 * Unreal material assets assigned directly in Blueprint/class defaults for glTFRuntime imports.
 *
 * The object references themselves are never discovered from Unreal package names or object paths.
 * Material-name keys are glTF document data and are intentionally preserved so imported materials
 * such as glass or terrain can select a directly assigned Unreal base material.
 */
USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FglTFMaterialAssetReferences
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="glTF Material Assets|By Type")
    TObjectPtr<UMaterialInterface> Opaque = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="glTF Material Assets|By Type")
    TObjectPtr<UMaterialInterface> Masked = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="glTF Material Assets|By Type")
    TObjectPtr<UMaterialInterface> TwoSided = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="glTF Material Assets|By Type")
    TObjectPtr<UMaterialInterface> TwoSidedMasked = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="glTF Material Assets|By Type")
    TObjectPtr<UMaterialInterface> Translucent = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="glTF Material Assets|By Type")
    TObjectPtr<UMaterialInterface> TwoSidedTranslucent = nullptr;

    /**
     * Optional glTF material-name overrides. Keys are names stored inside the imported glTF file;
     * values are Unreal material assets selected directly in the editor.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="glTF Material Assets|By glTF Name")
    TMap<FString, TObjectPtr<UMaterialInterface>> ByMaterialName;
};
