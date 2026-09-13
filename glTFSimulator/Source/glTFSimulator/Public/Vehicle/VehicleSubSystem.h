// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file VehicleSubSystem.h
 * 역할: 월드 차량의 업데이트와 제어 계산을 통합합니다.
 * 핵심 기능: 차량 등록, GT 스냅샷, 병렬 제어 계산·결과 적용.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

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

    void RegisterGameUpdate();
    void UnregisterGameUpdate();
    void CompactVehicles();
};
