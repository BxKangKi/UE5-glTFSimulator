// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Interface/WaterInteract.h"
#include "RuntimeBuoyancyComponent.generated.h"

class UPrimitiveComponent;
class USkeletalMeshComponent;

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FRuntimeBuoyancyPhysicsSettings
{
    GENERATED_BODY()

    /** Per-sample force safety cap. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics", meta=(ClampMin="1.0"))
    float BuoyancyForcePerPoint = 45000.0f;

    /** 1.0 means neutral lift for a fully submerged body. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics", meta=(ClampMin="0.0", ClampMax="2.0"))
    float BuoyancyAccelerationScale = 0.990f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics", meta=(ClampMin="1.0"))
    float SubmersionDepth = 320.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics", meta=(ClampMin="0.0"))
    float LinearWaterDamping = 2.75f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics", meta=(ClampMin="0.0"))
    float AngularWaterDamping = 3.60f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics", meta=(ClampMin="1.0"))
    float MaxForcePerPoint = 50000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Drag", meta=(ClampMin="0.0"))
    float WaterLinearDragCoefficient = 2.05f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Drag", meta=(ClampMin="0.0"))
    float WaterQuadraticDragCoefficient = 0.00155f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Drag", meta=(ClampMin="1.0"))
    float MaxDragForcePerPoint = 125000.0f;

    /** Speeds above this receive an additional drag multiplier, useful for fast water entry. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Drag", meta=(ClampMin="0.0"))
    float HighSpeedDragStartSpeed = 520.0f;

    /** Speed where HighSpeedDragMultiplier is fully applied. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Drag", meta=(ClampMin="1.0"))
    float HighSpeedDragFullSpeed = 2600.0f;

    /** Extra drag applied at and above HighSpeedDragFullSpeed. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Drag", meta=(ClampMin="1.0"))
    float HighSpeedDragMultiplier = 3.10f;

    /** Higher values soften the first water-contact frame so entry does not feel like a hard stop. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Drag", meta=(ClampMin="0.1"))
    float SurfaceEntryDragAlphaPower = 0.82f;

    /** All-direction water drag multiplier. It ramps in with submersion so surface entry still feels gradual. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Drag", meta=(ClampMin="0.0"))
    float WaterDragMultiplier = 3.00f;

    /** Extra all-direction drag starts after this submerged fraction to avoid a surface hard stop. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Drag", meta=(ClampMin="0.0", ClampMax="0.95"))
    float WaterDragMultiplierMinSubmergedAlpha = 0.03f;

    /** Additional drag only against downward motion, used to make submerged ragdolls sink slowly. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Drag", meta=(ClampMin="1.0"))
    float DownwardWaterDragMultiplier = 3.65f;

    /** Split large frame hitches into smaller impulse slices. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Stability", meta=(ClampMin="0.001", ClampMax="0.1"))
    float MaxBuoyancyStepSeconds = 0.016667f;

    /** Per-substep velocity-change cap, in cm/s, for buoyancy + drag impulses. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Stability", meta=(ClampMin="1.0"))
    float MaxImpulseVelocityChangePerStep = 185.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Stability")
    bool bClampLinearVelocity = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Stability", meta=(ClampMin="1.0", EditCondition="bClampLinearVelocity"))
    float MaxLinearSpeed = 1050.0f;

    /** Soft downward terminal-speed limit for submerged bodies. This is interpolated, not snapped. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Stability")
    bool bLimitDownwardSinkSpeed = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Stability", meta=(ClampMin="1.0", EditCondition="bLimitDownwardSinkSpeed"))
    float MaxDownwardSinkSpeed = 80.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Stability", meta=(ClampMin="0.0", EditCondition="bLimitDownwardSinkSpeed"))
    float SinkSpeedSoftClampInterpSpeed = 8.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Stability", meta=(ClampMin="0.0", ClampMax="1.0", EditCondition="bLimitDownwardSinkSpeed"))
    float SinkSpeedClampMinSubmergedAlpha = 0.18f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Stability")
    bool bClampAngularVelocity = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Physics|Stability", meta=(ClampMin="0.1", EditCondition="bClampAngularVelocity"))
    float MaxAngularSpeed = 5.0f;
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FRuntimeSkeletalBuoyancyBoneRule
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Skeletal Mesh")
    FName RuleName = NAME_None;

    /** Exact bone names covered by this rule. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Skeletal Mesh")
    TArray<FName> BoneNames;

    /** Case-insensitive normalized substrings. Separators such as _, -, . and spaces are ignored. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Skeletal Mesh")
    TArray<FString> BoneNameContainsPatterns;

    /** Excluded bones are not part of the main buoyancy sample set. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Skeletal Mesh")
    bool bExcludeFromPrimaryBuoyancy = false;

    /** False is useful for hair, cloth, accessories, and other secondary-only bodies. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Skeletal Mesh")
    bool bCountTowardsSimulationThreshold = true;

    /** Optional separate force/damping pass for excluded bones. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Skeletal Mesh", meta=(EditCondition="bExcludeFromPrimaryBuoyancy"))
    bool bApplyPhysicsWhenExcluded = false;

    /** When false, the component common physics settings are used for this rule. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Skeletal Mesh")
    bool bOverridePhysicsSettings = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Skeletal Mesh", meta=(EditCondition="bOverridePhysicsSettings"))
    FRuntimeBuoyancyPhysicsSettings PhysicsSettings;
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FRuntimeSkeletalBuoyancySettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Skeletal Mesh")
    bool bApplyToSimulatingBodies = true;

    /** Ignore secondary roots such as hair/dynamic roots before choosing buoyancy bodies. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Skeletal Mesh")
    bool bIgnoreSecondaryPhysicsBodies = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Skeletal Mesh", meta=(ClampMin="1", ClampMax="32"))
    int32 MaxBodySamples = 24;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Skeletal Mesh", meta=(ClampMin="1", ClampMax="16"))
    int32 MinSimulatedBodiesForBuoyancy = 4;

    /** Runtime-loaded skeletal meshes can report tiny aggregate mass. This prevents underpowered buoyancy. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Skeletal Mesh", meta=(ClampMin="1.0"))
    float TotalMassFloor = 70.0f;

    /** First matching rule wins. Put exact/special rules before broader pattern rules. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy|Skeletal Mesh")
    TArray<FRuntimeSkeletalBuoyancyBoneRule> BoneRules;
};

UCLASS(ClassGroup=(Physics), Blueprintable, BlueprintType, meta=(BlueprintSpawnableComponent))
class GLTFSIMULATOR_API URuntimeBuoyancyComponent : public UActorComponent, public IWaterInteract
{
    GENERATED_BODY()

public:
    URuntimeBuoyancyComponent();

    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    virtual void EnterWater(const float Level = 0.0f) override;
    virtual void ExitWater(const float Level = 0.0f) override;

    UFUNCTION(BlueprintPure, Category="Runtime Buoyancy")
    bool IsInWater() const { return bInWater; }

    UFUNCTION(BlueprintPure, Category="Runtime Buoyancy")
    float GetWaterLevel() const { return WaterLevel; }

    UFUNCTION(BlueprintCallable, Category="Runtime Buoyancy")
    void RebuildSamplePoints();

    UFUNCTION(BlueprintCallable, Category="Runtime Buoyancy")
    void SetTargetComponentName(FName InTargetComponentName);

    UFUNCTION(BlueprintCallable, Category="Runtime Buoyancy")
    void SetCommonPhysicsSettings(const FRuntimeBuoyancyPhysicsSettings& InSettings);

    UFUNCTION(BlueprintPure, Category="Runtime Buoyancy")
    FRuntimeBuoyancyPhysicsSettings GetCommonPhysicsSettings() const { return CommonPhysicsSettings; }

    UFUNCTION(BlueprintCallable, Category="Runtime Buoyancy")
    void SetSkeletalMeshSettings(const FRuntimeSkeletalBuoyancySettings& InSettings);

    UFUNCTION(BlueprintPure, Category="Runtime Buoyancy")
    FRuntimeSkeletalBuoyancySettings GetSkeletalMeshSettings() const { return SkeletalMeshSettings; }

protected:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy")
    FName TargetComponentName = NAME_None;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy")
    bool bAutoFindSimulatingPrimitive = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy")
    FRuntimeBuoyancyPhysicsSettings CommonPhysicsSettings;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy")
    FRuntimeSkeletalBuoyancySettings SkeletalMeshSettings;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy", meta=(ClampMin="1"))
    int32 AutoSampleCountX = 3;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy", meta=(ClampMin="1"))
    int32 AutoSampleCountY = 3;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy", meta=(ClampMin="1"))
    int32 AutoSampleCountZ = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Buoyancy")
    TArray<FVector> LocalSamplePoints;

private:
    UPROPERTY(Transient)
    TObjectPtr<UPrimitiveComponent> TargetPrimitive;

    bool bInWater = false;
    float WaterLevel = 0.0f;
    bool bAutoGeneratedSamplePoints = false;

    UPrimitiveComponent* ResolveTargetPrimitive();
    void ClearAutoGeneratedSamplePoints();
    bool ApplySkeletalMeshBuoyancy(USkeletalMeshComponent* SkeletalMesh, float DeltaTime);
};
