// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"
#include "GameUpdateSubSystem.generated.h"

/**
 * Single world-level game update dispatcher.
 *
 * Gameplay classes register small update callbacks here instead of enabling their own
 * Actor/Component tick functions. This keeps the engine tick function list short while
 * preserving explicit BeginPlay/EndPlay ownership for every registered callback.
 */
UCLASS()
class GLTFSIMULATOR_API UGameUpdateSubSystem : public UWorldSubsystem, public FTickableGameObject
{
    GENERATED_BODY()

public:
    UGameUpdateSubSystem();

    static UGameUpdateSubSystem* Get(const UObject* WorldContextObject);

    virtual void Deinitialize() override;

    /** Registers a game-thread update callback. Returns INDEX_NONE when registration fails. */
    int32 RegisterUpdate(UObject* Owner, TFunction<void(float)>&& UpdateFunction, int32 Priority = 0);

    void UnregisterUpdate(int32 Handle);
    void UnregisterOwner(const UObject* Owner);

    virtual void Tick(float DeltaTime) override;
    virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
    virtual bool IsTickable() const override { return !IsTemplate() && UpdateEntries.Num() > 0; }
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UGameUpdateSubSystem, STATGROUP_Tickables); }
    virtual UWorld* GetTickableGameObjectWorld() const override { return GetWorld(); }

private:
    struct FGameUpdateEntry
    {
        TWeakObjectPtr<UObject> Owner;
        TFunction<void(float)> UpdateFunction;
        int32 Priority = 0;
        uint64 Serial = 0;
    };

    TMap<int32, FGameUpdateEntry> UpdateEntries;
    int32 NextHandle = 1;
    uint64 NextSerial = 1;
    bool bIsDispatching = false;
    TArray<int32> PendingRemoveHandles;

    void RemoveInvalidEntries();
    void FlushPendingRemovals();
};
