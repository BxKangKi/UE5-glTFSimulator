// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

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
