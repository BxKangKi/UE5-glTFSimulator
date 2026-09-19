// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "Components/BoxComponent.h"
#include "WaterExclusionBoxComponent.generated.h"

UENUM(BlueprintType)
enum class EWaterExclusionFeature : uint8
{
    Rendering,
    UnderwaterPostProcess,
    Buoyancy,
    WaterInteraction
};

/** Detached numeric snapshot safe to read from worker tasks. */
struct GLTFSIMULATOR_API FWaterExclusionNativeBox
{
    FTransform WorldTransform = FTransform::Identity;
    FVector LocalExtent = FVector::ZeroVector;

    bool Contains(const FVector& WorldLocation, const float Tolerance = 0.0f) const
    {
        const FVector Local = WorldTransform.InverseTransformPosition(WorldLocation);
        const FVector Extent = LocalExtent + FVector(FMath::Max(0.0f, Tolerance));
        return FMath::Abs(Local.X) <= Extent.X
            && FMath::Abs(Local.Y) <= Extent.Y
            && FMath::Abs(Local.Z) <= Extent.Z;
    }
};

/**
 * Box-shaped region where water systems are ignored.
 *
 * Physics and water-presence queries are handled entirely in native code. Water rendering uses a
 * material contract: AWaterActor writes WaterExclusionCount and WaterExclusion{Center,Extent,Rotation}_N
 * parameters to its dynamic water material instances. Materials that implement those parameters can
 * clip the exact same region; legacy materials safely ignore unknown parameters.
 */
UCLASS(ClassGroup=(Water), BlueprintType, Blueprintable, meta=(BlueprintSpawnableComponent))
class GLTFSIMULATOR_API UWaterExclusionBoxComponent : public UBoxComponent
{
    GENERATED_BODY()

public:
    UWaterExclusionBoxComponent();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Water Exclusion")
    bool bExcludeWaterRendering = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Water Exclusion")
    bool bExcludeUnderwaterPostProcess = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Water Exclusion")
    bool bExcludeBuoyancy = true;

    /** Also makes swimming/vehicle water-presence queries report dry space inside this box. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Water Exclusion")
    bool bExcludeWaterInteraction = true;

    UFUNCTION(BlueprintPure, Category="Water Exclusion")
    bool ContainsWorldLocation(const FVector& WorldLocation) const;

    UFUNCTION(BlueprintCallable, Category="Water Exclusion")
    void RefreshWaterExclusion();

    static bool IsLocationExcluded(
        const UObject* WorldContextObject,
        const FVector& WorldLocation,
        EWaterExclusionFeature Feature,
        float Tolerance = 0.0f);

    static void GatherNativeBoxes(
        const UObject* WorldContextObject,
        EWaterExclusionFeature Feature,
        TArray<FWaterExclusionNativeBox>& OutBoxes,
        int32 MaxBoxes = MAX_int32);

protected:
    virtual void OnRegister() override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
    bool IsFeatureEnabled(EWaterExclusionFeature Feature) const;
    uint8 GetFeatureMask() const;
    void RegisterExclusion();
    void UnregisterExclusion();

    FTransform LastPublishedTransform = FTransform::Identity;
    FVector LastPublishedExtent = FVector::ZeroVector;
    uint8 LastPublishedFeatureMask = 0;
};
