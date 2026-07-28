// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "Model/glTFMaterialAssetReferences.h"
#include "Materials/MaterialInterface.h"
#include "glTFRuntimeParser.h"

namespace glTFMaterialOverrideUtils
{
    inline TMap<EglTFRuntimeMaterialType, UMaterialInterface*> BuildOverrideMap(
        const FglTFMaterialAssetReferences& References)
    {
        TMap<EglTFRuntimeMaterialType, UMaterialInterface*> Overrides;

        const auto AddIfAssigned = [&Overrides](const EglTFRuntimeMaterialType Type, UMaterialInterface* Material)
        {
            if (IsValid(Material))
            {
                Overrides.Add(Type, Material);
            }
        };

        AddIfAssigned(EglTFRuntimeMaterialType::Opaque, References.Opaque.Get());
        AddIfAssigned(EglTFRuntimeMaterialType::Masked, References.Masked.Get());
        AddIfAssigned(EglTFRuntimeMaterialType::TwoSided, References.TwoSided.Get());
        AddIfAssigned(EglTFRuntimeMaterialType::TwoSidedMasked, References.TwoSidedMasked.Get());
        AddIfAssigned(EglTFRuntimeMaterialType::Translucent, References.Translucent.Get());
        AddIfAssigned(EglTFRuntimeMaterialType::TwoSidedTranslucent, References.TwoSidedTranslucent.Get());

        return Overrides;
    }

    inline TMap<FString, UMaterialInterface*> BuildNamedOverrideMap(
        const FglTFMaterialAssetReferences& References)
    {
        TMap<FString, UMaterialInterface*> Overrides;
        for (const TPair<FString, TObjectPtr<UMaterialInterface>>& Pair : References.ByMaterialName)
        {
            const FString MaterialName = Pair.Key.TrimStartAndEnd();
            UMaterialInterface* Material = Pair.Value.Get();
            if (!MaterialName.IsEmpty() && IsValid(Material))
            {
                Overrides.Add(MaterialName, Material);
            }
        }
        return Overrides;
    }

    inline void ApplyNamedOverrides(
        const FglTFMaterialAssetReferences& References,
        FglTFRuntimeMaterialsConfig& MaterialsConfig)
    {
        const TMap<FString, UMaterialInterface*> NamedOverrides = BuildNamedOverrideMap(References);
        if (NamedOverrides.Num() > 0)
        {
            for (const TPair<FString, UMaterialInterface*>& Pair : NamedOverrides)
            {
                MaterialsConfig.MaterialsOverrideByNameMap.Add(Pair.Key, Pair.Value);
            }
            // Keep glTF factors and textures. Without injection, glTFRuntime returns the base
            // material directly and the imported texture parameters are not applied.
            MaterialsConfig.bMaterialsOverrideMapInjectParams = true;
        }
    }
}
