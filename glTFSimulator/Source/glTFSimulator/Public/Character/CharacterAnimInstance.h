// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "CharacterAnimInstance.generated.h"

class UCharacterMovementComponent;
class UCharacterComponent;

UCLASS()
class GLTFSIMULATOR_API UCharacterAnimInstance : public UAnimInstance
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    FVector Velocity;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    float Speed;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    float MoveSpeed;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    float UpSpeed;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bShouldMove;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsFlying;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsSwimming;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsFalling;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsGrounded;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsCrouch;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsDiving;
    
    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsRagdoll;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsGettingUp;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bIsWaterRagdollRecovery;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Ragdoll")
    bool bRagdollEnvironmentOnGround;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Ragdoll")
    bool bRagdollEnvironmentInWater;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Ragdoll")
    bool bTreatRagdollWaterAsGround;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Ragdoll")
    bool bShouldRecoverRagdollInWater;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Ragdoll")
    float RagdollEnvironmentWaterLevel;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Ragdoll")
    bool bRagdollMeaningfullySubmerged;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Ragdoll")
    float RagdollMaxSubmersionDepth;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation|Ragdoll")
    float RagdollAverageSubmersionDepth;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    bool bGetUpTrigger;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    float IsLieOnBack;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    FVector CapturedMeshLocation;

    UPROPERTY(BlueprintReadOnly, Category = "Character|Animation")
    FRotator CapturedMeshRotation;

private:
    virtual void NativeUpdateAnimation(float DeltaSeconds) override;
    virtual void NativeInitializeAnimation() override;

    UPROPERTY()
    TObjectPtr<UCharacterMovementComponent> Movement;

    UPROPERTY()
    TObjectPtr<UCharacterComponent> Component;
};