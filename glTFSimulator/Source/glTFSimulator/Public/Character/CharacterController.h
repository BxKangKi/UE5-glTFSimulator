// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Character/CharacterDefaultAsset.h"
#include "Interface/WaterInteract.h"
#include "CharacterController.generated.h"

// Forward declarations for the components created earlier.
class UCharacterComponent;
class USpringArmComponent;
class UGameManagerSubSystem;
class UCameraComponent;
class URuntimeBuoyancyComponent;
class UPrimitiveComponent;

UCLASS()
class GLTFSIMULATOR_API ACharacterController : public ACharacter, public IWaterInteract
{
    GENERATED_BODY()

public:
    ACharacterController();
    virtual void EnterWater(const float Level) override;
    virtual void ExitWater(const float Level) override;
    UFUNCTION()
    void Activate(bool bValue);
    void Load(const FString &Path);
    // --- Input Interface ---
    UFUNCTION(BlueprintCallable)
    void MovementInput(const float X, const float Y);
    UFUNCTION(BlueprintCallable)
    void ClearTransientInputState();
    UFUNCTION(BlueprintCallable)
    void CameraInput(const float X, const float Y, const float Sensitive);
    UFUNCTION(BlueprintCallable)
    void Jumping(bool bDoJump);
    UFUNCTION(BlueprintCallable)
    void Sprinting(bool Value);
    UFUNCTION(BlueprintCallable)
    void Crouching(bool Value);
    UFUNCTION(BlueprintCallable)
    void Flying();
    UFUNCTION(BlueprintCallable)
    void ToggleRagdoll();
    UFUNCTION(BlueprintCallable)
    FVector GetBottomLocation();
    UFUNCTION(BlueprintCallable)
    void SetFirstPersonEnabled(bool bEnabled);
    UFUNCTION(BlueprintCallable)
    void ToggleFirstPersonMode();
    UFUNCTION(BlueprintPure)
    bool IsFirstPersonMode() const { return bFirstPersonMode; }
    UPROPERTY(BlueprintReadOnly)
    bool bIsLoaded; // Current glTF file path.
    UPROPERTY(BlueprintReadOnly)
    bool bIsMoveable; // Current glTF file path.
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FCharacterDefaultAsset DefaultAsset;
    UCharacterComponent *GetCharacterComponent() { return Component.Get(); }
    USpringArmComponent *GetSpringArm() { return SpringArm.Get(); }
    bool RefreshWaterStateForRagdollRecovery(bool bRagdollBodyInWater, float Level);
    UFUNCTION(BlueprintCallable)
    void TriggerFootstepTrace(EControllerHand FootSide); // Foot-side selector for left/right traces.

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

    /** Receives hits from simulating physics objects while the player is not ragdolled. */
    UFUNCTION()
    void HandleCapsulePhysicsHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit);
    // --- Components ---
    // Do not move to private section. It needed to be visible in blueprint.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
    TObjectPtr<UCharacterComponent> Component;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
    TObjectPtr<USpringArmComponent> SpringArm;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
    TObjectPtr<UCameraComponent> FollowCamera;

    /** Applies buoyancy to the targeted skeletal mesh while its bodies simulate in water. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
    TObjectPtr<URuntimeBuoyancyComponent> SkeletalMeshBuoyancyComponent;

    /** If true, non-ragdoll players receive velocity impulses from moving simulated physics objects. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Physics Impact")
    bool bReceivePhysicsObjectImpacts = true;

    /** Minimum relative speed, in cm/s, before a physics object can push the player. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Physics Impact", meta=(ClampMin="0.0"))
    float MinPhysicsObjectImpactSpeed = 90.0f;

    /** Scales the received velocity change. Higher values make impacts shove the character harder. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Physics Impact", meta=(ClampMin="0.0"))
    float PhysicsObjectImpactVelocityScale = 0.65f;

    /** Maximum velocity change, in cm/s, applied by a single physics hit. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Physics Impact", meta=(ClampMin="1.0"))
    float MaxPhysicsObjectImpactVelocityChange = 1400.0f;

    /** Small upward component so heavy objects feel like they jolt the player instead of only sliding them. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Physics Impact", meta=(ClampMin="0.0", ClampMax="1.0"))
    float PhysicsObjectImpactUpwardRatio = 0.10f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Physics Impact", meta=(ClampMin="0.0"))
    float MaxPhysicsObjectImpactUpwardVelocity = 220.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Physics Impact", meta=(ClampMin="0.0"))
    float PhysicsObjectImpactCooldownSeconds = 0.08f;
    UFUNCTION()
    void OnLoadCompleted(bool Result);
private:
    UPROPERTY()
    TObjectPtr<UCharacterMovementComponent> Movement;
    UPROPERTY()
    TObjectPtr<UGameManagerSubSystem> SubSystem;
    void SetWaterState(bool bValue, float Level, bool bForceRagdollWaterState = false);
    void SyncRagdollWaterStateFromPhysics();
    int32 CharacterStateBit = 0;
    FVector RawMoveInput;
    float WaterLevel = 0.0f;
    double LastPhysicsObjectImpactTime = -1.0;
    bool bFirstPersonMode = false;
    bool bWaterStateFromOverlap = false;
    bool bWaterStateForcedByRagdoll = false;
    float SavedThirdPersonArmLength = 350.0f;
    FVector SavedThirdPersonSocketOffset = FVector::ZeroVector;
    void OnFootstepTraceCompleted(const FTraceHandle &TraceHandle, FTraceDatum &TraceDatum);
};