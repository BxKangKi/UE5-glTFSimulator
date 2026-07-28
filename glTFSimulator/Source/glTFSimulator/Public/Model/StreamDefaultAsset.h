// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Model/glTFMaterialAssetReferences.h"
#include "StreamDefaultAsset.generated.h"

/** Canonical direct Unreal material references used by streamed glTF assets. */
USTRUCT(BlueprintType)
struct FStreamDefaultAsset
{
    GENERATED_BODY()

public:
    /**
     * The only default-material configuration for streamed glTF assets.
     * Unreal assets are assigned directly; ByMaterialName keys are glTF-internal material names.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Streaming|Assets")
    FglTFMaterialAssetReferences Materials;
};
