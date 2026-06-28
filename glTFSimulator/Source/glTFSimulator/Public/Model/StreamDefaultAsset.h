// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "StreamDefaultAsset.generated.h"

class UMaterialInterface;

USTRUCT(BlueprintType)
struct FStreamDefaultAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UMaterialInterface> Opaque;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UMaterialInterface> TwoSided;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UMaterialInterface> Translucent;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UMaterialInterface> TranslucentTwoSided;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UMaterialInterface> Glass;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UMaterialInterface> TintedGlass;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UMaterialInterface> Terrain;
};