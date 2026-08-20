// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "CharacterAnimInstance.generated.h"

class UCharacterMovementComponent;
class UCharacterComponent;
class AWeaponActor;

UCLASS()
class GLTFSIMULATOR_API UCharacterAnimInstance : public UAnimInstance
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    FVector Velocity = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    float Speed = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    float MoveSpeed = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    float UpSpeed = 0.0f;

    /** Signed actor yaw angular velocity in degrees per second. */
    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    float RotationSpeed = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bShouldMove = false;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsFlying = false;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsSwimming = false;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsFalling = false;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsGrounded = false;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsCrouch = false;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsDiving = false;
    
    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsRagdoll = false;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsGettingUp = false;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsWaterRagdollRecovery = false;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Ragdoll")
    bool bRagdollEnvironmentOnGround = false;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Ragdoll")
    bool bRagdollEnvironmentInWater = false;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Ragdoll")
    bool bTreatRagdollWaterAsGround = false;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Ragdoll")
    bool bShouldRecoverRagdollInWater = false;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Ragdoll")
    float RagdollEnvironmentWaterLevel = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Ragdoll")
    bool bRagdollMeaningfullySubmerged = false;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Ragdoll")
    float RagdollMaxSubmersionDepth = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Ragdoll")
    float RagdollAverageSubmersionDepth = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bGetUpTrigger = false;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    float IsLieOnBack = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    FVector CapturedMeshLocation = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    FRotator CapturedMeshRotation = FRotator::ZeroRotator;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Weapon")
    bool bHasWeaponIK = false;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Weapon")
    FVector WeaponRightHandIKLocationCS = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Weapon")
    FRotator WeaponRightHandIKRotationCS = FRotator::ZeroRotator;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Weapon")
    FVector WeaponLeftHandIKLocationCS = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Weapon")
    FRotator WeaponLeftHandIKRotationCS = FRotator::ZeroRotator;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Weapon")
    FVector WeaponMuzzleLocationWS = FVector::ZeroVector;



    /** Immediately mirrors native character/ragdoll state into AnimBP-readable variables. */
    void RefreshCharacterAnimationState(float DeltaSeconds = 0.0f);

private:
    virtual void NativeUpdateAnimation(float DeltaSeconds) override;
    virtual void NativeInitializeAnimation() override;

    void RefreshCachedReferences();
    void ResetRuntimeAnimationState();
    void RefreshWeaponIKState();

    UPROPERTY()
    TObjectPtr<UCharacterMovementComponent> Movement;

    UPROPERTY()
    TObjectPtr<UCharacterComponent> Component;

    float PreviousActorYaw = 0.0f;
    bool bHasPreviousActorYaw = false;
    float YawAngularVelocity = 0.0f;
};
