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
class USkeletalMesh;
class USkeleton;
class UPhysicsAsset;
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
    /** Starts a single on-demand character load. The previous runtime character resources are detached first.
     *  Must be called on the game thread. File parsing and mesh construction continue asynchronously. */
    void Load(const FString &Path);

    /** Cancels the current character request and detaches any generated runtime resources. Game-thread only. */
    void CancelCharacterLoad(bool bRestoreDefaultMesh = true);

    /** Atomically installs a completed runtime character mesh. UObject/component mutation is game-thread only. */
    bool CommitRuntimeCharacterResources(USkeletalMesh* SkeletalMesh, UPhysicsAsset* PhysicsAsset, USkeleton* RuntimeSkeleton);

    /** Detaches the current runtime character mesh and releases all strong references without forcing a blocking GC. */
    void ReleaseRuntimeCharacterResources(bool bRestoreDefaultMesh = true);

    /** Internal completion hook used to serialize cancelled/in-flight glTFRuntime requests. Game-thread only. */
    void HandleCharacterLoadActionReleased(UCharacterLoadAsyncAction* ReleasedAction);

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
    UFUNCTION(BlueprintPure, Category="Character|Loading")
    bool WasLastMeshLoadSuccessful() const { return bLastMeshLoadSucceeded; }
    UPROPERTY(BlueprintReadOnly)
    bool bIsMoveable; // Current glTF file path.
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FCharacterDefaultAsset DefaultAsset;
    UCharacterComponent *GetCharacterComponent() { return Component.Get(); }
    USpringArmComponent *GetSpringArm() { return SpringArm.Get(); }
    UCameraComponent *GetFollowCameraComponent() const { return FollowCamera.Get(); }
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

    /** Latest on-demand request waiting for a cancelled glTFRuntime worker/finalizer to drain. */
    FString QueuedCharacterPath;

    /** Explicit ownership of the currently installed runtime resources. Resetting these after detaching the mesh
     *  lets Unreal's incremental GC reclaim the old character without a synchronous collection hitch. */
    UPROPERTY(Transient)
    TObjectPtr<USkeletalMesh> RuntimeSkeletalMesh = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UPhysicsAsset> RuntimePhysicsAsset = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<USkeleton> RuntimeSkeleton = nullptr;
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
    UPROPERTY(Transient)
    bool bLastMeshLoadSucceeded = false;
    double LastPhysicsObjectImpactTime = -1.0;
    bool bFirstPersonMode = false;
    bool bWaterStateFromOverlap = false;
    bool bWaterStateForcedByRagdoll = false;
    bool bHasSavedAnimationState = false;
    /** Requests one authoritative mesh/physics cleanup after load or ragdoll transitions. */
    bool bNeedsPostRagdollCleanup = true;
    /** Low-frequency safety audit replaces the previous full skeleton scan every frame. */
    float PhysicsStateAuditAccumulator = 0.0f;
    TEnumAsByte<EAnimationMode::Type> SavedAnimationMode;
    UPROPERTY()
    TSubclassOf<UAnimInstance> SavedAnimClass;
    float SavedThirdPersonArmLength = 350.0f;
    FVector SavedThirdPersonSocketOffset = FVector::ZeroVector;
    void OnFootstepTraceCompleted(const FTraceHandle &TraceHandle, FTraceDatum &TraceDatum);
};
