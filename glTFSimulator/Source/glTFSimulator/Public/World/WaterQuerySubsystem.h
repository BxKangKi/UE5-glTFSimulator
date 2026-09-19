// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WaterQuerySubsystem.generated.h"

class AActor;
class AWaterActor;

/** Controls how tightly a point must stay inside a finite local-water volume. */
UENUM(BlueprintType)
enum class EWaterQueryMode : uint8
{
    /** Exact gameplay interaction query. Best for characters, vehicles and ordinary state changes. */
    Strict,

    /** Adds small XY/surface/bottom tolerances. Best for ragdolls and detached physics samples. */
    Relaxed
};

/** Selects which water-exclusion mask participates in the query. */
UENUM(BlueprintType)
enum class EWaterQueryPurpose : uint8
{
    Interaction,
    Buoyancy
};

/** Common water-presence result shared by characters, buoyancy and arbitrary gameplay objects. */
USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FWaterQueryResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Water")
    bool bInWater = false;

    UPROPERTY(BlueprintReadOnly, Category="Water")
    float SurfaceZ = 0.0f;

    /** Positive below the selected water surface, negative above it. */
    UPROPERTY(BlueprintReadOnly, Category="Water")
    float ImmersionDepth = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category="Water")
    bool bGlobalOcean = false;

    UPROPERTY(BlueprintReadOnly, Category="Water")
    TObjectPtr<AWaterActor> WaterActor = nullptr;
};

/**
 * World-owned authoritative water query service.
 *
 * AWaterActor only publishes/removes water sources. Gameplay code asks this subsystem whether a
 * point is wet; therefore global ocean, local water boxes and exclusion volumes use one path.
 * The global ocean is an infinite XY half-space below its SurfaceZ and never relies on overlap.
 */
UCLASS()
class GLTFSIMULATOR_API UWaterQuerySubsystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    static UWaterQuerySubsystem* Get(const UObject* WorldContextObject);

    UFUNCTION(BlueprintCallable, Category="Water|Query")
    bool QueryWaterAtLocation(
        const FVector& WorldLocation,
        EWaterQueryMode Mode,
        EWaterQueryPurpose Purpose,
        FWaterQueryResult& OutResult);

    UFUNCTION(BlueprintCallable, Category="Water|Query")
    bool QueryActorLocation(
        const AActor* Actor,
        EWaterQueryMode Mode,
        EWaterQueryPurpose Purpose,
        FWaterQueryResult& OutResult);

    UFUNCTION(BlueprintPure, Category="Water|Query")
    bool GetGlobalOceanLevel(float& OutLevel) const;

    /** Water sources call these; ordinary gameplay code should use QueryWaterAtLocation instead. */
    void RegisterWaterActor(AWaterActor* WaterActor);
    void UnregisterWaterActor(AWaterActor* WaterActor);
    void SetGlobalOceanActor(AWaterActor* WaterActor, bool bIsGlobalOcean);
    void GetRegisteredWaterActors(TArray<AWaterActor*>& OutWaterActors);

    virtual void Deinitialize() override;

private:
    void CompactInvalidWaterActors();

    TArray<TWeakObjectPtr<AWaterActor>> WaterActors;
    TWeakObjectPtr<AWaterActor> GlobalOcean;
};
