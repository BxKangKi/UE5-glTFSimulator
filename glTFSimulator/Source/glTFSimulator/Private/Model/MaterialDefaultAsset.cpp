// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file MaterialDefaultAsset.cpp
 * 역할: glTFRuntime용 기본 머티리얼 에셋을 보관합니다.
 * 핵심 기능: 에디터 설정, 공통 머티리얼 참조, 비동기 요청의 GC 보호.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "Model/MaterialDefaultAsset.h"

#include "Materials/MaterialInterface.h"

namespace
{
    UMaterialInterface* ResolveMaterialReference(
        const TSoftObjectPtr<UMaterialInterface>& Reference,
        const FString& Label,
        TArray<FString>& OutFailures)
    {
        if (Reference.IsNull())
        {
            return nullptr;
        }

        UMaterialInterface* Material = Reference.Get();
        if (!IsValid(Material))
        {
            Material = Reference.LoadSynchronous();
        }

        if (!IsValid(Material))
        {
            OutFailures.Add(FString::Printf(
                TEXT("%s -> %s"),
                *Label,
                *Reference.ToSoftObjectPath().ToString()));
            return nullptr;
        }

        return Material;
    }
}

bool FglTFMaterialSoftAssetReferences::Resolve(
    FglTFMaterialAssetReferences& OutReferences,
    TArray<FString>& OutFailures) const
{
    OutReferences.Reset();
    OutFailures.Reset();

    if (!ensureMsgf(IsInGameThread(), TEXT("Material default references must be resolved on the game thread")))
    {
        return false;
    }

    OutReferences.Opaque = ResolveMaterialReference(Opaque, TEXT("Opaque"), OutFailures);
    OutReferences.Masked = ResolveMaterialReference(Masked, TEXT("Masked"), OutFailures);
    OutReferences.TwoSided = ResolveMaterialReference(TwoSided, TEXT("TwoSided"), OutFailures);
    OutReferences.TwoSidedMasked = ResolveMaterialReference(TwoSidedMasked, TEXT("TwoSidedMasked"), OutFailures);
    OutReferences.Translucent = ResolveMaterialReference(Translucent, TEXT("Translucent"), OutFailures);
    OutReferences.TwoSidedTranslucent = ResolveMaterialReference(
        TwoSidedTranslucent,
        TEXT("TwoSidedTranslucent"),
        OutFailures);

    // This is editor-authored trusted data, but cap the runtime map so a damaged/corrupt asset cannot
    // trigger an unbounded allocation during world bootstrap.
    constexpr int32 MaxNamedMaterialOverrides = 4096;
    int32 ProcessedCount = 0;
    OutReferences.ByMaterialName.Reserve(FMath::Min(ByMaterialName.Num(), MaxNamedMaterialOverrides));
    for (const TPair<FString, TSoftObjectPtr<UMaterialInterface>>& Pair : ByMaterialName)
    {
        if (ProcessedCount++ >= MaxNamedMaterialOverrides)
        {
            OutFailures.Add(TEXT("ByMaterialName exceeded the 4096-entry safety limit"));
            break;
        }

        FString MaterialName = Pair.Key;
        MaterialName.TrimStartAndEndInline();
        if (MaterialName.IsEmpty())
        {
            continue;
        }

        if (UMaterialInterface* Material = ResolveMaterialReference(
            Pair.Value,
            FString::Printf(TEXT("ByMaterialName[%s]"), *MaterialName),
            OutFailures))
        {
            OutReferences.ByMaterialName.Add(MoveTemp(MaterialName), Material);
        }
    }

    OutReferences.ByMaterialName.Compact();
    return true;
}

int32 FglTFMaterialSoftAssetReferences::NumConfiguredReferences() const
{
    int32 Count = ByMaterialName.Num();
    Count += Opaque.IsNull() ? 0 : 1;
    Count += Masked.IsNull() ? 0 : 1;
    Count += TwoSided.IsNull() ? 0 : 1;
    Count += TwoSidedMasked.IsNull() ? 0 : 1;
    Count += Translucent.IsNull() ? 0 : 1;
    Count += TwoSidedTranslucent.IsNull() ? 0 : 1;
    return Count;
}

