#include "RuntimeFramework/SimulatorInteractionAnimInstance.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"

void USimulatorInteractionAnimInstance::NativeInitializeAnimation()
{
    Super::NativeInitializeAnimation();
    OwningActorWeak = GetOwningActor();
    PreviousOwnerRotation = OwningActorWeak.IsValid() ? OwningActorWeak->GetActorRotation() : FRotator::ZeroRotator;
    ActiveIKConfig = CharacterConfig.FullBodyIK;
}

void USimulatorInteractionAnimInstance::SetCharacterInteractionConfig(const FSimulatorCharacterInteractionConfig& InConfig)
{
    CharacterConfig = InConfig;
    ResolvedPrimaryHand = CharacterConfig.DominantHand;
    if (!bHasEquipmentConfig)
    {
        ActiveIKConfig = CharacterConfig.FullBodyIK;
    }
}

void USimulatorInteractionAnimInstance::EquipInteractionActor(AActor* InEquipmentActor, const FSimulatorEquipmentInteractionConfig& InConfig)
{
    EquipmentActorWeak = InEquipmentActor;
    EquipmentConfig = InConfig;
    EquipmentConfig.Sanitize();
    bHasEquipmentConfig = IsValid(InEquipmentActor);
    ResolvedPrimaryHand = EquipmentConfig.ResolvePrimaryHand(CharacterConfig.DominantHand);
    ActiveIKConfig = bHasEquipmentConfig ? EquipmentConfig.FullBodyIK : CharacterConfig.FullBodyIK;
}

void USimulatorInteractionAnimInstance::ClearInteractionActor()
{
    EquipmentActorWeak.Reset();
    bHasEquipmentConfig = false;
    ResolvedPrimaryHand = CharacterConfig.DominantHand;
    ActiveIKConfig = CharacterConfig.FullBodyIK;
}

FTransform USimulatorInteractionAnimInstance::InterpTransform(const FTransform& Current, const FTransform& Target, const float DeltaSeconds, const float Speed)
{
    if (DeltaSeconds <= 0.0f || Speed <= 0.0f)
    {
        return Target;
    }
    const FVector Location = FMath::VInterpTo(Current.GetLocation(), Target.GetLocation(), DeltaSeconds, Speed);
    const FRotator Rotation = FMath::RInterpTo(Current.Rotator(), Target.Rotator(), DeltaSeconds, Speed);
    const FVector Scale = FMath::VInterpTo(Current.GetScale3D(), Target.GetScale3D(), DeltaSeconds, Speed);
    return FTransform(Rotation, Location, Scale);
}

USceneComponent* USimulatorInteractionAnimInstance::FindEquipmentAnchor(const FSimulatorGripPoint& Grip) const
{
    AActor* Equipment = EquipmentActorWeak.Get();
    if (!IsValid(Equipment)) return nullptr;
    if (!Grip.EquipmentAnchorTag.IsNone())
    {
        TInlineComponentArray<USceneComponent*> Components; Equipment->GetComponents(Components);
        for (USceneComponent* Component : Components)
        {
            if (IsValid(Component) && Component->ComponentHasTag(Grip.EquipmentAnchorTag)) return Component;
        }
    }
    return Equipment->GetRootComponent();
}

FSimulatorResolvedHandIK USimulatorInteractionAnimInstance::ResolveHand(const ESimulatorHand Hand, const float DeltaSeconds, const FSimulatorResolvedHandIK& Previous) const
{
    FSimulatorResolvedHandIK Result = Previous;
    const FSimulatorGripPoint* Grip = bHasEquipmentConfig ? EquipmentConfig.GetGrip(Hand) : nullptr;
    USkeletalMeshComponent* CharacterMesh = GetSkelMeshComponent();
    USceneComponent* Anchor = Grip ? FindEquipmentAnchor(*Grip) : nullptr;
    const bool bCanResolve = Grip && IsValid(CharacterMesh) && IsValid(Anchor) && ActiveIKConfig.bEnabled;
    const float TargetAlpha = bCanResolve ? Grip->IKAlpha : 0.0f;
    Result.Alpha = FMath::FInterpTo(Previous.Alpha, TargetAlpha, DeltaSeconds, ActiveIKConfig.AlphaInterpSpeed);
    if (!bCanResolve)
    {
        Result.bValid = Result.Alpha > KINDA_SMALL_NUMBER;
        if (!Result.bValid) Result.Fingers = FSimulatorFingerPose();
        return Result;
    }

    const FTransform TargetWorld = Grip->LocalTarget * Anchor->GetComponentTransform();
    const FTransform SmoothedWorld = Previous.bValid
        ? InterpTransform(Previous.TargetWorld, TargetWorld, DeltaSeconds, ActiveIKConfig.TransformInterpSpeed)
        : TargetWorld;
    Result.bValid = true;
    Result.TargetWorld = SmoothedWorld;
    Result.TargetComponentSpace = SmoothedWorld.GetRelativeTransform(CharacterMesh->GetComponentTransform());
    Result.Fingers = Grip->Fingers;
    return Result;
}

void USimulatorInteractionAnimInstance::UpdateBodyIK(const float DeltaSeconds)
{
    AActor* Owner = OwningActorWeak.Get();
    if (!IsValid(Owner))
    {
        ResolvedIK.BodyAlpha = FMath::FInterpTo(ResolvedIK.BodyAlpha, 0.0f, DeltaSeconds, ActiveIKConfig.AlphaInterpSpeed);
        return;
    }

    const FRotator CurrentRotation = Owner->GetActorRotation();
    const float YawDelta = FMath::FindDeltaAngleDegrees(PreviousOwnerRotation.Yaw, CurrentRotation.Yaw);
    const float YawRate = DeltaSeconds > SMALL_NUMBER ? YawDelta / DeltaSeconds : 0.0f;
    PreviousOwnerRotation = CurrentRotation;

    FVector Velocity = FVector::ZeroVector;
    if (const APawn* Pawn = Cast<APawn>(Owner)) Velocity = Pawn->GetVelocity();
    const FVector LocalVelocity = Owner->GetActorTransform().InverseTransformVectorNoScale(Velocity);
    const float NormalizedYawRate = FMath::Clamp(YawRate / FMath::Max(1.0f, ActiveIKConfig.MaxYawRateForFullLean), -1.0f, 1.0f);
    const float NormalizedForward = FMath::Clamp(LocalVelocity.X / 600.0f, -1.0f, 1.0f);
    const float NormalizedSide = FMath::Clamp(LocalVelocity.Y / 600.0f, -1.0f, 1.0f);

    const float TargetAlpha = ActiveIKConfig.bEnabled && (ResolvedIK.LeftHand.bValid || ResolvedIK.RightHand.bValid) ? 1.0f : 0.0f;
    ResolvedIK.BodyAlpha = FMath::FInterpTo(ResolvedIK.BodyAlpha, TargetAlpha, DeltaSeconds, ActiveIKConfig.AlphaInterpSpeed);
    const FRotator Base(
        FMath::Clamp(-NormalizedForward * ActiveIKConfig.MaxMoveLeanDegrees, -ActiveIKConfig.MaxSpinePitchDegrees, ActiveIKConfig.MaxSpinePitchDegrees),
        FMath::Clamp(NormalizedYawRate * ActiveIKConfig.MaxSpineYawDegrees, -ActiveIKConfig.MaxSpineYawDegrees, ActiveIKConfig.MaxSpineYawDegrees),
        FMath::Clamp((-NormalizedSide - NormalizedYawRate) * ActiveIKConfig.MaxTurnLeanDegrees, -ActiveIKConfig.MaxTurnLeanDegrees, ActiveIKConfig.MaxTurnLeanDegrees));

    ResolvedIK.PelvisRotation = FMath::RInterpTo(ResolvedIK.PelvisRotation, FRotator(Base.Pitch * ActiveIKConfig.PelvisWeight, Base.Yaw * ActiveIKConfig.PelvisWeight, Base.Roll * ActiveIKConfig.PelvisWeight), DeltaSeconds, ActiveIKConfig.TransformInterpSpeed);
    ResolvedIK.SpineRotation = FMath::RInterpTo(ResolvedIK.SpineRotation, FRotator(Base.Pitch * ActiveIKConfig.SpineWeight, Base.Yaw * ActiveIKConfig.SpineWeight, Base.Roll * ActiveIKConfig.SpineWeight), DeltaSeconds, ActiveIKConfig.TransformInterpSpeed);
    ResolvedIK.ShoulderRotation = FMath::RInterpTo(ResolvedIK.ShoulderRotation, FRotator(Base.Pitch * ActiveIKConfig.ShoulderWeight, Base.Yaw * ActiveIKConfig.ShoulderWeight, Base.Roll * ActiveIKConfig.ShoulderWeight), DeltaSeconds, ActiveIKConfig.TransformInterpSpeed);
}

void USimulatorInteractionAnimInstance::ResetResolvedState(const float DeltaSeconds)
{
    const auto Fade = [this, DeltaSeconds](FSimulatorResolvedHandIK& Hand)
    {
        Hand.Alpha = FMath::FInterpTo(Hand.Alpha, 0.0f, DeltaSeconds, ActiveIKConfig.AlphaInterpSpeed);
        Hand.bValid = Hand.Alpha > KINDA_SMALL_NUMBER;
        if (!Hand.bValid) Hand.Fingers = FSimulatorFingerPose();
    };
    Fade(ResolvedIK.LeftHand); Fade(ResolvedIK.RightHand);
}

void USimulatorInteractionAnimInstance::NativeUpdateAnimation(const float DeltaSeconds)
{
    Super::NativeUpdateAnimation(DeltaSeconds);
    if (!OwningActorWeak.IsValid()) OwningActorWeak = GetOwningActor();
    if (!bHasEquipmentConfig || !EquipmentActorWeak.IsValid())
    {
        if (bHasEquipmentConfig) ClearInteractionActor();
        ResetResolvedState(DeltaSeconds);
    }
    else
    {
        ResolvedIK.LeftHand = ResolveHand(ESimulatorHand::Left, DeltaSeconds, ResolvedIK.LeftHand);
        ResolvedIK.RightHand = ResolveHand(ESimulatorHand::Right, DeltaSeconds, ResolvedIK.RightHand);
    }
    if (ResolvedPrimaryHand == ESimulatorHand::Right)
    {
        ResolvedIK.PrimaryHand = ResolvedIK.RightHand; ResolvedIK.SecondaryHand = ResolvedIK.LeftHand;
    }
    else
    {
        ResolvedIK.PrimaryHand = ResolvedIK.LeftHand; ResolvedIK.SecondaryHand = ResolvedIK.RightHand;
    }
    UpdateBodyIK(DeltaSeconds);
}
