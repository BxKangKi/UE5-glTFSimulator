// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "Character/CharacterDefaultAsset.h"
#include "Interface/WaterInteract.h"
#include "CharacterController.generated.h"

// Forward declarations for the components created earlier.
class UCharacterComponent;
class USpringArmComponent;
class UGameManagerSubSystem;
class UCameraComponent;
class UBuoyancyComponent;
class UPrimitiveComponent;
class UAnimInstance;
class UCharacterLoadAsyncAction;
class UGameUpdateSubSystem;
class UPhysicalMaterial;
class USoundBase;
class UNiagaraSystem;

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FFootstepAssetBinding
{
    GENERATED_BODY()

    /** Collision physical material matched by direct object reference. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Character|Footsteps")
    TObjectPtr<UPhysicalMaterial> PhysicalMaterial = nullptr;

    /** Optional sound played when the assigned physical material is hit. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Character|Footsteps")
    TObjectPtr<USoundBase> Sound = nullptr;

    /** Optional Niagara effect spawned when the assigned physical material is hit. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Character|Footsteps")
    TObjectPtr<UNiagaraSystem> Effect = nullptr;
};

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
    void PrepareForMeshReload();
    void RestoreAfterMeshReload();
    void PrepareForPawnReplacement();
    void RestoreControlAfterRagdollRecovery();
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
    UFUNCTION(BlueprintPure, Category="Character|Loading")
    float GetLoadProgress() const { return LoadProgress; }
    UPROPERTY(BlueprintReadOnly)
    bool bIsMoveable; // Current glTF file path.
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FCharacterDefaultAsset DefaultAsset;
    UCharacterComponent *GetCharacterComponent() { return Component.Get(); }
    USpringArmComponent *GetSpringArm() { return SpringArm.Get(); }
    bool RefreshWaterStateForRagdollRecovery(bool bRagdollBodyInWater, float Level);
    UFUNCTION(BlueprintCallable)
    void TriggerFootstepTrace(EControllerHand FootSide); // Foot-side selector for left/right traces.

    /** Direct physical-material bindings used by footsteps. No asset-name matching is performed. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Character|Footsteps")
    TArray<FFootstepAssetBinding> FootstepAssetBindings;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

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
    TObjectPtr<UBuoyancyComponent> SkeletalMeshBuoyancyComponent;

    /** If true, non-ragdoll players receive velocity impulses from moving simulated physics objects. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Physics Impact")
    bool bReceivePhysicsObjectImpacts = true;

    // Physics impact tuning values are fixed native constants in CharacterController.cpp.
    // Only the feature toggle remains user-editable.
    UFUNCTION()
    void OnLoadCompleted(bool Result);
    UFUNCTION()
    void OnLoadProgress(float Progress);
private:
    UPROPERTY()
    TObjectPtr<UCharacterMovementComponent> Movement;
    UPROPERTY()
    TObjectPtr<UGameManagerSubSystem> SubSystem;
    UPROPERTY()
    TObjectPtr<UCharacterLoadAsyncAction> ActiveLoadAction;
    void SetWaterState(bool bValue, float Level, bool bForceRagdollWaterState = false);
    bool FindDirectWaterLevel(float& OutLevel) const;
    void ClearDryWaterState(float Level, bool bUpdateMovementMode);
    void SyncRagdollWaterStateFromPhysics();
    void UpdateFromGameUpdate(float DeltaSeconds);
    int32 GameUpdateTickHandle = INDEX_NONE;
    int32 CharacterStateBit = 0;
    FVector RawMoveInput;
    float WaterLevel = 0.0f;
    UPROPERTY(Transient)
    float LoadProgress = 0.0f;
    double LastPhysicsObjectImpactTime = -1.0;
    bool bFirstPersonMode = false;
    bool bWaterStateFromOverlap = false;
    bool bWaterStateForcedByRagdoll = false;
    bool bHasSavedAnimationState = false;
    TEnumAsByte<EAnimationMode::Type> SavedAnimationMode;
    UPROPERTY()
    TSubclassOf<UAnimInstance> SavedAnimClass;
    float SavedThirdPersonArmLength = 350.0f;
    FVector SavedThirdPersonSocketOffset = FVector::ZeroVector;
    void OnFootstepTraceCompleted(const FTraceHandle &TraceHandle, FTraceDatum &TraceDatum);
};