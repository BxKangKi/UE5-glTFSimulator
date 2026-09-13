// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Velocity-based streaming barrier; no synthetic collision geometry is created.

/**
 * @file StreamingMovementGateSubsystem.h
 * 역할: 아직 로드되지 않은 공간으로의 이동을 제한합니다.
 * 핵심 기능: 청크·메시 준비 검사, 플레이어·물리 객체 이동 보호.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "StreamingMovementGateSubsystem.generated.h"

class UPrimitiveComponent;

/**
 * Stops a player or physics object before it enters unavailable chunk/mesh data. Each machine
 * evaluates its own readiness: the authority controls authoritative motion, while a slow client
 * may pause only its local presentation. Original linear and angular velocities are restored once
 * both the local chunk and every intersecting model region are ready.
 */
UCLASS()
class GLTFSIMULATOR_API UStreamingMovementGateSubsystem final : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool IsTickable() const override { return !HasAnyFlags(RF_ClassDefaultObject); }

    void RegisterMovable(AActor* Actor);
    void UnregisterMovable(AActor* Actor);
    void SetModelRegionAvailable(const UObject* Owner, FName RegionName, const FBox& WorldBounds, bool bAvailable);
    void ClearModelRegions(const UObject* Owner);

private:
    struct FRegionKey
    {
        TWeakObjectPtr<UObject> Owner;
        FName Name;
        bool operator==(const FRegionKey& Other) const { return Owner == Other.Owner && Name == Other.Name; }
        friend uint32 GetTypeHash(const FRegionKey& Key)
        {
            return HashCombine(GetTypeHash(Key.Owner), GetTypeHash(Key.Name));
        }
    };

    struct FFrozenState
    {
        FVector LinearVelocity = FVector::ZeroVector;
        FVector AngularVelocity = FVector::ZeroVector;
        TWeakObjectPtr<UPrimitiveComponent> Primitive;
        uint8 MovementMode = 0;
        bool bWasSimulatingPhysics = false;
        bool bCharacterMovement = false;
    };

    TSet<TWeakObjectPtr<AActor>> RegisteredMovables;
    TMap<FRegionKey, FBox> UnavailableRegions;
    TMap<TWeakObjectPtr<AActor>, FFrozenState> FrozenActors;

    bool IsDestinationAvailable(const FVector& Destination) const;
    void Freeze(AActor* Actor);
    void Resume(AActor* Actor, const FFrozenState& State);
};
