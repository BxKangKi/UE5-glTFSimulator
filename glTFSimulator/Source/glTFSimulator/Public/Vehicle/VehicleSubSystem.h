// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
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
class GLTFSIMULATOR_API UVehicleSubSystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    UVehicleSubSystem();

    static UVehicleSubSystem* Get(const UObject* WorldContextObject);

    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    void RegisterVehicle(AVehiclePawn* VehiclePawn);
    void UnregisterVehicle(AVehiclePawn* VehiclePawn);
    void UpdateVehiclesFromGameUpdate(float DeltaSeconds);

private:
    TArray<TWeakObjectPtr<AVehiclePawn>> Vehicles;
    int32 GameUpdateHandle = INDEX_NONE;

    void CompactVehicles();
};
