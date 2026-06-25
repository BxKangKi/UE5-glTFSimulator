// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "CharacterDefaultAsset.generated.h"

class UPhysicsAsset;
class USkeleton;
class UInputMappingContext;
class UMaterialInterface;

USTRUCT(BlueprintType)
struct FCharacterDefaultAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UPhysicsAsset> PhysicsAsset;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<USkeleton> Skeleton;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UInputMappingContext> IMC;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UMaterialInterface> Material;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<USkeletalMesh> SkeletalMesh;
};