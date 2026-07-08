// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"
#include "VehicleSubSystem.generated.h"

class AVehiclePawn;
class UGameUpdateSubSystem;

/**
 * World-level vehicle update coordinator.
 *
 * Vehicle pawns register here instead of owning individual Actor ticks. The subsystem gathers
 * per-vehicle state on the game thread, runs input/control math in parallel, and then applies
 * the resulting force integration back on the game thread in one deterministic update phase.
 */
UCLASS()
class GLTFSIMULATOR_API UVehicleSubSystem : public UWorldSubsystem, public FTickableGameObject
{
    GENERATED_BODY()

public:
    UVehicleSubSystem();

    static UVehicleSubSystem* Get(const UObject* WorldContextObject);

    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    void RegisterVehicle(AVehiclePawn* VehiclePawn);
    void UnregisterVehicle(AVehiclePawn* VehiclePawn);
    void TickVehicles(float DeltaSeconds);

    virtual void Tick(float DeltaTime) override;
    virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
    virtual bool IsTickable() const override { return !IsTemplate() && GameUpdateHandle == INDEX_NONE && Vehicles.Num() > 0; }
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UVehicleSubSystem, STATGROUP_Tickables); }
    virtual UWorld* GetTickableGameObjectWorld() const override { return GetWorld(); }

private:
    TArray<TWeakObjectPtr<AVehiclePawn>> Vehicles;
    int32 GameUpdateHandle = INDEX_NONE;

    void CompactVehicles();
};
