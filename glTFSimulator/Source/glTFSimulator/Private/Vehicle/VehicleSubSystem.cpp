// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Vehicle/VehicleSubSystem.h"

#include "Async/ParallelFor.h"
#include "Engine/World.h"
#include "Subsystems/SubsystemCollection.h"
#include "System/GameUpdateSubSystem.h"
#include "Vehicle/VehiclePawn.h"

namespace
{
    /** Vehicle registration and every UObject/physics read or write are game-thread-owned. */
    bool EnsureVehicleSubsystemGameThread(const TCHAR* Context)
    {
        return ensureMsgf(IsInGameThread(), TEXT("%s must run on the game thread"), Context);
    }
}

UVehicleSubSystem::UVehicleSubSystem()
{
}

UVehicleSubSystem* UVehicleSubSystem::Get(const UObject* WorldContextObject)
{
    if (!EnsureVehicleSubsystemGameThread(TEXT("UVehicleSubSystem::Get")))
    {
        return nullptr;
    }

    if (!IsValid(WorldContextObject))
    {
        return nullptr;
    }

    UWorld* World = WorldContextObject->GetWorld();
    if (!World)
    {
        return nullptr;
    }

    return World->GetSubsystem<UVehicleSubSystem>();
}

void UVehicleSubSystem::Initialize(FSubsystemCollectionBase& Collection)
{
    check(IsInGameThread());
    Super::Initialize(Collection);
    // Register lazily when the first vehicle appears. Empty worlds should not pay a callback cost.
}

void UVehicleSubSystem::Deinitialize()
{
    check(IsInGameThread());
    UnregisterGameUpdate();
    Vehicles.Empty();

    Super::Deinitialize();
}

void UVehicleSubSystem::RegisterVehicle(AVehiclePawn* VehiclePawn)
{
    if (!EnsureVehicleSubsystemGameThread(TEXT("UVehicleSubSystem::RegisterVehicle")))
    {
        return;
    }

    if (!IsValid(VehiclePawn))
    {
        return;
    }

    RegisterGameUpdate();
    CompactVehicles();
    for (const TWeakObjectPtr<AVehiclePawn>& ExistingVehicle : Vehicles)
    {
        if (ExistingVehicle.Get() == VehiclePawn)
        {
            return;
        }
    }

    Vehicles.Add(VehiclePawn);
}

void UVehicleSubSystem::UnregisterVehicle(AVehiclePawn* VehiclePawn)
{
    if (!EnsureVehicleSubsystemGameThread(TEXT("UVehicleSubSystem::UnregisterVehicle")))
    {
        return;
    }

    if (!VehiclePawn)
    {
        return;
    }

    Vehicles.RemoveAllSwap([VehiclePawn](const TWeakObjectPtr<AVehiclePawn>& ExistingVehicle)
    {
        return !ExistingVehicle.IsValid() || ExistingVehicle.Get() == VehiclePawn;
    }, EAllowShrinking::No);

    if (Vehicles.Num() == 0)
    {
        Vehicles.Empty();
        UnregisterGameUpdate();
    }
}

void UVehicleSubSystem::UpdateVehiclesFromGameUpdate(float DeltaSeconds)
{
    if (!EnsureVehicleSubsystemGameThread(TEXT("UVehicleSubSystem::UpdateVehiclesFromGameUpdate")))
    {
        return;
    }

    const float SafeDeltaSeconds = FMath::Clamp(DeltaSeconds, 0.0f, 1.0f);
    if (SafeDeltaSeconds <= 0.0f)
    {
        CompactVehicles();
        return;
    }

    CompactVehicles();
    const int32 VehicleCount = Vehicles.Num();
    if (VehicleCount == 0)
    {
        Vehicles.Empty();
        UnregisterGameUpdate();
        return;
    }

    TArray<AVehiclePawn*> VehiclePtrs;
    TArray<AVehiclePawn::FVehicleParallelControlInput> Inputs;
    VehiclePtrs.Reserve(VehicleCount);
    Inputs.Reserve(VehicleCount);

    for (const TWeakObjectPtr<AVehiclePawn>& Vehicle : Vehicles)
    {
        if (AVehiclePawn* VehiclePawn = Vehicle.Get())
        {
            if (!VehiclePawn->ShouldUpdateVehicleSimulation())
            {
                continue;
            }
            VehiclePtrs.Add(VehiclePawn);
            Inputs.Add(VehiclePawn->BuildParallelControlInput(SafeDeltaSeconds));
        }
    }

    const int32 ValidVehicleCount = VehiclePtrs.Num();
    if (ValidVehicleCount == 0)
    {
        return;
    }

    TArray<AVehiclePawn::FVehicleParallelControlOutput> Outputs;
    Outputs.SetNum(ValidVehicleCount);

    // Task-graph setup costs more than this small scalar calculation for ordinary vehicle counts.
    constexpr int32 ParallelVehicleThreshold = 32;
    if (ValidVehicleCount >= ParallelVehicleThreshold)
    {
        // Worker tasks receive immutable value snapshots and write to disjoint array elements.
        // No actor, component, physics body, or other UObject is dereferenced inside ParallelFor.
        ParallelFor(ValidVehicleCount, [&Inputs, &Outputs](const int32 Index)
        {
            Outputs[Index] = AVehiclePawn::CalculateParallelControlOutput(Inputs[Index]);
        });
    }
    else
    {
        for (int32 Index = 0; Index < ValidVehicleCount; ++Index)
        {
            Outputs[Index] = AVehiclePawn::CalculateParallelControlOutput(Inputs[Index]);
        }
    }

    for (int32 Index = 0; Index < ValidVehicleCount; ++Index)
    {
        AVehiclePawn* VehiclePawn = VehiclePtrs[Index];
        if (!IsValid(VehiclePawn))
        {
            continue;
        }

        VehiclePawn->ApplyParallelControlOutput(Outputs[Index]);
        VehiclePawn->UpdateVehicleFromSubSystem(SafeDeltaSeconds);
    }
}

void UVehicleSubSystem::RegisterGameUpdate()
{
    check(IsInGameThread());
    if (GameUpdateHandle != INDEX_NONE)
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        TWeakObjectPtr<UVehicleSubSystem> WeakThis(this);
        GameUpdateHandle = GameUpdate->RegisterUpdate(
            this,
            [WeakThis](const float DeltaSeconds)
            {
                if (UVehicleSubSystem* StrongThis = WeakThis.Get())
                {
                    StrongThis->UpdateVehiclesFromGameUpdate(DeltaSeconds);
                }
            },
            20);
    }
}

void UVehicleSubSystem::UnregisterGameUpdate()
{
    check(IsInGameThread());
    if (GameUpdateHandle == INDEX_NONE)
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateHandle);
    }
    GameUpdateHandle = INDEX_NONE;
}

void UVehicleSubSystem::CompactVehicles()
{
    check(IsInGameThread());
    const int32 RemovedCount = Vehicles.RemoveAllSwap([](const TWeakObjectPtr<AVehiclePawn>& Vehicle)
    {
        return !Vehicle.IsValid();
    }, EAllowShrinking::No);

    if (Vehicles.Num() == 0)
    {
        Vehicles.Empty();
    }
    else if (RemovedCount > 0 && Vehicles.Max() > FMath::Max(32, Vehicles.Num() * 2))
    {
        Vehicles.Shrink();
    }
}
