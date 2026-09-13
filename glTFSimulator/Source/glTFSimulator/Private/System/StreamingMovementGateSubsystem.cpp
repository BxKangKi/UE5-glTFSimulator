// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file StreamingMovementGateSubsystem.cpp
 * 역할: 아직 로드되지 않은 공간으로의 이동을 제한합니다.
 * 핵심 기능: 청크·메시 준비 검사, 플레이어·물리 객체 이동 보호.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "System/StreamingMovementGateSubsystem.h"

#include "Components/PrimitiveComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "System/WorldObjectStreamingSubsystem.h"

namespace
{
    UPrimitiveComponent* FindSimulatingPrimitive(AActor* Actor)
    {
        if (!IsValid(Actor)) return nullptr;
        TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
        for (UPrimitiveComponent* Primitive : Primitives)
            if (IsValid(Primitive) && Primitive->IsSimulatingPhysics()) return Primitive;
        return nullptr;
    }
}

void UStreamingMovementGateSubsystem::Deinitialize()
{
    check(IsInGameThread());
    for (const TPair<TWeakObjectPtr<AActor>, FFrozenState>& Pair : FrozenActors)
        if (AActor* Actor = Pair.Key.Get()) Resume(Actor, Pair.Value);
    FrozenActors.Empty();
    RegisteredMovables.Empty();
    UnavailableRegions.Empty();
    Super::Deinitialize();
}

TStatId UStreamingMovementGateSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UStreamingMovementGateSubsystem, STATGROUP_Tickables);
}

void UStreamingMovementGateSubsystem::RegisterMovable(AActor* Actor)
{
    check(IsInGameThread());
    if (IsValid(Actor)) RegisteredMovables.Add(Actor);
}

void UStreamingMovementGateSubsystem::UnregisterMovable(AActor* Actor)
{
    check(IsInGameThread());
    if (!Actor) return;
    if (FFrozenState State; FrozenActors.RemoveAndCopyValue(Actor, State)) Resume(Actor, State);
    RegisteredMovables.Remove(Actor);
}

void UStreamingMovementGateSubsystem::SetModelRegionAvailable(
    const UObject* Owner, const FName RegionName, const FBox& WorldBounds, const bool bAvailable)
{
    check(IsInGameThread());
    if (!IsValid(Owner) || RegionName.IsNone()) return;
    const FRegionKey Key{const_cast<UObject*>(Owner), RegionName};
    if (bAvailable || !WorldBounds.IsValid) UnavailableRegions.Remove(Key);
    else UnavailableRegions.Add(Key, WorldBounds);
}

void UStreamingMovementGateSubsystem::ClearModelRegions(const UObject* Owner)
{
    check(IsInGameThread());
    for (auto It = UnavailableRegions.CreateIterator(); It; ++It)
        if (!It.Key().Owner.IsValid() || It.Key().Owner.Get() == Owner) It.RemoveCurrent();
}

bool UStreamingMovementGateSubsystem::IsDestinationAvailable(const FVector& Destination) const
{
    UWorldObjectStreamingSubsystem* Chunks = GetWorld()
        ? GetWorld()->GetSubsystem<UWorldObjectStreamingSubsystem>() : nullptr;
    if (Chunks && Chunks->IsRunning() && !Chunks->IsLocationLoaded(Destination))
    {
        Chunks->EnsureLocationLoaded(Destination);
        return false;
    }
    for (const TPair<FRegionKey, FBox>& Pair : UnavailableRegions)
        if (Pair.Key.Owner.IsValid() && Pair.Value.IsInsideOrOn(Destination)) return false;
    return true;
}

void UStreamingMovementGateSubsystem::Freeze(AActor* Actor)
{
    if (!IsValid(Actor) || FrozenActors.Contains(Actor)) return;
    FFrozenState State;
    // Simulated bodies take priority, including a ragdoll mesh below an ACharacter root capsule.
    // Storing both velocity vectors before disabling simulation prevents solver drift at the gate.
    if (UPrimitiveComponent* Primitive = FindSimulatingPrimitive(Actor))
    {
        State.Primitive = Primitive;
        State.bWasSimulatingPhysics = true;
        State.LinearVelocity = Primitive->GetPhysicsLinearVelocity();
        State.AngularVelocity = Primitive->GetPhysicsAngularVelocityInRadians();
        Primitive->SetPhysicsLinearVelocity(FVector::ZeroVector);
        Primitive->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
        Primitive->SetSimulatePhysics(false);
    }
    else if (ACharacter* Character = Cast<ACharacter>(Actor))
    {
        if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
        {
            State.bCharacterMovement = true;
            State.MovementMode = static_cast<uint8>(Movement->MovementMode);
            State.LinearVelocity = Movement->Velocity;
            Movement->StopMovementImmediately();
            Movement->DisableMovement();
        }
    }
    if (!State.bCharacterMovement && !State.bWasSimulatingPhysics) return;
    FrozenActors.Add(Actor, State);
}

void UStreamingMovementGateSubsystem::Resume(AActor* Actor, const FFrozenState& State)
{
    if (!IsValid(Actor)) return;
    if (State.bCharacterMovement)
    {
        if (ACharacter* Character = Cast<ACharacter>(Actor))
            if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
            {
                Movement->SetMovementMode(static_cast<EMovementMode>(State.MovementMode));
                Movement->Velocity = State.LinearVelocity;
            }
    }
    else if (UPrimitiveComponent* Primitive = State.Primitive.Get(); IsValid(Primitive) && State.bWasSimulatingPhysics)
    {
        Primitive->SetSimulatePhysics(true);
        Primitive->SetPhysicsLinearVelocity(State.LinearVelocity);
        Primitive->SetPhysicsAngularVelocityInRadians(State.AngularVelocity);
    }
}

void UStreamingMovementGateSubsystem::Tick(const float DeltaTime)
{
    check(IsInGameThread());
    if (!GetWorld()) return;
    for (TActorIterator<APawn> It(GetWorld()); It; ++It) RegisteredMovables.Add(*It);

    for (auto It = RegisteredMovables.CreateIterator(); It; ++It)
    {
        AActor* Actor = It->Get();
        if (!IsValid(Actor) || Actor->IsActorBeingDestroyed())
        {
            FrozenActors.Remove(*It);
            It.RemoveCurrent();
            continue;
        }
        const FVector Velocity = FrozenActors.Contains(Actor)
            ? FrozenActors.FindChecked(Actor).LinearVelocity : Actor->GetVelocity();
        const FVector Destination = Actor->GetActorLocation() + Velocity * FMath::Clamp(DeltaTime, 0.0f, 0.25f);
        const bool bAvailable = IsDestinationAvailable(Destination);
        if (!bAvailable) Freeze(Actor);
        else if (FFrozenState State; FrozenActors.RemoveAndCopyValue(Actor, State)) Resume(Actor, State);
    }
}
