// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/PointLightComponent.h"
#include "DynamicPointLightComponent.generated.h"

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class GLTFSIMULATOR_API UDynamicPointLightComponent : public UPointLightComponent
{
    GENERATED_BODY()

public:
    UDynamicPointLightComponent();

    UMaterialInterface *GetLightDecal() const { return LightDecal; }
    void SetLightDecal(UMaterialInterface *InDecal) { LightDecal = InDecal; }

    bool IsLightDecalFallbackEnabled() const { return bEnableLightDecalFallback; }
    float GetCullingDistance() const { return CullingDistance; }
    float GetDecalTransitionDistance() const { return DecalTransitionDistance; }
    float GetMaxLightDecalSize() const { return MaxLightDecalSize; }
    float GetMaxLightDecalOpacity() const { return MaxLightDecalOpacity; }

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    UPROPERTY(EditAnywhere, Category = "Optimization|Decal")
    TObjectPtr<UMaterialInterface> LightDecal;

    /** Disabled by default because large fallback decals can wash out glTF materials. */
    UPROPERTY(EditAnywhere, Category = "Optimization|Decal")
    bool bEnableLightDecalFallback = false;

    /** 이 거리보다 멀어지면 라이트가 꺼집니다. */
    UPROPERTY(EditAnywhere, Category = "Optimization", meta = (UIMin = "0.0", ClampMin = "0.0"))
    float CullingDistance = 10000.0f;

    /** Decal fallback is used only when bEnableLightDecalFallback is true and this value is greater than CullingDistance. */
    UPROPERTY(EditAnywhere, Category = "Optimization|Decal", meta = (UIMin = "0.0", ClampMin = "0.0"))
    float DecalTransitionDistance = 0.0f;

    UPROPERTY(EditAnywhere, Category = "Optimization|Decal", meta = (UIMin = "1.0", ClampMin = "1.0"))
    float MaxLightDecalSize = 2500.0f;

    UPROPERTY(EditAnywhere, Category = "Optimization|Decal", meta = (UIMin = "0.0", ClampMin = "0.0", ClampMax = "1.0"))
    float MaxLightDecalOpacity = 0.22f;
};
