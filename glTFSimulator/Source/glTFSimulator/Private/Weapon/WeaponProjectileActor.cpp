// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Weapon/WeaponProjectileActor.h"

#include "Components/SphereComponent.h"
#include "GameFramework/Controller.h"
#include "Kismet/GameplayStatics.h"
#include "System/GameUpdateSubSystem.h"

namespace WeaponProjectileTuning
{
    constexpr float MinProjectileLifeSeconds = 0.1f;
}

AWeaponProjectileActor::AWeaponProjectileActor()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = true;
    SetReplicateMovement(true);

    Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
    SetRootComponent(Collision);
    Collision->InitSphereRadius(4.0f);
    Collision->SetCollisionProfileName(TEXT("BlockAllDynamic"));
    Collision->SetNotifyRigidBodyCollision(true);
}

void AWeaponProjectileActor::BeginPlay()
{
    Super::BeginPlay();

    if (Collision)
    {
        Collision->OnComponentHit.AddDynamic(this, &AWeaponProjectileActor::OnProjectileHit);
    }

    SetLifeSpan(FMath::Max(WeaponProjectileTuning::MinProjectileLifeSeconds, LifeSeconds));
    RegisterGameUpdate();
}

void AWeaponProjectileActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UnregisterGameUpdate();
    Super::EndPlay(EndPlayReason);
}

void AWeaponProjectileActor::InitProjectile(AController* InInstigatorController, float InDamage, float InImpulseStrength, float InLifeSeconds, const FVector& InLaunchVelocity)
{
    CachedInstigatorController = InInstigatorController;
    Damage = FMath::Max(0.0f, InDamage);
    ImpulseStrength = FMath::Max(0.0f, InImpulseStrength);
    LifeSeconds = FMath::Max(WeaponProjectileTuning::MinProjectileLifeSeconds, InLifeSeconds);
    Velocity = InLaunchVelocity;
    SetInstigator(InInstigatorController ? InInstigatorController->GetPawn() : nullptr);

    if (!Velocity.IsNearlyZero())
    {
        SetActorRotation(Velocity.Rotation());
    }
}

void AWeaponProjectileActor::RegisterGameUpdate()
{
    if (GameUpdateTickHandle != INDEX_NONE)
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [this](const float DeltaSeconds)
            {
                UpdateProjectile(DeltaSeconds);
            },
            25);
    }
}

void AWeaponProjectileActor::UnregisterGameUpdate()
{
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
    }
    GameUpdateTickHandle = INDEX_NONE;
}

void AWeaponProjectileActor::UpdateProjectile(float DeltaSeconds)
{
    if (Velocity.IsNearlyZero() || DeltaSeconds <= 0.0f)
    {
        return;
    }

    const FVector NewLocation = GetActorLocation() + Velocity * DeltaSeconds;
    FHitResult Hit;
    SetActorLocation(NewLocation, true, &Hit, ETeleportType::None);
    SetActorRotation(Velocity.Rotation());

    if (Hit.bBlockingHit)
    {
        OnProjectileHit(Collision.Get(), Hit.GetActor(), Hit.GetComponent(), FVector::ZeroVector, Hit);
    }
}

void AWeaponProjectileActor::OnProjectileHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit)
{
    if (!IsValid(OtherActor) || OtherActor == this || OtherActor == GetInstigator())
    {
        Destroy();
        return;
    }

    const FVector ShotDirection = Velocity.IsNearlyZero() ? GetActorForwardVector() : Velocity.GetSafeNormal();
    UGameplayStatics::ApplyPointDamage(OtherActor, Damage, ShotDirection, Hit, CachedInstigatorController.Get(), this, nullptr);

    if (IsValid(OtherComp) && OtherComp->IsSimulatingPhysics())
    {
        OtherComp->AddImpulseAtLocation(ShotDirection * ImpulseStrength, Hit.ImpactPoint, Hit.BoneName);
    }

    Destroy();
}
