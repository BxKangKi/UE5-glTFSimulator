// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file glTFMaterialAssetReferences.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "glTFMaterialAssetReferences.generated.h"

class UMaterialInterface;

/**
 * Runtime strong-reference set resolved from the central soft-reference registry for glTFRuntime imports.
 *
 * This structure is not editor-authored. Material-name keys are glTF document data and are intentionally
 * preserved so imported materials such as glass or terrain can select the resolved Unreal base material.
 */
USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FglTFMaterialAssetReferences
{
    GENERATED_BODY()

    UPROPERTY(Transient, BlueprintReadOnly, Category="glTF Material Assets|Runtime")
    TObjectPtr<UMaterialInterface> Opaque = nullptr;

    UPROPERTY(Transient, BlueprintReadOnly, Category="glTF Material Assets|Runtime")
    TObjectPtr<UMaterialInterface> Masked = nullptr;

    UPROPERTY(Transient, BlueprintReadOnly, Category="glTF Material Assets|Runtime")
    TObjectPtr<UMaterialInterface> TwoSided = nullptr;

    UPROPERTY(Transient, BlueprintReadOnly, Category="glTF Material Assets|Runtime")
    TObjectPtr<UMaterialInterface> TwoSidedMasked = nullptr;

    UPROPERTY(Transient, BlueprintReadOnly, Category="glTF Material Assets|Runtime")
    TObjectPtr<UMaterialInterface> Translucent = nullptr;

    UPROPERTY(Transient, BlueprintReadOnly, Category="glTF Material Assets|Runtime")
    TObjectPtr<UMaterialInterface> TwoSidedTranslucent = nullptr;

    /**
     * Optional glTF material-name overrides. Keys are names stored inside the imported glTF file;
     * values are Unreal material assets selected directly in the editor.
     */
    UPROPERTY(Transient, BlueprintReadOnly, Category="glTF Material Assets|Runtime")
    TMap<FString, TObjectPtr<UMaterialInterface>> ByMaterialName;

    bool IsEmpty() const
    {
        return !IsValid(Opaque.Get())
            && !IsValid(Masked.Get())
            && !IsValid(TwoSided.Get())
            && !IsValid(TwoSidedMasked.Get())
            && !IsValid(Translucent.Get())
            && !IsValid(TwoSidedTranslucent.Get())
            && ByMaterialName.Num() == 0;
    }

    void Reset()
    {
        Opaque = nullptr;
        Masked = nullptr;
        TwoSided = nullptr;
        TwoSidedMasked = nullptr;
        Translucent = nullptr;
        TwoSidedTranslucent = nullptr;
        ByMaterialName.Empty();
    }
};
