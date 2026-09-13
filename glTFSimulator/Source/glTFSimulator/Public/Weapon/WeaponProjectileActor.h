// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WeaponProjectileActor.h
 * 역할: 서버 권한의 경량 투사체 액터입니다.
 * 핵심 기능: 통합 업데이트 이동, 충돌·수명 종료.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
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
