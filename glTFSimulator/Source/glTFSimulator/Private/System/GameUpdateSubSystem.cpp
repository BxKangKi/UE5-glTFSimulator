// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/GameUpdateSubSystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Subsystems/SubsystemCollection.h"

UGameUpdateSubSystem::UGameUpdateSubSystem()
{
}

UGameUpdateSubSystem* UGameUpdateSubSystem::Get(const UObject* WorldContextObject)
{
    if (!IsValid(WorldContextObject))
    {
        return nullptr;
    }

    if (const UGameInstance* GameInstance = Cast<UGameInstance>(WorldContextObject))
    {
        return const_cast<UGameInstance*>(GameInstance)->GetSubsystem<UGameUpdateSubSystem>();
    }

    if (const UGameInstanceSubsystem* GameInstanceSubsystem = Cast<UGameInstanceSubsystem>(WorldContextObject))
    {
        if (UGameInstance* GameInstance = GameInstanceSubsystem->GetGameInstance())
        {
            return GameInstance->GetSubsystem<UGameUpdateSubSystem>();
        }
    }

    UWorld* World = WorldContextObject->GetWorld();
    if (!World)
    {
        return nullptr;
    }

    UGameInstance* GameInstance = World->GetGameInstance();
    return GameInstance ? GameInstance->GetSubsystem<UGameUpdateSubSystem>() : nullptr;
}

void UGameUpdateSubSystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    bInitialized = true;
    MarkDispatchOrderDirty();
}

void UGameUpdateSubSystem::Deinitialize()
{
    bInitialized = false;
    bIsDispatching = false;
    bSortedHandlesDirty = true;
    PendingRemoveHandleSet.Empty();
    SortedHandles.Empty();
    UpdateEntries.Empty();
    Super::Deinitialize();
}

UWorld* UGameUpdateSubSystem::GetTickableGameObjectWorld() const
{
    const UGameInstance* GameInstance = GetGameInstance();
    return GameInstance ? GameInstance->GetWorld() : nullptr;
}

int32 UGameUpdateSubSystem::RegisterUpdate(UObject* Owner, TFunction<void(float)>&& UpdateFunction, int32 Priority)
{
    if (!IsValid(Owner) || !UpdateFunction)
    {
        return INDEX_NONE;
    }

    const int32 Handle = NextHandle++;
    FGameUpdateEntry& Entry = UpdateEntries.Add(Handle);
    Entry.Owner = Owner;
    Entry.UpdateFunction = MoveTemp(UpdateFunction);
    Entry.Priority = Priority;
    Entry.Serial = NextSerial++;

    MarkDispatchOrderDirty();
    return Handle;
}

void UGameUpdateSubSystem::UnregisterUpdate(int32 Handle)
{
    if (Handle == INDEX_NONE)
    {
        return;
    }

    if (bIsDispatching)
    {
        PendingRemoveHandleSet.Add(Handle);
        return;
    }

    if (UpdateEntries.Remove(Handle) > 0)
    {
        MarkDispatchOrderDirty();
    }
}

void UGameUpdateSubSystem::UnregisterOwner(const UObject* Owner)
{
    if (!Owner)
    {
        return;
    }

    for (const TPair<int32, FGameUpdateEntry>& Pair : UpdateEntries)
    {
        if (!Pair.Value.Owner.IsValid() || Pair.Value.Owner.Get() == Owner)
        {
            PendingRemoveHandleSet.Add(Pair.Key);
        }
    }

    // If called from inside a callback, defer physical removal until the dispatcher
    // finishes iterating the stable handle array.
    if (!bIsDispatching)
    {
        FlushPendingRemovals();
    }
}

void UGameUpdateSubSystem::Tick(float DeltaTime)
{
    if (!bInitialized)
    {
        return;
    }

    if (UpdateEntries.Num() == 0)
    {
        return;
    }

    if (DeltaTime <= 0.0f)
    {
        RemoveInvalidEntries();
        return;
    }

    RebuildDispatchOrderIfNeeded();

    struct FScopedDispatchFlag
    {
        bool& Flag;
        const bool PreviousValue;

        explicit FScopedDispatchFlag(bool& InFlag)
            : Flag(InFlag)
            , PreviousValue(InFlag)
        {
            Flag = true;
        }

        ~FScopedDispatchFlag()
        {
            Flag = PreviousValue;
        }
    } DispatchGuard(bIsDispatching);

    for (const int32 Handle : SortedHandles)
    {
        // Most frames have no deferred removals. Avoid a hash lookup for every
        // registered callback unless a callback actually unregistered something.
        if (PendingRemoveHandleSet.Num() > 0 && PendingRemoveHandleSet.Contains(Handle))
        {
            continue;
        }

        FGameUpdateEntry* Entry = UpdateEntries.Find(Handle);
        if (!Entry || !Entry->Owner.IsValid() || !Entry->UpdateFunction)
        {
            PendingRemoveHandleSet.Add(Handle);
            continue;
        }

        Entry->UpdateFunction(DeltaTime);
    }

    FlushPendingRemovals();
}

void UGameUpdateSubSystem::MarkDispatchOrderDirty()
{
    bSortedHandlesDirty = true;
}

void UGameUpdateSubSystem::RebuildDispatchOrderIfNeeded()
{
    if (!bSortedHandlesDirty)
    {
        return;
    }

    SortedHandles.Reset(UpdateEntries.Num());
    for (const TPair<int32, FGameUpdateEntry>& Pair : UpdateEntries)
    {
        if (Pair.Value.Owner.IsValid() && Pair.Value.UpdateFunction)
        {
            SortedHandles.Add(Pair.Key);
        }
        else
        {
            PendingRemoveHandleSet.Add(Pair.Key);
        }
    }

    SortedHandles.Sort([this](const int32 A, const int32 B)
    {
        const FGameUpdateEntry* EntryA = UpdateEntries.Find(A);
        const FGameUpdateEntry* EntryB = UpdateEntries.Find(B);
        if (!EntryA || !EntryB)
        {
            return A < B;
        }
        if (EntryA->Priority != EntryB->Priority)
        {
            return EntryA->Priority < EntryB->Priority;
        }
        return EntryA->Serial < EntryB->Serial;
    });

    bSortedHandlesDirty = false;
}

void UGameUpdateSubSystem::RemoveInvalidEntries()
{
    for (const TPair<int32, FGameUpdateEntry>& Pair : UpdateEntries)
    {
        if (!Pair.Value.Owner.IsValid() || !Pair.Value.UpdateFunction)
        {
            PendingRemoveHandleSet.Add(Pair.Key);
        }
    }

    FlushPendingRemovals();
}

void UGameUpdateSubSystem::FlushPendingRemovals()
{
    if (PendingRemoveHandleSet.Num() == 0)
    {
        return;
    }

    int32 RemovedCount = 0;
    for (const int32 Handle : PendingRemoveHandleSet)
    {
        RemovedCount += UpdateEntries.Remove(Handle);
    }
    PendingRemoveHandleSet.Empty();

    if (RemovedCount > 0)
    {
        MarkDispatchOrderDirty();
    }
}
