// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/PhysicsTransformInterpolationSubSystem.h"
#include "System/GameUpdateSubSystem.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Subsystems/SubsystemCollection.h"

UPhysicsTransformInterpolationSubSystem* UPhysicsTransformInterpolationSubSystem::Get(const UObject* WorldContextObject)
{
    if (!IsValid(WorldContextObject))
    {
        return nullptr;
    }

    if (const UGameInstance* GameInstance = Cast<UGameInstance>(WorldContextObject))
    {
        return const_cast<UGameInstance*>(GameInstance)->GetSubsystem<UPhysicsTransformInterpolationSubSystem>();
    }

    if (const UGameInstanceSubsystem* GameInstanceSubsystem = Cast<UGameInstanceSubsystem>(WorldContextObject))
    {
        if (UGameInstance* GameInstance = GameInstanceSubsystem->GetGameInstance())
        {
            return GameInstance->GetSubsystem<UPhysicsTransformInterpolationSubSystem>();
        }
    }

    UWorld* World = WorldContextObject->GetWorld();
    if (!World)
    {
        return nullptr;
    }

    UGameInstance* GameInstance = World->GetGameInstance();
    return GameInstance ? GameInstance->GetSubsystem<UPhysicsTransformInterpolationSubSystem>() : nullptr;
}

void UPhysicsTransformInterpolationSubSystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    RegisterGameUpdate();
}

void UPhysicsTransformInterpolationSubSystem::Deinitialize()
{
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateHandle);
    }

    GameUpdateHandle = INDEX_NONE;
    Entries.Empty();
    Super::Deinitialize();
}

void UPhysicsTransformInterpolationSubSystem::RegisterGameUpdate()
{
    if (GameUpdateHandle != INDEX_NONE)
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdateHandle = GameUpdate->RegisterUpdate(
            this,
            [this](const float DeltaSeconds)
            {
                UpdateFromGameUpdate(DeltaSeconds);
            },
            -50);
    }
}

void UPhysicsTransformInterpolationSubSystem::SubmitPhysicsTransformForComponent(
    const UObject* WorldContextObject,
    USceneComponent* Component,
    const FTransform& TargetTransform,
    float InterpSpeed,
    float TeleportDistance,
    bool bApplyScale,
    bool bCanMoveSimulatingPrimitive)
{
    if (UPhysicsTransformInterpolationSubSystem* Subsystem = Get(WorldContextObject))
    {
        Subsystem->SubmitTransform(Component, TargetTransform, InterpSpeed, TeleportDistance, bApplyScale, bCanMoveSimulatingPrimitive);
    }
}

void UPhysicsTransformInterpolationSubSystem::SubmitTransform(
    USceneComponent* Component,
    const FTransform& TargetTransform,
    float InterpSpeed,
    float TeleportDistance,
    bool bApplyScale,
    bool bCanMoveSimulatingPrimitive)
{
    if (!IsValid(Component) || TargetTransform.ContainsNaN())
    {
        return;
    }

    RegisterGameUpdate();

    for (FInterpolatedTransformEntry& Entry : Entries)
    {
        if (Entry.Component.Get() == Component)
        {
            const float SafeInterpSpeed = FMath::Max(0.0f, InterpSpeed);
            const float SafeTeleportDistance = FMath::Max(0.0f, TeleportDistance);
            const bool bSettingsChanged =
                !FMath::IsNearlyEqual(Entry.InterpSpeed, SafeInterpSpeed) ||
                !FMath::IsNearlyEqual(Entry.TeleportDistance, SafeTeleportDistance) ||
                Entry.bApplyScale != bApplyScale ||
                Entry.bCanMoveSimulatingPrimitive != bCanMoveSimulatingPrimitive;
            const bool bTargetChanged = !Entry.TargetTransform.Equals(TargetTransform, 0.001f);

            Entry.TargetTransform = TargetTransform;
            Entry.InterpSpeed = SafeInterpSpeed;
            Entry.TeleportDistance = SafeTeleportDistance;
            Entry.bApplyScale = bApplyScale;
            Entry.bCanMoveSimulatingPrimitive = bCanMoveSimulatingPrimitive;
            Entry.bAtTarget = Entry.bAtTarget && !bTargetChanged && !bSettingsChanged;
            return;
        }
    }

    FInterpolatedTransformEntry NewEntry;
    NewEntry.Component = Component;
    NewEntry.TargetTransform = TargetTransform;
    NewEntry.InterpSpeed = FMath::Max(0.0f, InterpSpeed);
    NewEntry.TeleportDistance = FMath::Max(0.0f, TeleportDistance);
    NewEntry.bApplyScale = bApplyScale;
    NewEntry.bCanMoveSimulatingPrimitive = bCanMoveSimulatingPrimitive;
    Entries.Add(MoveTemp(NewEntry));
}

void UPhysicsTransformInterpolationSubSystem::ClearComponent(USceneComponent* Component)
{
    if (!Component)
    {
        return;
    }

    Entries.RemoveAll([Component](const FInterpolatedTransformEntry& Entry)
    {
        return !Entry.Component.IsValid() || Entry.Component.Get() == Component;
    });
}

void UPhysicsTransformInterpolationSubSystem::ClearOwner(const UObject* Owner)
{
    if (!Owner)
    {
        return;
    }

    Entries.RemoveAll([Owner](const FInterpolatedTransformEntry& Entry)
    {
        const USceneComponent* Component = Entry.Component.Get();
        return !Component || Component->GetOwner() == Owner;
    });
}

bool UPhysicsTransformInterpolationSubSystem::ShouldSkipComponent(const USceneComponent* Component, bool bCanMoveSimulatingPrimitive) const
{
    if (!IsValid(Component))
    {
        return true;
    }

    const USkeletalMeshComponent* SkeletalMesh = Cast<USkeletalMeshComponent>(Component);
    if (SkeletalMesh && SkeletalMesh->IsSimulatingPhysics())
    {
        return true;
    }

    const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
    if (Primitive && Primitive->IsSimulatingPhysics() && !bCanMoveSimulatingPrimitive)
    {
        return true;
    }

    return false;
}

void UPhysicsTransformInterpolationSubSystem::UpdateFromGameUpdate(float DeltaTime)
{
    if (DeltaTime <= 0.0f)
    {
        return;
    }

    for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
    {
        FInterpolatedTransformEntry& Entry = Entries[Index];
        USceneComponent* Component = Entry.Component.Get();
        if (!IsValid(Component))
        {
            Entries.RemoveAtSwap(Index);
            continue;
        }

        if (ShouldSkipComponent(Component, Entry.bCanMoveSimulatingPrimitive))
        {
            continue;
        }

        // Stable targets stay registered for future submissions but do not dirty scene transforms.
        if (Entry.bAtTarget)
        {
            continue;
        }

        const FTransform CurrentTransform = Component->GetComponentTransform();
        const FVector CurrentLocation = CurrentTransform.GetLocation();
        const FVector TargetLocation = Entry.TargetTransform.GetLocation();
        const FQuat CurrentRotation = CurrentTransform.GetRotation().GetNormalized();
        const FQuat TargetRotation = Entry.TargetTransform.GetRotation().GetNormalized();
        const bool bLocationAtTarget = CurrentLocation.Equals(TargetLocation, 0.01f);
        const bool bRotationAtTarget = CurrentRotation.Equals(TargetRotation, 0.0001f);
        const bool bScaleAtTarget =
            !Entry.bApplyScale ||
            CurrentTransform.GetScale3D().Equals(Entry.TargetTransform.GetScale3D(), 0.001f);
        if (bLocationAtTarget && bRotationAtTarget && bScaleAtTarget)
        {
            Entry.bAtTarget = true;
            continue;
        }

        const float DistanceSquared = FVector::DistSquared(CurrentLocation, TargetLocation);
        const bool bTeleport = Entry.TeleportDistance > 0.0f && DistanceSquared > FMath::Square(Entry.TeleportDistance);

        FVector NewLocation = TargetLocation;
        FQuat NewRotation = TargetRotation;

        if (!bTeleport && Entry.InterpSpeed > KINDA_SMALL_NUMBER)
        {
            const float Alpha = FMath::Clamp(1.0f - FMath::Exp(-Entry.InterpSpeed * DeltaTime), 0.0f, 1.0f);
            NewLocation = FMath::Lerp(CurrentLocation, TargetLocation, Alpha);
            NewRotation = FQuat::Slerp(CurrentRotation, NewRotation, Alpha).GetNormalized();
        }

        const bool bReachedLocation = NewLocation.Equals(TargetLocation, 0.01f);
        const bool bReachedRotation = NewRotation.Equals(TargetRotation, 0.0001f);
        if (bReachedLocation && bReachedRotation)
        {
            // Snap once at convergence so later frames can skip the entry without residual drift.
            NewLocation = TargetLocation;
            NewRotation = TargetRotation;
        }

        Component->SetWorldLocationAndRotation(NewLocation, NewRotation.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
        if (Entry.bApplyScale)
        {
            Component->SetWorldScale3D(Entry.TargetTransform.GetScale3D());
        }
        Entry.bAtTarget = bReachedLocation && bReachedRotation;
    }
}
