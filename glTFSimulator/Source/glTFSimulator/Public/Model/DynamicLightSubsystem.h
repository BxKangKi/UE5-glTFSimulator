
// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Components/DecalComponent.h"
#include "DynamicLightSubsystem.generated.h"

class UDynamicPointLightComponent;

/** 스레드 세이프하게 거리 연산을 처리하기 위한 경량 구조체 (DoD 구조) */
struct FLightOptimizationData
{
    FVector Position;
    float CullingDistanceSq;
    float DecalTransitionDistanceSq;

    TWeakObjectPtr<UDynamicPointLightComponent> LightComponent;
    TWeakObjectPtr<UDecalComponent> DecalComponent;
    TWeakObjectPtr<UMaterialInterface> TargetDecalMaterial;

    // 스레드에서 쓸 계산 결과 캐싱 전용 플래그
    bool bTargetLightVisibility = true;
    bool bTargetDecalVisibility = false;

    // 무분별한 SetVisibility 호출을 막기 위한 현재 상태 캐싱
    bool bCurrentLightVisibility = true;
    bool bCurrentDecalVisibility = false;
};

UCLASS()
class GLTFSIMULATOR_API UDynamicLightSubsystem : public UWorldSubsystem, public FTickableGameObject
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase &Collection) override;
    virtual void Deinitialize() override;

    // FTickableGameObject 인터페이스 구현
    virtual void Tick(float DeltaTime) override;
    virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
    virtual bool IsTickable() const override { return !IsTemplate(); }
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UDynamicLightSubsystem, STATGROUP_Default); }
    void RegisterLight(UDynamicPointLightComponent *InLight);
    void UnregisterLight(UDynamicPointLightComponent *InLight);

private:
    TArray<FLightOptimizationData> ManagedLights;

    // 디칼 컴포넌트를 지연 생성하기 위한 내부 헬퍼
    UDecalComponent *CreateDecalComponent(UDynamicPointLightComponent *LightComp, UMaterialInterface *Material);
};