// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "PhysicsTransformInterpolationSubSystem.generated.h"

class USceneComponent;

/**
 * Game-thread transform interpolation bridge for async-physics results.
 *
 * This subsystem intentionally applies interpolation to the real component/actor transform.
 * It does not create a separate render transform.  Simulating skeletal meshes are skipped by
 * default because their render mesh and physics asset are the same component; splitting them
 * would produce bone/physics divergence and unstable ragdoll/camera behavior.
 */
UCLASS()
class GLTFSIMULATOR_API UPhysicsTransformInterpolationSubSystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    static UPhysicsTransformInterpolationSubSystem* Get(const UObject* WorldContextObject);

    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintCallable, Category="Physics|Interpolation", meta=(WorldContext="WorldContextObject"))
    static void SubmitPhysicsTransformForComponent(
        const UObject* WorldContextObject,
        USceneComponent* Component,
        const FTransform& TargetTransform,
        float InterpSpeed = 14.0f,
        float TeleportDistance = 500.0f,
        bool bApplyScale = false,
        bool bCanMoveSimulatingPrimitive = false);

    void SubmitTransform(
        USceneComponent* Component,
        const FTransform& TargetTransform,
        float InterpSpeed = 14.0f,
        float TeleportDistance = 500.0f,
        bool bApplyScale = false,
        bool bCanMoveSimulatingPrimitive = false);

    void ClearComponent(USceneComponent* Component);
    void ClearOwner(const UObject* Owner);
    void UpdateFromGameUpdate(float DeltaTime);

private:
    struct FInterpolatedTransformEntry
    {
        TWeakObjectPtr<USceneComponent> Component;
        FTransform TargetTransform = FTransform::Identity;
        float InterpSpeed = 14.0f;
        float TeleportDistance = 500.0f;
        bool bApplyScale = false;
        bool bCanMoveSimulatingPrimitive = false;
    };

    TArray<FInterpolatedTransformEntry> Entries;
    int32 GameUpdateHandle = INDEX_NONE;

    bool ShouldSkipComponent(const USceneComponent* Component, bool bCanMoveSimulatingPrimitive) const;
    void RegisterGameUpdate();
};
