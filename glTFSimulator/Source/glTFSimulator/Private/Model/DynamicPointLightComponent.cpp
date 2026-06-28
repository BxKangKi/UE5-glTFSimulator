// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Model/DynamicPointLightComponent.h"
#include "Model/DynamicLightSubsystem.h"
#include "Engine/World.h"

UDynamicPointLightComponent::UDynamicPointLightComponent()
{
    PrimaryComponentTick.bCanEverTick = false; // 틱을 완전히 꺼서 무거운 스케줄링 오버헤드 제거
}

void UDynamicPointLightComponent::BeginPlay()
{
    Super::BeginPlay();

    // 월드 서브시스템에 관리대상으로 등록
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
    // 소멸 시 서브시스템에서 해제
    if (UWorld *World = GetWorld())
    {
        if (auto *Subsystem = World->GetSubsystem<UDynamicLightSubsystem>())
        {
            Subsystem->UnregisterLight(this);
        }
    }

    Super::EndPlay(EndPlayReason);
}