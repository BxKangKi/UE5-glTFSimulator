// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "GameUpdateSubSystem.generated.h"

/**
 * Global game-instance update dispatcher.
 *
 * Gameplay classes register compact update callbacks here instead of enabling their
 * own Actor/Component/Subsystem tick functions. The dispatcher keeps one sorted
 * execution list and rebuilds it only when registration changes, so per-frame work
 * is a linear pass over live callbacks instead of many scattered tick functions and
 * a sort every frame.
 */
UCLASS()
class GLTFSIMULATOR_API UGameUpdateSubSystem : public UGameInstanceSubsystem, public FTickableGameObject
{
    GENERATED_BODY()

public:
    UGameUpdateSubSystem();

    static UGameUpdateSubSystem* Get(const UObject* WorldContextObject);

    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    /** Registers a game-thread update callback. Returns INDEX_NONE when registration fails. */
    int32 RegisterUpdate(UObject* Owner, TFunction<void(float)>&& UpdateFunction, int32 Priority = 0);

    void UnregisterUpdate(int32 Handle);
    void UnregisterOwner(const UObject* Owner);

    virtual void Tick(float DeltaTime) override;
    virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }
    virtual bool IsTickable() const override { return !IsTemplate() && bInitialized && UpdateEntries.Num() > 0; }
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UGameUpdateSubSystem, STATGROUP_Tickables); }
    virtual UWorld* GetTickableGameObjectWorld() const override;

    UFUNCTION(BlueprintPure, Category="Game Update")
    int32 GetRegisteredUpdateCount() const { return UpdateEntries.Num(); }

private:
    struct FGameUpdateEntry
    {
        TWeakObjectPtr<UObject> Owner;
        TFunction<void(float)> UpdateFunction;
        int32 Priority = 0;
        uint64 Serial = 0;
    };

    TMap<int32, FGameUpdateEntry> UpdateEntries;
    TArray<int32> SortedHandles;
    TSet<int32> PendingRemoveHandleSet;
    int32 NextHandle = 1;
    uint64 NextSerial = 1;
    bool bInitialized = false;
    bool bIsDispatching = false;
    bool bSortedHandlesDirty = true;

    void MarkDispatchOrderDirty();
    void RebuildDispatchOrderIfNeeded();
    void RemoveInvalidEntries();
    void FlushPendingRemovals();
};
