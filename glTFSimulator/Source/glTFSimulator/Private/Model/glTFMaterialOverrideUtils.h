// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file glTFMaterialOverrideUtils.h
 * 역할: 공통 머티리얼 설정을 glTFRuntime 로드 설정에 적용합니다.
 * 핵심 기능: 기본 머티리얼 오버라이드 매핑.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

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
        Overrides.Reserve(6);

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

    inline void ApplyNamedOverrides(
        const FglTFMaterialAssetReferences& References,
        FglTFRuntimeMaterialsConfig& MaterialsConfig)
    {
        if (References.ByMaterialName.Num() == 0)
        {
            return;
        }

        MaterialsConfig.MaterialsOverrideByNameMap.Reserve(
            MaterialsConfig.MaterialsOverrideByNameMap.Num() + References.ByMaterialName.Num());

        bool bAddedAnyOverride = false;
        for (const TPair<FString, TObjectPtr<UMaterialInterface>>& Pair : References.ByMaterialName)
        {
            FString MaterialName = Pair.Key;
            MaterialName.TrimStartAndEndInline();
            UMaterialInterface* Material = Pair.Value.Get();
            if (MaterialName.IsEmpty() || !IsValid(Material))
            {
                continue;
            }

            MaterialsConfig.MaterialsOverrideByNameMap.Add(MoveTemp(MaterialName), Material);
            bAddedAnyOverride = true;
        }

        if (bAddedAnyOverride)
        {
            // Keep glTF factors and textures. Without injection, glTFRuntime returns the base
            // material directly and the imported texture parameters are not applied.
            MaterialsConfig.bMaterialsOverrideMapInjectParams = true;
        }
    }

    inline void ApplyOverrides(
        const FglTFMaterialAssetReferences& References,
        FglTFRuntimeMaterialsConfig& MaterialsConfig)
    {
        TMap<EglTFRuntimeMaterialType, UMaterialInterface*> TypeOverrides = BuildOverrideMap(References);
        if (TypeOverrides.Num() > 0)
        {
            MaterialsConfig.UberMaterialsOverrideMap = TypeOverrides;
            MaterialsConfig.UnlitOverrideMap = MoveTemp(TypeOverrides);
        }

        ApplyNamedOverrides(References, MaterialsConfig);
    }

}
