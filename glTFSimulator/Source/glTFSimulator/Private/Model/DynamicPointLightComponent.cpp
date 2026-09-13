// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file DynamicPointLightComponent.cpp
 * 역할: 동적 포인트 조명 컴포넌트를 제공합니다.
 * 핵심 기능: 조명 수명과 동적 조명 서브시스템 연동.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
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