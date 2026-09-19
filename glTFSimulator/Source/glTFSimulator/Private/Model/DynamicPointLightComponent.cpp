// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file DynamicPointLightComponent.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Model/DynamicPointLightComponent.h"
#include "Model/DynamicLightSubsystem.h"
#include "Engine/World.h"

UDynamicPointLightComponent::UDynamicPointLightComponent()
{
    PrimaryComponentTick.bCanEverTick = false; // Disable ticking to remove scheduling overhead.
}

void UDynamicPointLightComponent::BeginPlay()
{
    Super::BeginPlay();

    // Register this component with the world subsystem.
    if (UWorld *World = GetWorld())
    {
        if (auto *Subsystem = World->GetSubsystem<UDynamicLightSubsystem>())
        {
            Subsystem->RegisterLight(this);
        }
    }
}

void UDynamicPointLightComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // Unregister this component when it is destroyed.
    if (UWorld *World = GetWorld())
    {
        if (auto *Subsystem = World->GetSubsystem<UDynamicLightSubsystem>())
        {
            Subsystem->UnregisterLight(this);
        }
    }

    Super::EndPlay(EndPlayReason);
}