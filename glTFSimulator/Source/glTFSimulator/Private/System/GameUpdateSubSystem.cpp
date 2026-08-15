// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/GameUpdateSubSystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Subsystems/SubsystemCollection.h"

namespace
{
    /** The dispatcher owns UObject weak pointers and callback containers, so one thread must mutate it. */
    bool EnsureGameUpdateThread(const TCHAR* Context)
    {
        return ensureMsgf(IsInGameThread(), TEXT("%s must run on the game thread"), Context);
    }
}

UGameUpdateSubSystem::UGameUpdateSubSystem()
{
}

UGameUpdateSubSystem* UGameUpdateSubSystem::Get(const UObject* WorldContextObject)
{
    if (!EnsureGameUpdateThread(TEXT("UGameUpdateSubSystem::Get")))
    {
        return nullptr;
    }

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
    check(IsInGameThread());
    Super::Initialize(Collection);
    bInitialized = true;
    MarkDispatchOrderDirty();
}

void UGameUpdateSubSystem::Deinitialize()
{
    check(IsInGameThread());
    bInitialized = false;
    bIsDispatching = false;
    bSortedHandlesDirty = true;
    PendingRemoveHandleSet.Empty();
    SortedHandles.Empty();
    UpdateEntries.Empty();
    NextHandle = 1;
    NextSerial = 1;
    Super::Deinitialize();
}

UWorld* UGameUpdateSubSystem::GetTickableGameObjectWorld() const
{
    const UGameInstance* GameInstance = GetGameInstance();
    return GameInstance ? GameInstance->GetWorld() : nullptr;
}

int32 UGameUpdateSubSystem::RegisterUpdate(UObject* Owner, TFunction<void(float)>&& UpdateFunction, int32 Priority)
{
    if (!EnsureGameUpdateThread(TEXT("UGameUpdateSubSystem::RegisterUpdate")))
    {
        return INDEX_NONE;
    }

    if (!IsValid(Owner) || !UpdateFunction)
    {
        return INDEX_NONE;
    }

    // The counter normally never wraps, but persistent game instances can run indefinitely. Skip
    // INDEX_NONE/non-positive values and any still-live handle rather than overwriting a callback.
    int32 Handle = NextHandle;
    for (int32 Attempts = 0; Attempts < MAX_int32; ++Attempts)
    {
        if (NextHandle >= MAX_int32)
        {
            NextHandle = 1;
        }
        else
        {
            ++NextHandle;
        }

        if (Handle > 0 && Handle != INDEX_NONE && !UpdateEntries.Contains(Handle))
        {
            break;
        }
        Handle = NextHandle;
    }

    if (Handle <= 0 || Handle == INDEX_NONE || UpdateEntries.Contains(Handle))
    {
        ensureMsgf(false, TEXT("UGameUpdateSubSystem exhausted all callback handles"));
        return INDEX_NONE;
    }

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
    if (!EnsureGameUpdateThread(TEXT("UGameUpdateSubSystem::UnregisterUpdate")))
    {
        return;
    }

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
        if (UpdateEntries.Num() == 0)
        {
            // GameInstance subsystems survive map travel. Release all callback/capture allocator
            // storage when the last world-owned registration disappears.
            UpdateEntries.Empty();
            SortedHandles.Empty();
            PendingRemoveHandleSet.Empty();
            bSortedHandlesDirty = false;
        }
        else
        {
            MarkDispatchOrderDirty();
        }
    }
}

void UGameUpdateSubSystem::UnregisterOwner(const UObject* Owner)
{
    if (!EnsureGameUpdateThread(TEXT("UGameUpdateSubSystem::UnregisterOwner")))
    {
        return;
    }

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
    if (!EnsureGameUpdateThread(TEXT("UGameUpdateSubSystem::Tick")))
    {
        return;
    }

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
        if (UpdateEntries.Num() == 0)
        {
            UpdateEntries.Empty();
            SortedHandles.Empty();
            PendingRemoveHandleSet.Empty();
            bSortedHandlesDirty = false;
        }
        else
        {
            MarkDispatchOrderDirty();
        }
    }
}
