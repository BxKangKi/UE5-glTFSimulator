// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Character/CharacterController.h"
#include "Character/CharacterComponent.h"
#include "Character/CharacterFunctionLibrary.h"
#include "Character/InputFunctionLibrary.h"
#include "Character/PlayerCharacterController.h"
#include "GameFramework/SpringArmComponent.h"
#include "System/GameManagerSubSystem.h"
#include "System/GameUpdateSubSystem.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Character/CharacterLoadAsyncAction.h"
#include "World/WaterActor.h"
#include "World/BuoyancyComponent.h"
#include "System/MacroLibrary.h"
#include "Components/PrimitiveComponent.h"

namespace CharacterControllerTuning
{
    static const FVector MeshDefaultRelativeLocation(0.0f, 0.0f, -90.0f);
    static const FRotator MeshDefaultRelativeRotation(0.0f, 270.0f, 0.0f);

    constexpr float DefaultThirdPersonArmLength = 350.0f;
    constexpr float WaterLevelChangeToleranceCm = 1.0f;
    constexpr float MinPhysicsObjectImpactSpeed = 90.0f;
    constexpr float PhysicsObjectImpactVelocityScale = 0.65f;
    constexpr float MaxPhysicsObjectImpactVelocityChange = 1400.0f;
    constexpr float PhysicsObjectImpactUpwardRatio = 0.10f;
    constexpr float MaxPhysicsObjectImpactUpwardVelocity = 220.0f;
    constexpr float PhysicsObjectImpactCooldownSeconds = 0.08f;
}

ACharacterController::ACharacterController()
{
    PrimaryActorTick.bCanEverTick = false;
    // 1. Create gameplay components.
    Component = CreateDefaultSubobject<UCharacterComponent>(TEXT("CharacterComponent"));
    // Camera setup.
    SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
    SpringArm->SetupAttachment(RootComponent);
    SpringArm->bUsePawnControlRotation = true;
    FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
    FollowCamera->SetupAttachment(SpringArm);
    FollowCamera->bUsePawnControlRotation = false;

    SkeletalMeshBuoyancyComponent = CreateDefaultSubobject<UBuoyancyComponent>(TEXT("SkeletalMeshBuoyancy"));
    if (SkeletalMeshBuoyancyComponent)
    {
        if (USkeletalMeshComponent* MeshComponent = GetMesh())
        {
            SkeletalMeshBuoyancyComponent->SetTargetComponentName(MeshComponent->GetFName());
        }

        FBuoyancyPhysicsSettings CharacterBuoyancyPhysics;
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

        FSkeletalBuoyancySettings SkeletalBuoyancySettings = SkeletalMeshBuoyancyComponent->GetSkeletalMeshSettings();
        for (FSkeletalBuoyancyBoneRule& Rule : SkeletalBuoyancySettings.BoneRules)
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
    SavedThirdPersonArmLength = SpringArm->TargetArmLength > 1.0f ? SpringArm->TargetArmLength : CharacterControllerTuning::DefaultThirdPersonArmLength;
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

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [this](const float DeltaSeconds)
            {
                TickFromGameUpdate(DeltaSeconds);
            },
            0);
    }
}

void ACharacterController::Load(const FString &Path)
{
    bIsLoaded = false;
    LoadProgress = 0.0f;

    PrepareForMeshReload();

    if (IsValid(Component.Get()))
    {
        // A new async mesh invalidates the previous waterline reference. The new
        // BONE_HEAD/capsule offset will be committed only after OnLoadCompleted.
        Component->InvalidateWaterReferenceForPendingMeshLoad();
    }

    if (IsValid(ActiveLoadAction.Get()))
    {
        ActiveLoadAction->CancelAndRelease();
        ActiveLoadAction = nullptr;
    }

    // 1. Create a fresh glTFRuntime asset load action and pass WorldContextObject, Owner, and Path.
    UCharacterLoadAsyncAction *LoadAction = UCharacterLoadAsyncAction::LoadCharacterAsync(this, this, Path);
    if (LoadAction)
    {
        ActiveLoadAction = LoadAction;
        // 2. Connect completed delegate (Optional)
        // Optional hook for UI/state work after loading completes.
        LoadAction->OnCompleted.AddDynamic(this, &ACharacterController::OnLoadCompleted);
        LoadAction->OnProgress.AddDynamic(this, &ACharacterController::OnLoadProgress);
        // 3. Start async loading
        // Activate() runs glTF loading, BoneMap loading, and mesh creation in order.
        LoadAction->Activate();
    }
    else
    {
        RestoreAfterMeshReload();
        LoadProgress = 1.0f;
    }
}

void ACharacterController::OnLoadProgress(float Progress)
{
    LoadProgress = FMath::Max(LoadProgress, FMath::Clamp(Progress, 0.0f, 1.0f));
}

void ACharacterController::OnLoadCompleted(bool Result)
{
    ActiveLoadAction = nullptr;

    if (!Result)
    {
        USkeletalMeshComponent *MeshComp = GetMesh();
        if (IsValid(MeshComp) && IsValid(DefaultAsset.SkeletalMesh))
        {
            MeshComp->SetSkinnedAssetAndUpdate(DefaultAsset.SkeletalMesh, true);
        }
        UE_LOG(LogTemp, Warning, TEXT("Character glTF load failed. Falling back to the default mesh so world/runtime loading can continue."));
    }

    RestoreAfterMeshReload();

    if (IsValid(Component.Get()))
    {
        // Waterline reference sampling is intentionally delayed until the final
        // loaded mesh has produced valid bone transforms. Do not cache BONE_HEAD
        // or a capsule fallback from CharacterComponent::BeginPlay.
        Component->RequestWaterReferenceRefreshAfterMeshLoad();
    }

    // bIsLoaded is used by glTFStreamSubSystem as a player load-completion gate. Treat the
    // default-mesh fallback as a completed load; otherwise main-world startup can never finish.
    LoadProgress = 1.0f;
    bIsLoaded = true;
}

void ACharacterController::PrepareForMeshReload()
{
    USkeletalMeshComponent* MeshComp = GetMesh();
    if (!IsValid(MeshComp))
    {
        return;
    }

    // Dynamically loaded player meshes are generated from glTFRuntime and can temporarily have a
    // different USkeleton/bone layout while the async load is still in progress. Disable
    // the AnimBP/ControlRig graph before the swap so worker-thread CacheBones cannot build
    // mappings against a half-replaced runtime mesh.
    if (!bHasSavedAnimationState)
    {
        SavedAnimationMode = MeshComp->GetAnimationMode();
        SavedAnimClass = MeshComp->GetAnimClass();
        bHasSavedAnimationState = true;
    }

    MeshComp->bPauseAnims = true;
    MeshComp->SetComponentTickEnabled(false);
    MeshComp->SetAllBodiesSimulatePhysics(false);
    MeshComp->SetSimulatePhysics(false);
    MeshComp->PutAllRigidBodiesToSleep();
    MeshComp->SetAnimationMode(EAnimationMode::AnimationSingleNode);
}

void ACharacterController::RestoreAfterMeshReload()
{
    USkeletalMeshComponent* MeshComp = GetMesh();
    if (!IsValid(MeshComp))
    {
        return;
    }

    if (bHasSavedAnimationState)
    {
        if (SavedAnimationMode == EAnimationMode::AnimationBlueprint && SavedAnimClass)
        {
            MeshComp->SetAnimInstanceClass(SavedAnimClass);
        }
        else
        {
            MeshComp->SetAnimationMode(SavedAnimationMode.GetValue());
        }
        bHasSavedAnimationState = false;
    }

    MeshComp->SetComponentTickEnabled(true);
    MeshComp->bPauseAnims = false;
    MeshComp->RecreatePhysicsState();
}

void ACharacterController::PrepareForPawnReplacement()
{
    if (IsValid(ActiveLoadAction.Get()))
    {
        ActiveLoadAction->CancelAndRelease();
        ActiveLoadAction = nullptr;
    }

    Activate(false);
    PrepareForMeshReload();

    USkeletalMeshComponent* MeshComp = GetMesh();
    if (IsValid(MeshComp))
    {
        // Do not restore the animation graph on a pawn that is about to be destroyed.
        // This keeps ControlRig/PoseDriver from evaluating while the PlayerController is
        // being moved to the freshly spawned runtime character.
        MeshComp->bPauseAnims = true;
        MeshComp->SetComponentTickEnabled(false);
        MeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        MeshComp->SetGenerateOverlapEvents(false);
    }

    if (UCapsuleComponent* Capsule = GetCapsuleComponent())
    {
        Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Capsule->SetGenerateOverlapEvents(false);
    }
}



void ACharacterController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
    }
    GameUpdateTickHandle = INDEX_NONE;

    Super::EndPlay(EndPlayReason);
}

void ACharacterController::RestoreControlAfterRagdollRecovery()
{
    // Ragdoll recovery disables movement for several frames while the mesh is being reattached.
    // Always restore the authoritative movement/input/collision state in one place so a missed
    // animation frame, water-state transition, or repeated deactivate request cannot leave the pawn unresponsive.
    bIsMoveable = true;
    SetActorHiddenInGame(false);
    SetActorEnableCollision(true);

    if (UCapsuleComponent* Capsule = GetCapsuleComponent())
    {
        Capsule->SetActive(true);
        Capsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        Capsule->SetGenerateOverlapEvents(true);
    }

    if (USkeletalMeshComponent* MeshComp = GetMesh())
    {
        UCharacterFunctionLibrary::DisableRagdollPhysicsButKeepSecondary(*MeshComp);
        MeshComp->SetSimulatePhysics(false);
        MeshComp->PutAllRigidBodiesToSleep();
        MeshComp->SetComponentTickEnabled(true);
        MeshComp->bPauseAnims = false;
        MeshComp->SetVisibility(true, true);

        if (UCapsuleComponent* Capsule = GetCapsuleComponent())
        {
            if (MeshComp->GetAttachParent() != Capsule)
            {
                MeshComp->AttachToComponent(Capsule, FAttachmentTransformRules::KeepWorldTransform);
            }
            MeshComp->SetRelativeLocation(CharacterControllerTuning::MeshDefaultRelativeLocation);
            MeshComp->SetRelativeRotation(CharacterControllerTuning::MeshDefaultRelativeRotation);
        }
    }

    if (IsValid(Movement))
    {
        Movement->SetActive(true);
        Movement->Activate(true);
        Movement->ConsumeInputVector();
        Movement->StopMovementImmediately();
        Movement->bOrientRotationToMovement = false;

        if (Movement->MovementMode == MOVE_None)
        {
            const bool bHasWalkableSupport = Movement->IsMovingOnGround()
                || (IsValid(Component.Get()) && Component->IsCharacterSupportedByWalkableGround());
            Movement->SetMovementMode(bHasWalkableSupport ? MOVE_Walking : MOVE_Falling);
        }
    }

    CharacterStateBit &= ~(STATE_JUMPING | STATE_SPRINT | STATE_CROUCH);
    RawMoveInput = FVector::ZeroVector;
    StopJumping();
    UnCrouch();

    if (APlayerController* PlayerController = Cast<APlayerController>(GetController()))
    {
        PlayerController->SetIgnoreMoveInput(false);
        PlayerController->SetIgnoreLookInput(false);
        PlayerController->SetViewTarget(this);

        if (APlayerCharacterController* PlayerCharacterController = Cast<APlayerCharacterController>(PlayerController))
        {
            PlayerCharacterController->ApplyGameInputMode();
            PlayerCharacterController->ClearLatchedMovementInput();
        }
    }
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
    if (LastPhysicsObjectImpactTime >= 0.0 && Now - LastPhysicsObjectImpactTime < CharacterControllerTuning::PhysicsObjectImpactCooldownSeconds)
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

    if (ImpactSpeed < CharacterControllerTuning::MinPhysicsObjectImpactSpeed)
    {
        return;
    }

    ImpactSpeed = FMath::Clamp(ImpactSpeed * CharacterControllerTuning::PhysicsObjectImpactVelocityScale, 0.0f, CharacterControllerTuning::MaxPhysicsObjectImpactVelocityChange);

    FVector VelocityDelta = HorizontalDirection * ImpactSpeed;
    const float UpwardVelocity = FMath::Clamp(ImpactSpeed * CharacterControllerTuning::PhysicsObjectImpactUpwardRatio, 0.0f, CharacterControllerTuning::MaxPhysicsObjectImpactUpwardVelocity);
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
    TickFromGameUpdate(DeltaSeconds);
}

void ACharacterController::TickFromGameUpdate(float DeltaSeconds)
{
    if (!IsValid(Component.Get()))
    {
        return;
    }
    if (!IsValid(SubSystem))
    {
        SubSystem = UGameManagerSubSystem::GetSubSystem(this);
    }
    if (!IsValid(SubSystem))
    {
        return;
    }

    if (GetVelocity().Z <= 0.0f)
    {
        CharacterStateBit &= ~STATE_JUMPING;
    }
    SyncRagdollWaterStateFromPhysics();
    Component->UpdateComponent(DeltaSeconds, RawMoveInput, CharacterStateBit, WaterLevel);

    const bool bRagdollTransitionActive = Component->IsRagdollTransitionInProgress();

    // Buoyancy is only allowed to touch simulated ragdoll bodies. When no ragdoll
    // transition is active, keep the visual mesh attached/visible so stale physics
    // state cannot make it vanish or drift away. Do not run this during get-up:
    // the component is intentionally blending from the ragdoll world transform.
    if (!bRagdollTransitionActive)
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
                if (!bIsCrouched)
                {
                    MeshComp->SetRelativeLocation(CharacterControllerTuning::MeshDefaultRelativeLocation);
                    MeshComp->SetRelativeRotation(CharacterControllerTuning::MeshDefaultRelativeRotation);
                }
            }
        }
    }

    if (!bRagdollTransitionActive && Component->IsRagdollDamage())
    {
        float DirectWaterLevel = WaterLevel;
        if (!FindDirectWaterLevel(DirectWaterLevel))
        {
            // Fall-damage ragdoll is starting from dry air/ground. Drop stale water state
            // before ActiveRagdoll() snapshots bRagdollInWater.
            ClearDryWaterState(DirectWaterLevel, false);
        }
        else
        {
            WaterLevel = DirectWaterLevel;
        }

        Component->SetRagdollActive(true);
    }

    SubSystem->SetPlayerLocation(GetActorLocation());
}

void ACharacterController::EnterWater(const float Level)
{
    // Component overlap can stay true while only the capsule radius touches the water box.
    // For normal character swimming, the capsule center column must be inside the water
    // BoxComponent.  Otherwise moving out through the side of the box can leave stale
    // overlap/state bits that keep MOVE_Swimming alive.
    float DirectWaterLevel = Level;
    if (!FindDirectWaterLevel(DirectWaterLevel))
    {
        bWaterStateFromOverlap = false;
        if (!bWaterStateForcedByRagdoll)
        {
            ClearDryWaterState(Level, true);
        }
        return;
    }

    bWaterStateFromOverlap = true;
    WaterLevel = DirectWaterLevel;

    if (IsValid(Movement) && Movement->IsFlying())
    {
        // Flying is an explicit player override.  Keep the latest water level for
        // Fly-off rechecks, but do not re-enter STATE_WATER while the mode is Flying.
        CharacterStateBit &= ~STATE_WATER;
        bWaterStateForcedByRagdoll = false;
        if (IsValid(Component.Get()))
        {
            Component->ClearSwimmingSurfaceConstraintState();
        }
        return;
    }

    const bool bCurrentlySwimming = IsValid(Movement) && Movement->MovementMode == MOVE_Swimming;
    if (IsValid(Component.Get()) && !Component->ShouldUseDirectWaterState(DirectWaterLevel, bCurrentlySwimming))
    {
        // Touching only the top of the water volume is not enough to enter Swimming.
        // Keep the overlap memory, because the next direct probe can still enable water
        // once the capsule is actually immersed past the enter threshold.
        CharacterStateBit &= ~STATE_WATER;
        return;
    }

    SetWaterState(true, DirectWaterLevel);
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

bool ACharacterController::FindDirectWaterLevel(float& OutLevel) const
{
    // This is the authoritative non-ragdoll water check.  Do not trust STATE_WATER here,
    // because Fly -> Fall toggles can leave that bit alive after the overlap has already ended.
    float DetectedLevel = OutLevel;
    const FVector ActorLocation = GetActorLocation();
    const float CapsuleHalfHeight = GetCapsuleComponent() ? GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 0.0f;
    const FVector BottomLocation(ActorLocation.X, ActorLocation.Y, ActorLocation.Z - CapsuleHalfHeight);

    const bool bActorPointInWater = AWaterActor::FindWaterLevelAtLocationStrict(this, ActorLocation, DetectedLevel);
    const bool bBottomPointInWater = AWaterActor::FindWaterLevelAtLocationStrict(this, BottomLocation, DetectedLevel);
    if (bActorPointInWater || bBottomPointInWater)
    {
        OutLevel = DetectedLevel;
        const bool bCurrentlySwimming = IsValid(Movement)
            && Movement->MovementMode == MOVE_Swimming
            && !Movement->IsFlying();
        return !IsValid(Component.Get()) || Component->ShouldUseDirectWaterState(DetectedLevel, bCurrentlySwimming);
    }

    return false;
}

void ACharacterController::ClearDryWaterState(float Level, bool bUpdateMovementMode)
{
    // A dry movement probe wins over stale overlap/state bits.  This is especially important
    // before fall-damage ragdoll starts, because ActiveRagdoll() can otherwise preserve
    // a previous swimming intent and classify a dry ground impact as water recovery.
    bWaterStateFromOverlap = false;
    bWaterStateForcedByRagdoll = false;
    CharacterStateBit &= ~STATE_WATER;
    WaterLevel = Level;

    if (IsValid(Component.Get()))
    {
        Component->SetRagdollWaterState(false, true);
    }

    if (IsValid(SkeletalMeshBuoyancyComponent))
    {
        SkeletalMeshBuoyancyComponent->ExitWater(Level);
    }

    if (bUpdateMovementMode && IsValid(Movement) && Movement->MovementMode == MOVE_Swimming)
    {
        // Drying out at the surface should not zero horizontal momentum; otherwise Fly-off
        // or surface exit feels like the character gets stuck in slow swimming for a frame.
        const FVector PreservedVelocity = Movement->Velocity;
        const bool bHasWalkableSupport = Movement->IsMovingOnGround()
            || (IsValid(Component.Get()) && Component->IsCharacterSupportedByWalkableGround());
        Movement->SetMovementMode(bHasWalkableSupport ? MOVE_Walking : MOVE_Falling);
        Movement->Velocity = FVector(PreservedVelocity.X, PreservedVelocity.Y, FMath::Min(PreservedVelocity.Z, 0.0f));
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
    if (IsValid(Component.Get()))
    {
        Component->ClearSwimmingSurfaceConstraintState();
    }

    float DirectWaterLevel = WaterLevel;
    const bool bDirectlyInWater = FindDirectWaterLevel(DirectWaterLevel);

    if (bWasFlying)
    {
        CharacterStateBit &= ~STATE_FLYING;

        if (bDirectlyInWater)
        {
            WaterLevel = DirectWaterLevel;
            CharacterStateBit |= STATE_WATER;
            bWaterStateFromOverlap = true;
            bWaterStateForcedByRagdoll = false;
            if (IsValid(SkeletalMeshBuoyancyComponent))
            {
                SkeletalMeshBuoyancyComponent->EnterWater(WaterLevel);
            }
            Movement->SetMovementMode(MOVE_Swimming);
        }
        else
        {
            // Leaving Fly over dry ground/air must not reuse an old STATE_WATER bit.
            // If a raised self-ignored ground probe already sees support, land directly
            // in Walking instead of spending a frame in slow water/fall control.
            ClearDryWaterState(DirectWaterLevel, false);
            const bool bHasWalkableSupport = Movement->IsMovingOnGround()
                || (IsValid(Component.Get()) && Component->IsCharacterSupportedByWalkableGround());
            Movement->SetMovementMode(bHasWalkableSupport ? MOVE_Walking : MOVE_Falling);
        }
    }
    else
    {
        // Starting Fly always leaves swim control immediately.  The direct water
        // probe is kept only to refresh WaterLevel; Fly-off performs a fresh check
        // and will choose Swimming again only when the character is actually deep enough.
        WaterLevel = DirectWaterLevel;

        CharacterStateBit &= ~STATE_WATER;
        bWaterStateFromOverlap = false;
        bWaterStateForcedByRagdoll = false;
        if (IsValid(SkeletalMeshBuoyancyComponent))
        {
            SkeletalMeshBuoyancyComponent->ExitWater(WaterLevel);
        }

        Movement->SetMovementMode(MOVE_Flying);
        CharacterStateBit |= STATE_FLYING;
    }
}

void ACharacterController::ToggleRagdoll()
{
    const bool bNewState = !Component->IsRagdollActive();
    if (bNewState)
    {
        float DirectWaterLevel = WaterLevel;
        if (!FindDirectWaterLevel(DirectWaterLevel))
        {
            // Manual ragdoll uses the same dry guard as fall-damage ragdoll.
            ClearDryWaterState(DirectWaterLevel, false);
        }
        else
        {
            WaterLevel = DirectWaterLevel;
        }
    }

    Component->SetRagdollActive(bNewState);
}

void ACharacterController::SetWaterState(bool bValue, float Level, bool bForceRagdollWaterState)
{
    const bool bWasInWater = UCharacterFunctionLibrary::IsStateActive(CharacterStateBit, STATE_WATER);
    const bool bStateChanged = bWasInWater != bValue;
    const bool bLevelChanged = !FMath::IsNearlyEqual(WaterLevel, Level, CharacterControllerTuning::WaterLevelChangeToleranceCm);
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
    const bool bRagdollAcceptsWater = !bRagdollTransitionActive
        || (IsValid(Component.Get())
            && !Component->ShouldTreatRagdollWaterAsGround()
            && (Component->ShouldRecoverRagdollInWaterFromEnvironment() || Component->IsRecoveringRagdollInWater() || Component->ShouldKeepSwimmingAfterWaterRagdoll()));

    if (IsValid(Component.Get()) && (bStateChanged || bLevelChanged || bForceRagdollWaterState || bRagdollTransitionActive))
    {
        // During ragdoll, an overlap bit alone is not enough to enter water state.  The filtered
        // ragdoll environment snapshot must agree first, otherwise SetRagdollWaterState(true)
        // would immediately force MOVE_Swimming from a stale Fly/Fall water flag.
        const bool bComponentInWater = bValue && bRagdollAcceptsWater;
        Component->SetRagdollWaterState(bComponentInWater, bForceRagdollWaterState || (bValue && bRagdollTransitionActive && !bRagdollAcceptsWater));
    }

    if (bValue && IsValid(Movement) && bRagdollTransitionActive && bRagdollAcceptsWater)
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
        bActorPointInWater = FindDirectWaterLevel(EffectiveLevel);
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
        const bool bKeepPostRagdollSwimming = Component->ShouldKeepSwimmingAfterWaterRagdoll();
        const bool bHasAnyWaterState = bWaterStateFromOverlap
            || bWaterStateForcedByRagdoll
            || UCharacterFunctionLibrary::IsStateActive(CharacterStateBit, STATE_WATER)
            || (IsValid(Movement) && Movement->MovementMode == MOVE_Swimming);

        if (!bKeepPostRagdollSwimming && (!IsValid(Movement) || !Movement->IsFlying()))
        {
            float DirectWaterLevel = WaterLevel;
            const bool bDirectWaterIsDeepEnough = FindDirectWaterLevel(DirectWaterLevel);
            if (bDirectWaterIsDeepEnough)
            {
                WaterLevel = DirectWaterLevel;
                bWaterStateFromOverlap = true;
                if (!UCharacterFunctionLibrary::IsStateActive(CharacterStateBit, STATE_WATER))
                {
                    SetWaterState(true, WaterLevel, true);
                }
            }
            else if (bHasAnyWaterState)
            {
                // A shallow surface touch, including Fly-off with only the capsule bottom in water,
                // is treated as dry movement.  Hysteresis inside FindDirectWaterLevel keeps real
                // swimmers from flickering out at the surface.
                ClearDryWaterState(DirectWaterLevel, true);
                return;
            }
        }

        if (bKeepPostRagdollSwimming)
        {
            float LockWaterLevel = WaterLevel;
            const bool bLockStillInWater = FindDirectWaterLevel(LockWaterLevel);

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
            const bool bActorStillInWater = FindDirectWaterLevel(ActorWaterLevel);

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
        if (!bWasForcedByRagdoll || !UCharacterFunctionLibrary::IsStateActive(CharacterStateBit, STATE_WATER) || !FMath::IsNearlyEqual(WaterLevel, DetectedWaterLevel, CharacterControllerTuning::WaterLevelChangeToleranceCm))
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