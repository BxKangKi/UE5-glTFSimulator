// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/GameUpdateSubSystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

UGameUpdateSubSystem::UGameUpdateSubSystem()
{
}

UGameUpdateSubSystem* UGameUpdateSubSystem::Get(const UObject* WorldContextObject)
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

    return World->GetSubsystem<UGameUpdateSubSystem>();
}

void UGameUpdateSubSystem::Deinitialize()
{
    PendingRemoveHandles.Empty();
    UpdateEntries.Empty();
    Super::Deinitialize();
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
        PendingRemoveHandles.AddUnique(Handle);
        return;
    }

    UpdateEntries.Remove(Handle);
}

void UGameUpdateSubSystem::UnregisterOwner(const UObject* Owner)
{
    if (!Owner)
    {
        return;
    }

    TArray<int32> HandlesToRemove;
    for (const TPair<int32, FGameUpdateEntry>& Pair : UpdateEntries)
    {
        if (!Pair.Value.Owner.IsValid() || Pair.Value.Owner.Get() == Owner)
        {
            HandlesToRemove.Add(Pair.Key);
        }
    }

    for (int32 Handle : HandlesToRemove)
    {
        UnregisterUpdate(Handle);
    }
}

void UGameUpdateSubSystem::Tick(float DeltaTime)
{
    if (DeltaTime <= 0.0f)
    {
        RemoveInvalidEntries();
        return;
    }

    TArray<int32> Handles;
    Handles.Reserve(UpdateEntries.Num());
    for (const TPair<int32, FGameUpdateEntry>& Pair : UpdateEntries)
    {
        if (Pair.Value.Owner.IsValid() && Pair.Value.UpdateFunction)
        {
            Handles.Add(Pair.Key);
        }
        else
        {
            PendingRemoveHandles.AddUnique(Pair.Key);
        }
    }

    Handles.Sort([this](const int32 A, const int32 B)
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
    for (int32 Handle : Handles)
    {
        if (PendingRemoveHandles.Contains(Handle))
        {
            continue;
        }

        FGameUpdateEntry* Entry = UpdateEntries.Find(Handle);
        if (!Entry || !Entry->Owner.IsValid() || !Entry->UpdateFunction)
        {
            PendingRemoveHandles.AddUnique(Handle);
            continue;
        }

        Entry->UpdateFunction(DeltaTime);
    }

    FlushPendingRemovals();
}

void UGameUpdateSubSystem::RemoveInvalidEntries()
{
    TArray<int32> HandlesToRemove;
    for (const TPair<int32, FGameUpdateEntry>& Pair : UpdateEntries)
    {
        if (!Pair.Value.Owner.IsValid() || !Pair.Value.UpdateFunction)
        {
            HandlesToRemove.Add(Pair.Key);
        }
    }

    for (int32 Handle : HandlesToRemove)
    {
        UpdateEntries.Remove(Handle);
    }
}

void UGameUpdateSubSystem::FlushPendingRemovals()
{
    for (int32 Handle : PendingRemoveHandles)
    {
        UpdateEntries.Remove(Handle);
    }
    PendingRemoveHandles.Reset();
}
