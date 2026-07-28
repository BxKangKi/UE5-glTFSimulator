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

namespace
{
    template <typename TObjectType>
    static void RetainGeneratedObject(TObjectType* Object)
    {
        if (IsValid(Object) && !Object->IsAsset() && !Object->IsRooted())
        {
            // The registry owns runtime-generated objects between async creation and component
            // attachment. Release paths remove this root before marking the object as garbage.
            Object->AddToRoot();
        }
    }

    template <typename TObjectType>
    static void MarkGeneratedObjectAsGarbage(TObjectType* Object)
    {
        if (IsValid(Object) && !Object->IsAsset())
        {
            if (Object->IsRooted())
            {
                Object->RemoveFromRoot();
            }
            Object->ClearFlags(RF_Public | RF_Standalone);
            Object->MarkAsGarbage();
        }
    }
}

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
    if (bActive)
    {
        return;
    }

    bActive = true;
    WriteLogAsync(TEXT("AssetManageSubSystem activated for main world"));
}

void UAssetManageSubSystem::DeactivateAndRelease()
{
    FScopeLock Lock(&RegistryLock);

    for (TPair<FManagedStaticMeshKey, FManagedStaticMeshEntry>& Pair : StaticMeshSet)
    {
        MarkGeneratedObjectAsGarbage(Pair.Value.Asset.Get());
    }
    for (TPair<TWeakObjectPtr<UMaterialInterface>, FManagedMaterialEntry>& Pair : MaterialSet)
    {
        MarkGeneratedObjectAsGarbage(Pair.Value.Asset.Get());
    }
    for (TPair<TWeakObjectPtr<UTexture>, FManagedTextureEntry>& Pair : TextureSet)
    {
        MarkGeneratedObjectAsGarbage(Pair.Value.Asset.Get());
    }

    StaticMeshSet.Empty();
    MaterialSet.Empty();
    TextureSet.Empty();
    bActive = false;
    WriteLogAsync(TEXT("AssetManageSubSystem deactivated and runtime asset registries were cleared"));
}

UStaticMesh* UAssetManageSubSystem::AcquireStaticMesh(
    UObject* WorldContextObject,
    const FName& MeshKey,
    UStaticMesh* GeneratedMeshAsset)
{
    if (!bActive || !IsValid(GeneratedMeshAsset) || GeneratedMeshAsset->IsAsset())
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

    if (FManagedStaticMeshEntry* ExistingEntry = StaticMeshSet.Find(RegistryKey))
    {
        if (ExistingEntry->Asset.IsValid())
        {
            ++ExistingEntry->RefCount;
            UStaticMesh* ExistingMesh = ExistingEntry->Asset.Get();
            if (ExistingMesh != GeneratedMeshAsset)
            {
                MarkGeneratedObjectAsGarbage(GeneratedMeshAsset);
            }
            WriteLogAsync(FString::Printf(
                TEXT("StaticMesh reused by scoped key. Scope=%s Key=%s RefCount=%d"),
                *GetNameSafe(RegistryScope),
                *MeshKey.ToString(),
                ExistingEntry->RefCount));
            return ExistingMesh;
        }

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
    if (!bActive || !IsValid(GeneratedMeshAsset) || GeneratedMeshAsset->IsAsset())
    {
        return;
    }

    FScopeLock Lock(&RegistryLock);
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
                MarkGeneratedObjectAsGarbage(GeneratedMeshAsset);
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
    if (!bActive || !IsValid(GeneratedMaterialAsset) || GeneratedMaterialAsset->IsAsset())
    {
        return GeneratedMaterialAsset;
    }

    // Runtime material instances may share the same short UObject name under different outers
    // while containing completely different glTF texture parameters. Track exact identity only.
    const TWeakObjectPtr<UMaterialInterface> RegistryKey(GeneratedMaterialAsset);
    FScopeLock Lock(&RegistryLock);

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
    if (!bActive || !IsValid(GeneratedMaterialAsset) || GeneratedMaterialAsset->IsAsset())
    {
        return;
    }

    const TWeakObjectPtr<UMaterialInterface> RegistryKey(GeneratedMaterialAsset);
    FScopeLock Lock(&RegistryLock);
    if (FManagedMaterialEntry* Entry = MaterialSet.Find(RegistryKey))
    {
        Entry->RefCount = FMath::Max(0, Entry->RefCount - 1);
        if (Entry->RefCount <= 0)
        {
            MarkGeneratedObjectAsGarbage(GeneratedMaterialAsset);
            MaterialSet.Remove(RegistryKey);
        }
    }
}

UTexture* UAssetManageSubSystem::AcquireTexture(UObject* WorldContextObject, UTexture* GeneratedTextureAsset)
{
    if (!bActive || !IsValid(GeneratedTextureAsset) || GeneratedTextureAsset->IsAsset())
    {
        return GeneratedTextureAsset;
    }

    // Texture object names are not content hashes. Different imported images can legally have the
    // same short UObject name, so only repeated acquisition of the same object is coalesced.
    const TWeakObjectPtr<UTexture> RegistryKey(GeneratedTextureAsset);
    FScopeLock Lock(&RegistryLock);

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
    if (!bActive || !IsValid(GeneratedTextureAsset) || GeneratedTextureAsset->IsAsset())
    {
        return;
    }

    const TWeakObjectPtr<UTexture> RegistryKey(GeneratedTextureAsset);
    FScopeLock Lock(&RegistryLock);
    if (FManagedTextureEntry* Entry = TextureSet.Find(RegistryKey))
    {
        Entry->RefCount = FMath::Max(0, Entry->RefCount - 1);
        if (Entry->RefCount <= 0)
        {
            MarkGeneratedObjectAsGarbage(GeneratedTextureAsset);
            TextureSet.Remove(RegistryKey);
        }
    }
}

void UAssetManageSubSystem::WriteLogAsync(const FString& Message) const
{
    UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("AssetManageSubSystem"), Message);
}
