// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file DynamicPointLightComponent.h
 * 역할: 동적 포인트 조명 컴포넌트를 제공합니다.
 * 핵심 기능: 조명 수명과 동적 조명 서브시스템 연동.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

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

    UMaterialInterface *GetLightDecal() const { return LightDecal.Get(); }
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
    UPROPERTY(Transient)
    TObjectPtr<UMaterialInterface> LightDecal;

    /** Disabled by default because large fallback decals can wash out glTF materials. */
    UPROPERTY(EditAnywhere, Category = "Optimization|Decal")
    bool bEnableLightDecalFallback = false;

    /** Disables the light when the camera is farther than this distance. */
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
