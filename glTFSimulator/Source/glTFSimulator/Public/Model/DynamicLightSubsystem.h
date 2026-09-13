// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file DynamicLightSubsystem.h
 * 역할: 동적 조명의 거리별 활성 상태를 관리합니다.
 * 핵심 기능: 조명 등록·해제, 거리 컬링, 불필요한 렌더 상태 변경 억제.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Components/DecalComponent.h"
#include "DynamicLightSubsystem.generated.h"

class UDynamicPointLightComponent;
class UGameUpdateSubSystem;

/** Compact state used to batch distance culling and avoid redundant render-state changes. */
struct FLightOptimizationData
{
    FVector Position = FVector::ZeroVector;
    float CullingDistanceSq = 0.0f;
    float DecalTransitionDistanceSq = 0.0f;

    TWeakObjectPtr<UDynamicPointLightComponent> LightComponent;
    TWeakObjectPtr<UDecalComponent> DecalComponent;
    TWeakObjectPtr<UMaterialInterface> TargetDecalMaterial;

    // Desired visibility calculated during the current update.
    bool bTargetLightVisibility = true;
    bool bTargetDecalVisibility = false;

    // Cached visibility state used to avoid redundant SetVisibility calls.
    bool bCurrentLightVisibility = true;
    bool bCurrentDecalVisibility = false;
};

UCLASS()
class GLTFSIMULATOR_API UDynamicLightSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase &Collection) override;
    virtual void Deinitialize() override;

    void RegisterLight(UDynamicPointLightComponent *InLight);
    void UnregisterLight(UDynamicPointLightComponent *InLight);

private:
    TArray<FLightOptimizationData> ManagedLights;
    int32 GameUpdateHandle = INDEX_NONE;

    void RegisterGameUpdate();
    void UnregisterGameUpdate();
    void CompactManagedLights();
    void UpdateLightsFromGameUpdate(float DeltaTime);

    // Internal helper for lazy decal component creation.
    UDecalComponent *CreateDecalComponent(UDynamicPointLightComponent *LightComp, UMaterialInterface *Material);
};
