// Copyright 2026 OpenAI. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "RuntimeImpostorTypes.generated.h"

UENUM(BlueprintType)
enum class ERuntimeImpostorState : uint8
{
    Uninitialized,
    Baking,
    Ready,
    Failed
};

USTRUCT(BlueprintType)
struct FRuntimeImpostorBakeSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bake", meta=(ClampMin="32", ClampMax="1024"))
    int32 TileResolution = 128;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bake", meta=(ClampMin="4", ClampMax="32"))
    int32 HorizontalViews = 8;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bake", meta=(ClampMin="1", ClampMax="5"))
    int32 VerticalViews = 3;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bake", meta=(ClampMin="0.0", ClampMax="75.0"))
    float PitchDegrees = 30.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bake", meta=(ClampMin="1.0", ClampMax="4.0"))
    float CapturePadding = 1.15f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bake")
    bool bHideSourceAfterBake = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bake")
    bool bDisableSourceCollisionAfterBake = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Bake")
    bool bCaptureTranslucent = true;
};

USTRUCT(BlueprintType)
struct FRuntimeImpostorLODSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LOD", meta=(ClampMin="0.0"))
    float StartDistance = 6000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LOD", meta=(ClampMin="0.0"))
    float EndDistance = 7200.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LOD")
    bool bUseDistanceSwitching = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LOD", meta=(ClampMin="0.0", ClampMax="2.0"))
    float BillboardHeightScale = 1.0f;
};
