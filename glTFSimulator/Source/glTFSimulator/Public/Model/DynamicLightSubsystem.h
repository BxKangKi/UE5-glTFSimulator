
// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Components/DecalComponent.h"
#include "DynamicLightSubsystem.generated.h"

class UDynamicPointLightComponent;

/** Lightweight data-oriented structure for thread-safe distance calculations. */
struct FLightOptimizationData
{
    FVector Position;
    float CullingDistanceSq;
    float DecalTransitionDistanceSq;

    TWeakObjectPtr<UDynamicPointLightComponent> LightComponent;
    TWeakObjectPtr<UDecalComponent> DecalComponent;
    TWeakObjectPtr<UMaterialInterface> TargetDecalMaterial;

    // Cached calculation flag used by worker threads.
    bool bTargetLightVisibility = true;
    bool bTargetDecalVisibility = false;

    // Cached visibility state used to avoid redundant SetVisibility calls.
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

    // FTickableGameObject interface implementation.
    virtual void Tick(float DeltaTime) override;
    virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
    virtual bool IsTickable() const override { return !IsTemplate(); }
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UDynamicLightSubsystem, STATGROUP_Default); }
    void RegisterLight(UDynamicPointLightComponent *InLight);
    void UnregisterLight(UDynamicPointLightComponent *InLight);

private:
    TArray<FLightOptimizationData> ManagedLights;

    // Internal helper for lazy decal component creation.
    UDecalComponent *CreateDecalComponent(UDynamicPointLightComponent *LightComp, UMaterialInterface *Material);
};