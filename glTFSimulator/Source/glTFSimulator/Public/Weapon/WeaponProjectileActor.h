// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WeaponProjectileActor.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WeaponProjectileActor.generated.h"

class USphereComponent;
class AController;

/**
 * Lightweight server-authoritative projectile.
 *
 * It intentionally does not use UProjectileMovementComponent because that component owns a
 * separate tick function. Movement is registered in GameUpdateSubSystem instead, keeping all
 * gameplay update callbacks under the same dispatcher.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API AWeaponProjectileActor : public AActor
{
    GENERATED_BODY()

public:
    AWeaponProjectileActor();

    void InitProjectile(AController* InInstigatorController, float InDamage, float InImpulseStrength, float InLifeSeconds, const FVector& InLaunchVelocity);

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<USphereComponent> Collision;

    UPROPERTY()
    TObjectPtr<AController> CachedInstigatorController;

    FVector Velocity = FVector::ZeroVector;
    float Damage = 20.0f;
    float ImpulseStrength = 24000.0f;
    float LifeSeconds = 5.0f;
    int32 GameUpdateTickHandle = INDEX_NONE;

    void RegisterGameUpdate();
    void UnregisterGameUpdate();
    void UpdateProjectile(float DeltaSeconds);

    UFUNCTION()
    void OnProjectileHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit);
};
