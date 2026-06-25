// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Character/CharacterController.h"
#include "Character/CharacterComponent.h"
#include "Character/CharacterFunctionLibrary.h"
#include "Character/InputFunctionLibrary.h"
#include "GameFramework/SpringArmComponent.h"
#include "System/GameManagerSubSystem.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Character/CharacterLoadAsyncAction.h"
#include "World/WaterActor.h"
#include "World/RuntimeBuoyancyComponent.h"
#include "System/MacroLibrary.h"
#include "Components/PrimitiveComponent.h"


ACharacterController::ACharacterController()
{
    PrimaryActorTick.bCanEverTick = true;
    // 1. Create gameplay components.
    Component = CreateDefaultSubobject<UCharacterComponent>(TEXT("CharacterComponent"));
    // Camera setup.
    SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
    SpringArm->SetupAttachment(RootComponent);
    SpringArm->bUsePawnControlRotation = true;
    FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
    FollowCamera->SetupAttachment(SpringArm);
    FollowCamera->bUsePawnControlRotation = false;

    SkeletalMeshBuoyancyComponent = CreateDefaultSubobject<URuntimeBuoyancyComponent>(TEXT("SkeletalMeshBuoyancy"));
    if (SkeletalMeshBuoyancyComponent)
    {
        if (USkeletalMeshComponent* MeshComponent = GetMesh())
        {
            SkeletalMeshBuoyancyComponent->SetTargetComponentName(MeshComponent->GetFName());
        }

        FRuntimeBuoyancyPhysicsSettings CharacterBuoyancyPhysics;
        CharacterBuoyancyPhysics.BuoyancyAccelerationScale = 0.992f;
        CharacterBuoyancyPhysics.WaterLinearDragCoefficient = 2.35f;
        CharacterBuoyancyPhysics.WaterQuadraticDragCoefficient = 0.00175f;
        CharacterBuoyancyPhysics.LinearWaterDamping = 3.25f;
        CharacterBuoyancyPhysics.AngularWaterDamping = 4.10f;
        CharacterBuoyancyPhysics.MaxDragForcePerPoint = 135000.0f;
        CharacterBuoyancyPhysics.HighSpeedDragStartSpeed = 480.0f;
        CharacterBuoyancyPhysics.HighSpeedDragFullSpeed = 2200.0f;
        CharacterBuoyancyPhysics.HighSpeedDragMultiplier = 3.35f;
        CharacterBuoyancyPhysics.MaxImpulseVelocityChangePerStep = 185.0f;
        CharacterBuoyancyPhysics.bClampLinearVelocity = true;
        CharacterBuoyancyPhysics.MaxLinearSpeed = 950.0f;
        CharacterBuoyancyPhysics.SurfaceEntryDragAlphaPower = 0.82f;
        CharacterBuoyancyPhysics.WaterDragMultiplier = 3.25f;
        CharacterBuoyancyPhysics.WaterDragMultiplierMinSubmergedAlpha = 0.03f;
        CharacterBuoyancyPhysics.DownwardWaterDragMultiplier = 3.90f;
        CharacterBuoyancyPhysics.bLimitDownwardSinkSpeed = true;
        CharacterBuoyancyPhysics.MaxDownwardSinkSpeed = 75.0f;
        CharacterBuoyancyPhysics.SinkSpeedSoftClampInterpSpeed = 8.5f;
        CharacterBuoyancyPhysics.SinkSpeedClampMinSubmergedAlpha = 0.18f;
        CharacterBuoyancyPhysics.bClampAngularVelocity = true;
        CharacterBuoyancyPhysics.MaxAngularSpeed = 4.8f;
        SkeletalMeshBuoyancyComponent->SetCommonPhysicsSettings(CharacterBuoyancyPhysics);

        FRuntimeSkeletalBuoyancySettings SkeletalBuoyancySettings = SkeletalMeshBuoyancyComponent->GetSkeletalMeshSettings();
        for (FRuntimeSkeletalBuoyancyBoneRule& Rule : SkeletalBuoyancySettings.BoneRules)
        {
            if (Rule.RuleName == FName(TEXT("DistalLimbs")))
            {
                Rule.PhysicsSettings = CharacterBuoyancyPhysics;
                Rule.PhysicsSettings.BuoyancyAccelerationScale *= 0.78f;
                Rule.PhysicsSettings.WaterLinearDragCoefficient *= 0.95f;
                Rule.PhysicsSettings.WaterQuadraticDragCoefficient *= 0.95f;
                Rule.PhysicsSettings.WaterDragMultiplier *= 0.90f;
                Rule.PhysicsSettings.LinearWaterDamping = 2.85f;
                Rule.PhysicsSettings.AngularWaterDamping = 3.80f;
                Rule.PhysicsSettings.MaxImpulseVelocityChangePerStep = 165.0f;
                Rule.PhysicsSettings.bClampLinearVelocity = true;
                Rule.PhysicsSettings.MaxLinearSpeed = 1050.0f;
                Rule.PhysicsSettings.DownwardWaterDragMultiplier = 3.20f;
                Rule.PhysicsSettings.MaxDownwardSinkSpeed = 95.0f;
                Rule.PhysicsSettings.SinkSpeedSoftClampInterpSpeed = 7.0f;
                Rule.PhysicsSettings.bClampAngularVelocity = true;
                Rule.PhysicsSettings.MaxAngularSpeed = 6.0f;
            }
        }
        SkeletalMeshBuoyancyComponent->SetSkeletalMeshSettings(SkeletalBuoyancySettings);
    }

    if (UCapsuleComponent* Capsule = GetCapsuleComponent())
    {
        Capsule->SetNotifyRigidBodyCollision(true);
        Capsule->SetGenerateOverlapEvents(true);
    }
}

void ACharacterController::BeginPlay()
{
    Super::BeginPlay();
    if (DefaultAsset.IMC)
    {
        UInputFunctionLibrary::AddInputMappingContext(this, DefaultAsset.IMC, 0);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("Input Mapping Context is not assigned in the editor."));
    }
    bIsLoaded = false;
    SavedThirdPersonArmLength = SpringArm->TargetArmLength > 1.0f ? SpringArm->TargetArmLength : 350.0f;
    SavedThirdPersonSocketOffset = SpringArm->SocketOffset;
    // Initialize Component
    Movement = GetCharacterMovement();
    if (IsValid(Movement))
    {
        Movement->bEnablePhysicsInteraction = true;
    }
    if (UCapsuleComponent* Capsule = GetCapsuleComponent())
    {
        Capsule->SetNotifyRigidBodyCollision(true);
        Capsule->OnComponentHit.RemoveDynamic(this, &ACharacterController::HandleCapsulePhysicsHit);
        Capsule->OnComponentHit.AddDynamic(this, &ACharacterController::HandleCapsulePhysicsHit);
    }
    SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    if (!IsValid(SubSystem))
        return;
    SubSystem->SetPlayerActor(this);
    SubSystem->SetCameraComponent(FollowCamera);
    SetActorLocation(SubSystem->GetPlayerLocation(), false, nullptr, ETeleportType::TeleportPhysics);
    Activate(false);
}

void ACharacterController::Load(const FString &Path)
{
    // 1. Create the async load action and pass WorldContextObject, Owner, and Path.
    UCharacterLoadAsyncAction *LoadAction = UCharacterLoadAsyncAction::LoadCharacterAsync(this, this, Path);
    if (LoadAction)
    {
        // 2. Connect completed delegate (Optional)
        // Optional hook for UI/state work after loading completes.
        LoadAction->OnCompleted.AddDynamic(this, &ACharacterController::OnLoadCompleted);
        // 3. Start async loading
        // Activate() runs glTF loading, BoneMap loading, and mesh creation in order.
        LoadAction->Activate();
    }
}

void ACharacterController::OnLoadCompleted(bool Result)
{
    if (!Result)
    {
        USkeletalMeshComponent *MeshComp = GetMesh();
        if (IsValid(MeshComp) && IsValid(DefaultAsset.SkeletalMesh))
        {
            MeshComp->SetSkinnedAssetAndUpdate(DefaultAsset.SkeletalMesh, true);
        }
        UE_LOG(LogTemp, Warning, TEXT("Character glTF load failed. Falling back to the default mesh so world/runtime loading can continue."));
    }

    // bIsLoaded is used by WorldManager as a load-completion gate. Treat the
    // default-mesh fallback as a completed load; otherwise LoadPlayerAsync loops
    // forever and RuntimeGameplayManager never starts.
    bIsLoaded = true;
}


void ACharacterController::HandleCapsulePhysicsHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit)
{
    if (!bReceivePhysicsObjectImpacts || !IsValid(OtherActor) || OtherActor == this || !IsValid(OtherComp) || !IsValid(Movement) || !IsValid(Component.Get()))
    {
        return;
    }

    if (Component->IsRagdollActive() || !OtherComp->IsSimulatingPhysics())
    {
        return;
    }

    UWorld* World = GetWorld();
    const double Now = World ? World->GetTimeSeconds() : 0.0;
    if (LastPhysicsObjectImpactTime >= 0.0 && Now - LastPhysicsObjectImpactTime < PhysicsObjectImpactCooldownSeconds)
    {
        return;
    }

    const FVector ActorLocation = GetActorLocation();
    const FVector HitImpactPoint(Hit.ImpactPoint.X, Hit.ImpactPoint.Y, Hit.ImpactPoint.Z);
    const FVector HitNormal(Hit.Normal.X, Hit.Normal.Y, Hit.Normal.Z);

    FVector PushDirection = (ActorLocation - OtherComp->GetComponentLocation()).GetSafeNormal();
    if (PushDirection.IsNearlyZero() && !HitImpactPoint.IsNearlyZero())
    {
        PushDirection = (ActorLocation - HitImpactPoint).GetSafeNormal();
    }
    if (PushDirection.IsNearlyZero())
    {
        PushDirection = (-HitNormal).GetSafeNormal();
    }

    FVector HorizontalDirection(PushDirection.X, PushDirection.Y, 0.0f);
    if (!HorizontalDirection.Normalize())
    {
        HorizontalDirection = FVector(-HitNormal.X, -HitNormal.Y, 0.0f);
        if (!HorizontalDirection.Normalize())
        {
            return;
        }
    }

    const FVector ImpactPoint = HitImpactPoint.IsNearlyZero() ? OtherComp->GetComponentLocation() : HitImpactPoint;
    const FVector OtherVelocity = OtherComp->GetPhysicsLinearVelocityAtPoint(ImpactPoint);
    const FVector RelativeVelocity = OtherVelocity - GetVelocity();
    const float RelativeImpactSpeed = FVector::DotProduct(RelativeVelocity, HorizontalDirection);

    constexpr float ReferenceCharacterMassKg = 80.0f;
    const float ImpulseSpeed = NormalImpulse.Size() / ReferenceCharacterMassKg;
    float ImpactSpeed = FMath::Max(RelativeImpactSpeed, ImpulseSpeed);

    if (ImpactSpeed < MinPhysicsObjectImpactSpeed)
    {
        return;
    }

    ImpactSpeed = FMath::Clamp(ImpactSpeed * PhysicsObjectImpactVelocityScale, 0.0f, MaxPhysicsObjectImpactVelocityChange);

    FVector VelocityDelta = HorizontalDirection * ImpactSpeed;
    const float UpwardVelocity = FMath::Clamp(ImpactSpeed * PhysicsObjectImpactUpwardRatio, 0.0f, MaxPhysicsObjectImpactUpwardVelocity);
    if (UpwardVelocity > 0.0f)
    {
        VelocityDelta.Z = UpwardVelocity;
    }

    if (!VelocityDelta.IsNearlyZero())
    {
        Movement->AddImpulse(VelocityDelta, true);
        LastPhysicsObjectImpactTime = Now;
    }
}

void ACharacterController::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (GetVelocity().Z <= 0.0f)
    {
        CharacterStateBit &= ~STATE_JUMPING;
    }
    SyncRagdollWaterStateFromPhysics();
    Component->UpdateComponent(DeltaSeconds, RawMoveInput, CharacterStateBit, WaterLevel);

    // Buoyancy is only allowed to touch simulated ragdoll bodies.  When the character is not ragdolled,
    // keep the visual mesh attached/visible so stale physics state cannot make it vanish or drift away.
    if (!Component->IsRagdollActive() && !Component->IsGettingUp())
    {
        if (USkeletalMeshComponent* MeshComp = GetMesh())
        {
            if (UCharacterFunctionLibrary::HasNonSecondarySimulatingPhysicsBodies(*MeshComp))
            {
                UCharacterFunctionLibrary::DisableRagdollPhysicsButKeepSecondary(*MeshComp);
            }
            else
            {
                UCharacterFunctionLibrary::KeepSecondaryPhysicsBodies(*MeshComp);
            }
            MeshComp->SetVisibility(true, true);
            if (!bFirstPersonMode)
            {
                MeshComp->SetOwnerNoSee(false);
            }
            if (UCapsuleComponent* Capsule = GetCapsuleComponent())
            {
                if (MeshComp->GetAttachParent() != Capsule)
                {
                    MeshComp->AttachToComponent(Capsule, FAttachmentTransformRules::KeepRelativeTransform);
                }
                MeshComp->SetRelativeLocation(FVector(0.0f, 0.0f, -90.0f));
                MeshComp->SetRelativeRotation(FRotator(0.0f, 270.0f, 0.0f));
            }
        }
    }

    if (!Component->IsRagdollActive() && Component->IsRagdollDamage())
    {
        Component->SetRagdollActive(true);
    }
    SubSystem->SetPlayerLocation(GetActorLocation());
}

void ACharacterController::EnterWater(const float Level)
{
    bWaterStateFromOverlap = true;
    SetWaterState(true, Level);
}

void ACharacterController::ExitWater(const float Level)
{
    bWaterStateFromOverlap = false;
    if (!bWaterStateForcedByRagdoll)
    {
        SetWaterState(false, Level);
    }
    else
    {
        WaterLevel = Level;
    }
}

void ACharacterController::Activate(bool bValue)
{
    Movement->SetActive(bValue);
    GetCapsuleComponent()->SetActive(bValue);
    AWaterActor::CheckOverlappingWater(this);
}

// --- Input handling ---

void ACharacterController::MovementInput(const float X, const float Y)
{
    RawMoveInput.X = X;
    RawMoveInput.Y = Y;
}

void ACharacterController::ClearTransientInputState()
{
    RawMoveInput = FVector::ZeroVector;
    CharacterStateBit &= ~(STATE_JUMPING | STATE_SPRINT | STATE_CROUCH);

    StopJumping();
    UnCrouch();

    if (IsValid(Component.Get()))
    {
        Component->ResetMovementState();
    }

    if (IsValid(Movement))
    {
        Movement->ConsumeInputVector();
        Movement->StopMovementImmediately();
    }
}

void ACharacterController::CameraInput(const float X, const float Y, const float Sensitive)
{
    AddControllerYawInput(X * Sensitive);
    AddControllerPitchInput(Y * Sensitive);
}

void ACharacterController::Jumping(bool bDoJump)
{
    if (bDoJump)
    {
        RawMoveInput.Z = 1.0f;
        if (Movement->IsMovingOnGround())
        {
            Jump();
            CharacterStateBit |= STATE_JUMPING; // Jumping Bit On
        }
    }
    else
    {
        RawMoveInput.Z = FMath::Min(0.0f, RawMoveInput.Z);
        StopJumping();
        CharacterStateBit &= ~STATE_JUMPING;
    }
}

void ACharacterController::Sprinting(bool Value)
{
    if (Value)
        CharacterStateBit |= STATE_SPRINT;
    else
        CharacterStateBit &= ~STATE_SPRINT;
}

void ACharacterController::Crouching(bool Value)
{
    if (Value && !Movement->IsFalling())
    {
        RawMoveInput.Z = -1.0f;
        CharacterStateBit |= STATE_CROUCH;
    }
    else
    {
        RawMoveInput.Z = FMath::Max(0.0f, RawMoveInput.Z);
        CharacterStateBit &= ~STATE_CROUCH;
    }
}

void ACharacterController::Flying()
{
    if (!IsValid(Movement))
    {
        return;
    }

    if (IsValid(Component.Get()) && Component->IsRagdollTransitionInProgress())
    {
        // Flying is intentionally blocked during active ragdoll and during the get-up/blend-out window.
        // Otherwise the movement mode can be changed while the mesh/actor transform is still being restored.
        return;
    }

    if (IsValid(Component.Get()) && !Component->IsRagdollActive() && !Component->IsGettingUp() && Component->GetRagdollWeight() <= KINDA_SMALL_NUMBER)
    {
        // A short post-water-ragdoll swim lock is only meant to protect the animation transition.
        // Explicit Flying input must override it immediately. Also drop the forced-by-ragdoll
        // controller flag so the next water sync cannot put the movement mode back to Swimming.
        Component->ClearRagdollSwimmingRecoveryLock(true);
        bWaterStateForcedByRagdoll = false;
    }

    const bool bWasFlying = Movement->IsFlying();
    Movement->StopMovementImmediately();
    if (bWasFlying)
    {
        float CurrentWaterLevel = WaterLevel;
        const bool bStillInWater = UCharacterFunctionLibrary::IsStateActive(CharacterStateBit, STATE_WATER)
            || AWaterActor::FindWaterLevelAtLocation(this, GetActorLocation(), CurrentWaterLevel)
            || AWaterActor::FindWaterLevelAtLocation(this, GetBottomLocation(), CurrentWaterLevel);

        if (bStillInWater)
        {
            WaterLevel = CurrentWaterLevel;
            CharacterStateBit |= STATE_WATER;
            Movement->SetMovementMode(MOVE_Swimming);
        }
        else
        {
            CharacterStateBit &= ~STATE_WATER;
            Movement->SetMovementMode(MOVE_Falling);
        }
        CharacterStateBit &= ~STATE_FLYING;
    }
    else
    {
        Movement->SetMovementMode(MOVE_Flying);
        CharacterStateBit |= STATE_FLYING;
    }
}

void ACharacterController::ToggleRagdoll()
{
    const bool bNewState = !Component->IsRagdollActive();
    Component->SetRagdollActive(bNewState);
}

void ACharacterController::SetWaterState(bool bValue, float Level, bool bForceRagdollWaterState)
{
    const bool bWasInWater = UCharacterFunctionLibrary::IsStateActive(CharacterStateBit, STATE_WATER);
    const bool bStateChanged = bWasInWater != bValue;
    const bool bLevelChanged = !FMath::IsNearlyEqual(WaterLevel, Level, 1.0f);
    WaterLevel = Level;

    if (bValue)
    {
        CharacterStateBit |= STATE_WATER;
    }
    else
    {
        CharacterStateBit &= ~STATE_WATER;
    }

    const bool bRagdollTransitionActive = IsValid(Component.Get()) && Component->IsRagdollTransitionInProgress();
    if (IsValid(Component.Get()) && (bStateChanged || bLevelChanged || bForceRagdollWaterState || bRagdollTransitionActive))
    {
        Component->SetRagdollWaterState(bValue, bForceRagdollWaterState);
    }

    if (bValue && IsValid(Movement) && bRagdollTransitionActive)
    {
        Movement->StopMovementImmediately();
        if (Movement->MovementMode != MOVE_Swimming)
        {
            Movement->SetMovementMode(MOVE_Swimming);
        }
    }

    if (IsValid(SkeletalMeshBuoyancyComponent) && (bStateChanged || bLevelChanged || bForceRagdollWaterState))
    {
        if (bValue)
        {
            SkeletalMeshBuoyancyComponent->EnterWater(Level);
        }
        else
        {
            SkeletalMeshBuoyancyComponent->ExitWater(Level);
        }
    }
}

bool ACharacterController::RefreshWaterStateForRagdollRecovery(bool bRagdollBodyInWater, float Level)
{
    const bool bCommittedWaterRecovery = IsValid(Component.Get())
        && (bRagdollBodyInWater || Component->ShouldRecoverRagdollInWaterFromEnvironment() || Component->IsRecoveringRagdollInWater() || Component->ShouldKeepSwimmingAfterWaterRagdoll());

    if (IsValid(Component.Get()) && Component->ShouldTreatRagdollWaterAsGround() && !bCommittedWaterRecovery)
    {
        bWaterStateFromOverlap = false;
        bWaterStateForcedByRagdoll = false;
        SetWaterState(false, Level, true);
        return false;
    }

    float EffectiveLevel = bRagdollBodyInWater ? Level : WaterLevel;
    bool bActorPointInWater = false;
    const bool bUseRagdollProbeOnly = IsValid(Component.Get()) && Component->IsRagdollTransitionInProgress();

    if (!bRagdollBodyInWater && !bUseRagdollProbeOnly)
    {
        bActorPointInWater = AWaterActor::FindWaterLevelAtLocation(this, GetActorLocation(), EffectiveLevel)
            || AWaterActor::FindWaterLevelAtLocation(this, GetBottomLocation(), EffectiveLevel);
    }

    // Direct ragdoll recovery must be decided by the current ragdoll body/bone positions, not by
    // the capsule/actor that may already have been moved toward the recovery target.
    const bool bShouldBeInWater = bRagdollBodyInWater || bActorPointInWater;
    if (bShouldBeInWater)
    {
        bWaterStateForcedByRagdoll = !bWaterStateFromOverlap;
        SetWaterState(true, EffectiveLevel, true);
    }
    else
    {
        bWaterStateFromOverlap = false;
        bWaterStateForcedByRagdoll = false;
        SetWaterState(false, EffectiveLevel, true);
    }

    return bShouldBeInWater;
}

void ACharacterController::SyncRagdollWaterStateFromPhysics()
{
    if (!IsValid(Component.Get()))
    {
        return;
    }

    const bool bRagdollLikeState = Component->IsRagdollActive() || Component->IsGettingUp() || Component->GetRagdollWeight() > 0.0f;

    if (Component->IsLandRagdollRecoveryOverridingWater())
    {
        bWaterStateForcedByRagdoll = false;
        CharacterStateBit &= ~STATE_WATER;
        Component->SetRagdollWaterState(false, true);
        if (IsValid(Movement) && Movement->MovementMode == MOVE_Swimming)
        {
            Movement->StopMovementImmediately();
            Movement->DisableMovement();
        }
        return;
    }

    if (!bRagdollLikeState)
    {
        if (bWaterStateFromOverlap && !UCharacterFunctionLibrary::IsStateActive(CharacterStateBit, STATE_WATER))
        {
            SetWaterState(true, WaterLevel, true);
        }
        if (Component->ShouldKeepSwimmingAfterWaterRagdoll())
        {
            float LockWaterLevel = WaterLevel;
            const bool bLockStillInWater = AWaterActor::FindWaterLevelAtLocation(this, GetActorLocation(), LockWaterLevel)
                || AWaterActor::FindWaterLevelAtLocation(this, GetBottomLocation(), LockWaterLevel);

            if (bLockStillInWater)
            {
                bWaterStateForcedByRagdoll = !bWaterStateFromOverlap;
                CharacterStateBit |= STATE_WATER;
                WaterLevel = LockWaterLevel;
                Component->SetRagdollWaterState(true);
                // Do not stop movement or force MOVE_Swimming here.  The post-recovery lock is
                // animation protection only once ragdoll/get-up is over; normal UpdateComponent
                // will choose Swimming when appropriate, and player Flying input must remain valid.
                return;
            }

            bWaterStateFromOverlap = false;
            Component->SetRagdollWaterState(false, true);
            SetWaterState(false, LockWaterLevel, true);
        }

        if (bWaterStateForcedByRagdoll)
        {
            float ActorWaterLevel = WaterLevel;
            const bool bActorStillInWater = AWaterActor::FindWaterLevelAtLocation(this, GetActorLocation(), ActorWaterLevel)
                || AWaterActor::FindWaterLevelAtLocation(this, GetBottomLocation(), ActorWaterLevel);

            if (bActorStillInWater)
            {
                CharacterStateBit |= STATE_WATER;
                WaterLevel = ActorWaterLevel;
                Component->SetRagdollWaterState(true);
                // Keep the water state, but do not force a movement mode now that the ragdoll recovery is done.
                // UpdateComponent will select Swimming unless the user explicitly switched to Flying.
                return;
            }

            bWaterStateForcedByRagdoll = false;
            if (!bWaterStateFromOverlap)
            {
                SetWaterState(false, WaterLevel, true);
            }
        }
        return;
    }

    if (Component->RefreshRagdollWaterStateForAnimation())
    {
        const FCharacterRagdollEnvironmentState RagdollWaterState = Component->GetRagdollEnvironmentState();
        const float DetectedWaterLevel = RagdollWaterState.WaterLevel;
        const bool bWasForcedByRagdoll = bWaterStateForcedByRagdoll;
        bWaterStateForcedByRagdoll = true;
        if (!bWasForcedByRagdoll || !UCharacterFunctionLibrary::IsStateActive(CharacterStateBit, STATE_WATER) || !FMath::IsNearlyEqual(WaterLevel, DetectedWaterLevel, 1.0f))
        {
            SetWaterState(true, DetectedWaterLevel);
        }
        else
        {
            WaterLevel = DetectedWaterLevel;
            Component->SetRagdollWaterState(true);
        }
        return;
    }

    if (bWaterStateForcedByRagdoll)
    {
        bWaterStateForcedByRagdoll = false;
        if (!bWaterStateFromOverlap)
        {
            SetWaterState(false, WaterLevel, true);
        }
    }

    if (bRagdollLikeState)
    {
        // Active ragdoll water state is driven only by the ragdoll mesh/bodies.  The capsule can be
        // far from the simulated pose while get-up positioning is being prepared.
        bWaterStateFromOverlap = false;
        SetWaterState(false, WaterLevel, true);
        if (IsValid(Movement) && Movement->MovementMode == MOVE_Swimming)
        {
            Movement->StopMovementImmediately();
            Movement->DisableMovement();
        }
    }
}

FVector ACharacterController::GetBottomLocation()
{
    const FVector Location = GetActorLocation();
    const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    // Move only the Z value down by half the capsule height.
    return FVector(Location.X, Location.Y, Location.Z - HalfHeight);
}

void ACharacterController::SetFirstPersonEnabled(bool bEnabled)
{
    if (!IsValid(SpringArm) || !IsValid(FollowCamera))
    {
        return;
    }

    if (!bFirstPersonMode)
    {
        SavedThirdPersonArmLength = SpringArm->TargetArmLength > 1.0f ? SpringArm->TargetArmLength : SavedThirdPersonArmLength;
        SavedThirdPersonSocketOffset = SpringArm->SocketOffset;
    }

    bFirstPersonMode = bEnabled;
    if (bFirstPersonMode)
    {
        SpringArm->TargetArmLength = 0.0f;
        SpringArm->SocketOffset = FVector(0.0f, 0.0f, 0.0f);
        FollowCamera->SetRelativeLocation(FVector::ZeroVector);
        if (USkeletalMeshComponent* MeshComp = GetMesh())
        {
            MeshComp->SetOwnerNoSee(true);
        }
    }
    else
    {
        SpringArm->TargetArmLength = SavedThirdPersonArmLength;
        SpringArm->SocketOffset = SavedThirdPersonSocketOffset;
        FollowCamera->SetRelativeLocation(FVector::ZeroVector);
        if (USkeletalMeshComponent* MeshComp = GetMesh())
        {
            MeshComp->SetOwnerNoSee(false);
        }
    }
}

void ACharacterController::ToggleFirstPersonMode()
{
    SetFirstPersonEnabled(!bFirstPersonMode);
}



void ACharacterController::TriggerFootstepTrace(EControllerHand FootSide)
{
    UWorld* World = GetWorld();
    if (!World) return;

    // 1. Pick the foot bone that should be traced.
    FName BoneName = (FootSide == EControllerHand::Left) ? BONE_LEFT_FOOT : BONE_RIGHT_FOOT;
    FVector Start = GetMesh()->GetBoneLocation(BoneName) + FVector(0.0f, 0.0f, 10.0f);
    FVector End = Start - FVector(0.0f, 0.0f, 50.0f); 

    FCollisionQueryParams Params;
    // Use complex tracing so the hit result can return the physical material.
    Params.bTraceComplex = true; 
    Params.bReturnPhysicalMaterial = true; // Required: ask the trace to return the physical material.
    Params.AddIgnoredActor(this); 

    FTraceDelegate TraceDelegate;
    TraceDelegate.BindUObject(this, &ACharacterController::OnFootstepTraceCompleted);

    // Submit the asynchronous footstep raycast.
    World->AsyncLineTraceByChannel(
        EAsyncTraceType::Single,
        Start,
        End,
        ECC_Visibility,
        Params,
        FCollisionResponseParams::DefaultResponseParam,
        &TraceDelegate
    );
}

void ACharacterController::OnFootstepTraceCompleted(const FTraceHandle& TraceHandle, FTraceDatum& TraceDatum)
{
    // Nothing was hit, so there is no surface to resolve.
    if (TraceDatum.OutHits.Num() == 0) return;

    const FHitResult& HitResult = TraceDatum.OutHits[0];
    
    // 2. Resolve the physical material weak pointer from the hit result.
    if (HitResult.PhysMaterial.IsValid())
    {
        UPhysicalMaterial* HitPhysMat = HitResult.PhysMaterial.Get();
        if (HitPhysMat)
        {
            // Use the physical material asset name, for example "PM_Concrete" or "PM_Wood".
            FString PhysMatName = HitPhysMat->GetName();
#if WITH_EDITOR
            UE_LOG(LogTemp, Log, TEXT("Footstep raycast hit physical material: %s"), *PhysMatName);
#endif
            // 3. Branch by surface material to play matching sound/effects.
            if (PhysMatName.Contains(TEXT("Concrete")))
            {
                // Concrete footstep playback hook, for example PlaySoundAtLocation.
            }
            else if (PhysMatName.Contains(TEXT("Grass")))
            {
                // Grass footstep playback hook.
            }
            else if (PhysMatName.Contains(TEXT("Wood")))
            {
                // Wood footstep playback hook.
            }
            else if (PhysMatName.Contains(TEXT("Glass")))
            {
                // Glass footstep playback hook.
            }
        }
    }
    else
    {
        // Fallback for surfaces that have collision but no physical material, such as default footsteps.
#if WITH_EDITOR
        UE_LOG(LogTemp, Warning, TEXT("Footstep trace hit a collider, but no PhysMaterial is assigned."));
#endif
    }
}