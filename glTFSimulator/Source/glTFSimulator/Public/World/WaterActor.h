// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WaterActor.generated.h"

class USceneComponent;
class UDecalComponent;
class UBoxComponent;
class UPostProcessComponent;
class UStaticMeshComponent;

UCLASS()
class GLTFSIMULATOR_API AWaterActor : public AActor
{
    GENERATED_BODY()

public:

    AWaterActor();

    UPROPERTY(BlueprintReadOnly)
    float Level;

    void WaterTrigger(AActor *Actor, bool InWater);

    static void CheckOverlappingWater(AActor *Target);

    /**
     * Returns the water level for a world-space point even when the owning actor did not
     * receive an overlap event. This is used by detached ragdoll bodies: the capsule can
     * still be outside the water volume while a simulated limb/torso body is already inside.
     */
    static bool FindWaterLevelAtLocation(const UObject *WorldContextObject, const FVector &WorldLocation, float &OutLevel);

    UPROPERTY(EditDefaultsOnly, Category = "Materials")
    TObjectPtr<UMaterialInterface> DecalMaterial;

    UPROPERTY(EditDefaultsOnly, Category = "Materials")
    TObjectPtr<UMaterialInterface> UnderWaterMaterial;

protected:

    virtual void BeginPlay() override;
    virtual void NotifyActorBeginOverlap(AActor *OtherActor) override;
    virtual void NotifyActorEndOverlap(AActor *OtherActor) override;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UDecalComponent> Decal;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UBoxComponent> Collision;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UPostProcessComponent> PostProcess;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UStaticMeshComponent> StaticMesh;

private:
    void SetCurrentLevel();
};