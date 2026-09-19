// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

/**
 * @file WaterActor.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WaterActor.generated.h"

class USceneComponent;
class UDecalComponent;
class UBoxComponent;
class UPostProcessComponent;
class UStaticMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UWaterQuerySubsystem;

UCLASS()
class GLTFSIMULATOR_API AWaterActor : public AActor
{
    GENERATED_BODY()

public:

    AWaterActor();

    UPROPERTY(BlueprintReadOnly)
    float Level;

    void WaterTrigger(AActor *Actor, bool InWater);

    /** Marks this actor as the world-wide ocean. Global ocean presence is height-based, not overlap-based. */
    void SetGlobalOcean(bool bInGlobalOcean);

    /** Sizes the camera-following global-ocean render mesh and attached underwater decal volume to a world-space radius in centimeters. */
    void SetGlobalOceanRenderRadius(float RadiusCm);

    UFUNCTION(BlueprintPure, Category="Water")
    bool IsGlobalOcean() const { return bGlobalOcean; }

    static void CheckOverlappingWater(AActor *Target);

    /** Returns the configured global-ocean level without consulting collision or exclusion volumes. */
    static bool FindGlobalOceanLevel(const UObject* WorldContextObject, float& OutLevel);

    /**
     * Returns the water level for a world-space point even when the owning actor did not
     * receive an overlap event. This is used by detached ragdoll bodies: the capsule can
     * still be outside the water volume while a simulated limb/torso body is already inside.
     */
    static bool FindWaterLevelAtLocation(const UObject *WorldContextObject, const FVector &WorldLocation, float &OutLevel);

    /**
     * Strict water-volume query for controlled characters. Unlike the loose ragdoll
     * probe above, this does not allow horizontal tolerance, so swimming is cleared
     * as soon as the character reference leaves the water box side.
     */
    static bool FindWaterLevelAtLocationStrict(const UObject *WorldContextObject, const FVector &WorldLocation, float &OutLevel);

    /** Refreshes standardized exclusion-box parameters on every live water render material. */
    static void RefreshWaterExclusionRendering(const UObject* WorldContextObject);

    /** Enables/disables local underwater post processing according to the active exclusion boxes. */
    static void UpdateLocalViewWaterEffects(const UObject* WorldContextObject, const FVector& ViewLocation);

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInterface> DecalMaterial;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInterface> UnderWaterMaterial;

protected:

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void NotifyActorBeginOverlap(AActor *OtherActor) override;
    virtual void NotifyActorEndOverlap(AActor *OtherActor) override;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Water|Components")
    TObjectPtr<UDecalComponent> Decal;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Water|Components")
    TObjectPtr<UBoxComponent> Collision;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Water|Components")
    TObjectPtr<UPostProcessComponent> PostProcess;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Water|Components")
    TObjectPtr<UStaticMeshComponent> StaticMesh;

private:
    friend class UWaterQuerySubsystem;
    void SetCurrentLevel();
    void EnsureWaterMaterialInstances();
    void ApplyWaterExclusionRenderParameters();
    void UpdateUnderwaterPostProcessForView(const FVector& ViewLocation, bool bExcluded);

    UPROPERTY(Transient)
    TArray<TObjectPtr<UMaterialInstanceDynamic>> WaterMaterialInstances;

    bool bAuthoredPostProcessEnabled = true;

    UPROPERTY(Transient)
    bool bGlobalOcean = false;

    /** Last applied camera-following visual radius; does not participate in water-volume queries. */
    float GlobalOceanRenderRadiusCm = 0.0f;
};
