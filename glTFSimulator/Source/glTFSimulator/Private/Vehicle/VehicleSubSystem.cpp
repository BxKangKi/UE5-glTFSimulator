// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Vehicle/VehicleSubSystem.h"

#include "Async/ParallelFor.h"
#include "Engine/World.h"
#include "Subsystems/SubsystemCollection.h"
#include "System/GameUpdateSubSystem.h"
#include "Vehicle/VehiclePawn.h"

UVehicleSubSystem::UVehicleSubSystem()
{
}

UVehicleSubSystem* UVehicleSubSystem::Get(const UObject* WorldContextObject)
{
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
    Super::Initialize(Collection);
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdateHandle = GameUpdate->RegisterUpdate(
            this,
            [this](const float DeltaSeconds)
            {
                UpdateVehiclesFromGameUpdate(DeltaSeconds);
            },
            20);
    }
}

void UVehicleSubSystem::Deinitialize()
{
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateHandle);
    }
    GameUpdateHandle = INDEX_NONE;
    Vehicles.Empty();

    Super::Deinitialize();
}

void UVehicleSubSystem::RegisterVehicle(AVehiclePawn* VehiclePawn)
{
    if (!IsValid(VehiclePawn))
    {
        return;
    }

    if (GameUpdateHandle == INDEX_NONE)
    {
        if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
        {
            GameUpdateHandle = GameUpdate->RegisterUpdate(
                this,
                [this](const float DeltaSeconds)
                {
                    UpdateVehiclesFromGameUpdate(DeltaSeconds);
                },
                20);
        }
    }

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
    if (!VehiclePawn)
    {
        return;
    }

    Vehicles.RemoveAllSwap([VehiclePawn](const TWeakObjectPtr<AVehiclePawn>& ExistingVehicle)
    {
        return !ExistingVehicle.IsValid() || ExistingVehicle.Get() == VehiclePawn;
    }, EAllowShrinking::No);
}

void UVehicleSubSystem::UpdateVehiclesFromGameUpdate(float DeltaSeconds)
{
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

    if (ValidVehicleCount >= 4)
    {
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

void UVehicleSubSystem::CompactVehicles()
{
    Vehicles.RemoveAllSwap([](const TWeakObjectPtr<AVehiclePawn>& Vehicle)
    {
        return !Vehicle.IsValid();
    }, EAllowShrinking::No);
}
