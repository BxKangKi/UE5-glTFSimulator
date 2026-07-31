// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "System/AssetManageSubSystem.h"

#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopeLock.h"
#include "System/FileFunctionLibrary.h"
#include "System/MacroLibrary.h"

UAssetManageSubSystem* UAssetManageSubSystem::Get(UObject* WorldContextObject)
{
    if (!IsValid(WorldContextObject))
    {
        return nullptr;
    }

    UWorld* World = WorldContextObject->GetWorld();
    UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
    return IsValid(GameInstance) ? GameInstance->GetSubsystem<UAssetManageSubSystem>() : nullptr;
}

void UAssetManageSubSystem::Deinitialize()
{
    DeactivateAndRelease();
    Super::Deinitialize();
}

void UAssetManageSubSystem::ActivateForMainWorld(UObject* WorldContextObject)
{
    {
        FScopeLock Lock(&RegistryLock);
        if (bActive)
        {
            return;
        }
        bActive = true;
    }

    WriteLogAsync(TEXT("AssetManageSubSystem activated for main world"));
}

void UAssetManageSubSystem::DeactivateAndRelease()
{
    {
        FScopeLock Lock(&RegistryLock);

        // Block new registrations before releasing roots. Acquire calls re-check this state while
        // holding the same lock, so a request that raced with shutdown cannot repopulate a cleared
        // registry.
        bActive = false;
        ReleaseAllGeneratedObjectReferences();
        StaticMeshSet.Empty();
        MaterialSet.Empty();
        TextureSet.Empty();
    }

    WriteLogAsync(TEXT("AssetManageSubSystem deactivated and runtime asset registries were cleared"));
}

UStaticMesh* UAssetManageSubSystem::AcquireStaticMesh(
    UObject* WorldContextObject,
    const FName& MeshKey,
    UStaticMesh* GeneratedMeshAsset)
{
    if (!IsValid(GeneratedMeshAsset) || GeneratedMeshAsset->IsAsset())
    {
        return GeneratedMeshAsset;
    }

    // A glTF mesh name is only unique inside its owning runtime asset. Scope it to the caller
    // (normally AglTFStreamActor) so different world files with the same mesh name never merge.
    UObject* RegistryScope = IsValid(WorldContextObject)
        ? WorldContextObject
        : GeneratedMeshAsset->GetOuter();
    if (MeshKey.IsNone())
    {
        // An unnamed mesh must never be merged with another unnamed mesh.
        RegistryScope = GeneratedMeshAsset;
    }

    const FManagedStaticMeshKey RegistryKey(RegistryScope, MeshKey);
    FScopeLock Lock(&RegistryLock);
    if (!bActive)
    {
        return GeneratedMeshAsset;
    }

    if (FManagedStaticMeshEntry* ExistingEntry = StaticMeshSet.Find(RegistryKey))
    {
        if (ExistingEntry->Asset.IsValid())
        {
            ++ExistingEntry->RefCount;
            UStaticMesh* ExistingMesh = ExistingEntry->Asset.Get();
            WriteLogAsync(FString::Printf(
                TEXT("StaticMesh reused by scoped key. Scope=%s Key=%s RefCount=%d"),
                *GetNameSafe(RegistryScope),
                *MeshKey.ToString(),
                ExistingEntry->RefCount));
            return ExistingMesh;
        }

        ReleaseGeneratedObjectReference(ExistingEntry->Asset.Get());
        StaticMeshSet.Remove(RegistryKey);
    }

    RetainGeneratedObject(GeneratedMeshAsset);

    FManagedStaticMeshEntry NewEntry;
    NewEntry.Asset = GeneratedMeshAsset;
    NewEntry.RefCount = 1;
    StaticMeshSet.Add(RegistryKey, NewEntry);
    WriteLogAsync(FString::Printf(
        TEXT("StaticMesh registered by scoped key. Scope=%s Key=%s"),
        *GetNameSafe(RegistryScope),
        *MeshKey.ToString()));
    return GeneratedMeshAsset;
}

void UAssetManageSubSystem::ReleaseStaticMesh(UObject* WorldContextObject, UStaticMesh* GeneratedMeshAsset)
{
    if (!IsValid(GeneratedMeshAsset) || GeneratedMeshAsset->IsAsset())
    {
        return;
    }

    FScopeLock Lock(&RegistryLock);
    if (!bActive)
    {
        return;
    }

    for (auto It = StaticMeshSet.CreateIterator(); It; ++It)
    {
        FManagedStaticMeshEntry& Entry = It.Value();
        if (Entry.Asset.Get() == GeneratedMeshAsset)
        {
            Entry.RefCount = FMath::Max(0, Entry.RefCount - 1);
            WriteLogAsync(FString::Printf(
                TEXT("StaticMesh released. Key=%s RefCount=%d"),
                *It.Key().MeshKey.ToString(),
                Entry.RefCount));
            if (Entry.RefCount <= 0)
            {
                ReleaseGeneratedObjectReference(GeneratedMeshAsset);
                It.RemoveCurrent();
            }
            return;
        }
    }
}

UMaterialInterface* UAssetManageSubSystem::AcquireMaterial(
    UObject* WorldContextObject,
    UMaterialInterface* GeneratedMaterialAsset)
{
    if (!IsValid(GeneratedMaterialAsset) || GeneratedMaterialAsset->IsAsset())
    {
        return GeneratedMaterialAsset;
    }

    // Runtime material instances may share the same short UObject name under different outers
    // while containing completely different glTF texture parameters. Track exact identity only.
    const TWeakObjectPtr<UMaterialInterface> RegistryKey(GeneratedMaterialAsset);
    FScopeLock Lock(&RegistryLock);
    if (!bActive)
    {
        return GeneratedMaterialAsset;
    }

    if (FManagedMaterialEntry* ExistingEntry = MaterialSet.Find(RegistryKey))
    {
        ++ExistingEntry->RefCount;
        return GeneratedMaterialAsset;
    }

    RetainGeneratedObject(GeneratedMaterialAsset);

    FManagedMaterialEntry NewEntry;
    NewEntry.Asset = GeneratedMaterialAsset;
    NewEntry.RefCount = 1;
    MaterialSet.Add(RegistryKey, NewEntry);
    return GeneratedMaterialAsset;
}

void UAssetManageSubSystem::ReleaseMaterial(
    UObject* WorldContextObject,
    UMaterialInterface* GeneratedMaterialAsset)
{
    if (!IsValid(GeneratedMaterialAsset) || GeneratedMaterialAsset->IsAsset())
    {
        return;
    }

    const TWeakObjectPtr<UMaterialInterface> RegistryKey(GeneratedMaterialAsset);
    FScopeLock Lock(&RegistryLock);
    if (!bActive)
    {
        return;
    }

    if (FManagedMaterialEntry* Entry = MaterialSet.Find(RegistryKey))
    {
        Entry->RefCount = FMath::Max(0, Entry->RefCount - 1);
        if (Entry->RefCount <= 0)
        {
            ReleaseGeneratedObjectReference(GeneratedMaterialAsset);
            MaterialSet.Remove(RegistryKey);
        }
    }
}

UTexture* UAssetManageSubSystem::AcquireTexture(UObject* WorldContextObject, UTexture* GeneratedTextureAsset)
{
    if (!IsValid(GeneratedTextureAsset) || GeneratedTextureAsset->IsAsset())
    {
        return GeneratedTextureAsset;
    }

    // Texture object names are not content hashes. Different imported images can legally have the
    // same short UObject name, so only repeated acquisition of the same object is coalesced.
    const TWeakObjectPtr<UTexture> RegistryKey(GeneratedTextureAsset);
    FScopeLock Lock(&RegistryLock);
    if (!bActive)
    {
        return GeneratedTextureAsset;
    }

    if (FManagedTextureEntry* ExistingEntry = TextureSet.Find(RegistryKey))
    {
        ++ExistingEntry->RefCount;
        return GeneratedTextureAsset;
    }

    RetainGeneratedObject(GeneratedTextureAsset);

    FManagedTextureEntry NewEntry;
    NewEntry.Asset = GeneratedTextureAsset;
    NewEntry.RefCount = 1;
    TextureSet.Add(RegistryKey, NewEntry);
    return GeneratedTextureAsset;
}

void UAssetManageSubSystem::ReleaseTexture(UObject* WorldContextObject, UTexture* GeneratedTextureAsset)
{
    if (!IsValid(GeneratedTextureAsset) || GeneratedTextureAsset->IsAsset())
    {
        return;
    }

    const TWeakObjectPtr<UTexture> RegistryKey(GeneratedTextureAsset);
    FScopeLock Lock(&RegistryLock);
    if (!bActive)
    {
        return;
    }

    if (FManagedTextureEntry* Entry = TextureSet.Find(RegistryKey))
    {
        Entry->RefCount = FMath::Max(0, Entry->RefCount - 1);
        if (Entry->RefCount <= 0)
        {
            ReleaseGeneratedObjectReference(GeneratedTextureAsset);
            TextureSet.Remove(RegistryKey);
        }
    }
}

bool UAssetManageSubSystem::IsActive() const
{
    FScopeLock Lock(&RegistryLock);
    return bActive;
}

void UAssetManageSubSystem::RetainGeneratedObject(UObject* Object)
{
    if (!IsValid(Object) || Object->IsAsset())
    {
        return;
    }

    const TWeakObjectPtr<UObject> ObjectKey(Object);
    if (int32* RegistrationCount = OwnedRootRegistrationCounts.Find(ObjectKey))
    {
        ++(*RegistrationCount);
        return;
    }

    // An existing root belongs to another system. AddToRoot is a global bit rather than an
    // ownership-counted operation, so this subsystem must leave that root and its flags untouched.
    if (Object->IsRooted())
    {
        return;
    }

    Object->AddToRoot();
    OwnedRootRegistrationCounts.Add(ObjectKey, 1);
}

void UAssetManageSubSystem::ReleaseGeneratedObjectReference(UObject* Object)
{
    if (!Object)
    {
        return;
    }

    const TWeakObjectPtr<UObject> ObjectKey(Object);
    int32* RegistrationCount = OwnedRootRegistrationCounts.Find(ObjectKey);
    if (!RegistrationCount)
    {
        // The subsystem never added this object's root, so it does not own either the root or the
        // lifetime-related flags.
        return;
    }

    --(*RegistrationCount);
    if (*RegistrationCount > 0)
    {
        return;
    }

    OwnedRootRegistrationCounts.Remove(ObjectKey);
    if (!IsValid(Object))
    {
        return;
    }

    if (Object->IsRooted())
    {
        Object->RemoveFromRoot();
    }
    Object->ClearFlags(RF_Public | RF_Standalone);
    // Do not force MarkAsGarbage while render/physics commands can still hold a transient
    // reference. Normal GC reclaims the object after registry/component references disappear.
}

void UAssetManageSubSystem::ReleaseAllGeneratedObjectReferences()
{
    for (const TPair<TWeakObjectPtr<UObject>, int32>& Pair : OwnedRootRegistrationCounts)
    {
        UObject* Object = Pair.Key.Get();
        if (!IsValid(Object))
        {
            continue;
        }

        if (Object->IsRooted())
        {
            Object->RemoveFromRoot();
        }
        Object->ClearFlags(RF_Public | RF_Standalone);
    }
    OwnedRootRegistrationCounts.Empty();
}

void UAssetManageSubSystem::WriteLogAsync(const FString& Message) const
{
    UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("AssetManageSubSystem"), Message);
}
