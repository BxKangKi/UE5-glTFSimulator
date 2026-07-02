// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Model/AssetManageSubSystem.h"

#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Crc.h"
#include "Misc/ScopeLock.h"
#include "System/FileFunctionLibrary.h"
#include "System/MacroLibrary.h"

namespace
{
    template <typename TObjectType>
    static void MarkRuntimeObjectAsGarbage(TObjectType* Object)
    {
        if (IsValid(Object) && !Object->IsAsset())
        {
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

    for (TPair<uint32, FManagedRuntimeStaticMeshEntry>& Pair : StaticMeshSet)
    {
        MarkRuntimeObjectAsGarbage(Pair.Value.Asset.Get());
    }
    for (TPair<uint32, FManagedRuntimeMaterialEntry>& Pair : MaterialSet)
    {
        MarkRuntimeObjectAsGarbage(Pair.Value.Asset.Get());
    }
    for (TPair<uint32, FManagedRuntimeTextureEntry>& Pair : TextureSet)
    {
        MarkRuntimeObjectAsGarbage(Pair.Value.Asset.Get());
    }

    StaticMeshSet.Empty();
    MaterialSet.Empty();
    TextureSet.Empty();
    bActive = false;
    WriteLogAsync(TEXT("AssetManageSubSystem deactivated and runtime asset registries were cleared"));
}

UStaticMesh* UAssetManageSubSystem::AcquireStaticMesh(UObject* WorldContextObject, const FName& MeshKey, UStaticMesh* RuntimeMesh)
{
    if (!bActive || !IsValid(RuntimeMesh) || RuntimeMesh->IsAsset())
    {
        return RuntimeMesh;
    }

    DeduplicateStaticMeshMaterials(WorldContextObject, RuntimeMesh);

    const uint32 Hash = HashStaticMesh(MeshKey, RuntimeMesh);
    FScopeLock Lock(&RegistryLock);

    if (FManagedRuntimeStaticMeshEntry* ExistingEntry = StaticMeshSet.Find(Hash))
    {
        if (ExistingEntry->Asset.IsValid())
        {
            ++ExistingEntry->RefCount;
            UStaticMesh* ExistingMesh = ExistingEntry->Asset.Get();
            if (ExistingMesh != RuntimeMesh)
            {
                MarkRuntimeObjectAsGarbage(RuntimeMesh);
            }
            WriteLogAsync(FString::Printf(TEXT("StaticMesh deduplicated. Key=%s Hash=%u RefCount=%d"), *MeshKey.ToString(), Hash, ExistingEntry->RefCount));
            return ExistingMesh;
        }
    }

    FManagedRuntimeStaticMeshEntry NewEntry;
    NewEntry.Asset = RuntimeMesh;
    NewEntry.RefCount = 1;
    StaticMeshSet.Add(Hash, NewEntry);
    WriteLogAsync(FString::Printf(TEXT("StaticMesh registered. Key=%s Hash=%u"), *MeshKey.ToString(), Hash));
    return RuntimeMesh;
}

void UAssetManageSubSystem::ReleaseStaticMesh(UObject* WorldContextObject, UStaticMesh* RuntimeMesh)
{
    if (!bActive || !IsValid(RuntimeMesh) || RuntimeMesh->IsAsset())
    {
        return;
    }

    FScopeLock Lock(&RegistryLock);
    for (auto It = StaticMeshSet.CreateIterator(); It; ++It)
    {
        FManagedRuntimeStaticMeshEntry& Entry = It.Value();
        if (Entry.Asset.Get() == RuntimeMesh)
        {
            Entry.RefCount = FMath::Max(0, Entry.RefCount - 1);
            WriteLogAsync(FString::Printf(TEXT("StaticMesh released. Hash=%u RefCount=%d"), It.Key(), Entry.RefCount));
            if (Entry.RefCount <= 0)
            {
                MarkRuntimeObjectAsGarbage(RuntimeMesh);
                It.RemoveCurrent();
            }
            return;
        }
    }
}

UMaterialInterface* UAssetManageSubSystem::AcquireMaterial(UObject* WorldContextObject, UMaterialInterface* RuntimeMaterial)
{
    if (!bActive || !IsValid(RuntimeMaterial) || RuntimeMaterial->IsAsset())
    {
        return RuntimeMaterial;
    }

    const uint32 Hash = HashMaterial(RuntimeMaterial);
    FScopeLock Lock(&RegistryLock);

    if (FManagedRuntimeMaterialEntry* ExistingEntry = MaterialSet.Find(Hash))
    {
        if (ExistingEntry->Asset.IsValid())
        {
            ++ExistingEntry->RefCount;
            UMaterialInterface* ExistingMaterial = ExistingEntry->Asset.Get();
            if (ExistingMaterial != RuntimeMaterial)
            {
                MarkRuntimeObjectAsGarbage(RuntimeMaterial);
            }
            return ExistingMaterial;
        }
    }

    FManagedRuntimeMaterialEntry NewEntry;
    NewEntry.Asset = RuntimeMaterial;
    NewEntry.RefCount = 1;
    MaterialSet.Add(Hash, NewEntry);
    return RuntimeMaterial;
}

void UAssetManageSubSystem::ReleaseMaterial(UObject* WorldContextObject, UMaterialInterface* RuntimeMaterial)
{
    if (!bActive || !IsValid(RuntimeMaterial) || RuntimeMaterial->IsAsset())
    {
        return;
    }

    FScopeLock Lock(&RegistryLock);
    for (auto It = MaterialSet.CreateIterator(); It; ++It)
    {
        FManagedRuntimeMaterialEntry& Entry = It.Value();
        if (Entry.Asset.Get() == RuntimeMaterial)
        {
            Entry.RefCount = FMath::Max(0, Entry.RefCount - 1);
            if (Entry.RefCount <= 0)
            {
                MarkRuntimeObjectAsGarbage(RuntimeMaterial);
                It.RemoveCurrent();
            }
            return;
        }
    }
}

UTexture* UAssetManageSubSystem::AcquireTexture(UObject* WorldContextObject, UTexture* RuntimeTexture)
{
    if (!bActive || !IsValid(RuntimeTexture) || RuntimeTexture->IsAsset())
    {
        return RuntimeTexture;
    }

    const uint32 Hash = HashTexture(RuntimeTexture);
    FScopeLock Lock(&RegistryLock);

    if (FManagedRuntimeTextureEntry* ExistingEntry = TextureSet.Find(Hash))
    {
        if (ExistingEntry->Asset.IsValid())
        {
            ++ExistingEntry->RefCount;
            UTexture* ExistingTexture = ExistingEntry->Asset.Get();
            if (ExistingTexture != RuntimeTexture)
            {
                MarkRuntimeObjectAsGarbage(RuntimeTexture);
            }
            return ExistingTexture;
        }
    }

    FManagedRuntimeTextureEntry NewEntry;
    NewEntry.Asset = RuntimeTexture;
    NewEntry.RefCount = 1;
    TextureSet.Add(Hash, NewEntry);
    return RuntimeTexture;
}

void UAssetManageSubSystem::ReleaseTexture(UObject* WorldContextObject, UTexture* RuntimeTexture)
{
    if (!bActive || !IsValid(RuntimeTexture) || RuntimeTexture->IsAsset())
    {
        return;
    }

    FScopeLock Lock(&RegistryLock);
    for (auto It = TextureSet.CreateIterator(); It; ++It)
    {
        FManagedRuntimeTextureEntry& Entry = It.Value();
        if (Entry.Asset.Get() == RuntimeTexture)
        {
            Entry.RefCount = FMath::Max(0, Entry.RefCount - 1);
            if (Entry.RefCount <= 0)
            {
                MarkRuntimeObjectAsGarbage(RuntimeTexture);
                It.RemoveCurrent();
            }
            return;
        }
    }
}

uint32 UAssetManageSubSystem::HashStaticMesh(const FName& MeshKey, const UStaticMesh* Mesh) const
{
    FString KeyString;
    if (Mesh)
    {
        KeyString += Mesh->GetBoundingBox().GetSize().ToCompactString();
        KeyString += FString::FromInt(Mesh->GetStaticMaterials().Num());
        for (int32 Index = 0; Index < Mesh->GetStaticMaterials().Num(); ++Index)
        {
            if (UMaterialInterface* Material = const_cast<UStaticMesh*>(Mesh)->GetMaterial(Index))
            {
                KeyString += Material->GetName();
            }
        }
    }
    return FCrc::StrCrc32(*KeyString);
}

uint32 UAssetManageSubSystem::HashMaterial(const UMaterialInterface* Material) const
{
    if (!Material)
    {
        return 0;
    }

    FString KeyString = Material->GetClass()->GetName();
    KeyString += TEXT("|");
    KeyString += Material->GetName();
    return FCrc::StrCrc32(*KeyString);
}

uint32 UAssetManageSubSystem::HashTexture(const UTexture* Texture) const
{
    if (!Texture)
    {
        return 0;
    }

    FString KeyString = Texture->GetClass()->GetName();
    KeyString += TEXT("|");
    KeyString += Texture->GetName();
    return FCrc::StrCrc32(*KeyString);
}

void UAssetManageSubSystem::DeduplicateStaticMeshMaterials(UObject* WorldContextObject, UStaticMesh* Mesh)
{
    if (!IsValid(Mesh))
    {
        return;
    }

    const int32 MaterialCount = Mesh->GetStaticMaterials().Num();
    for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
    {
        UMaterialInterface* Material = Mesh->GetMaterial(MaterialIndex);
        UMaterialInterface* ManagedMaterial = AcquireMaterial(WorldContextObject, Material);
        if (ManagedMaterial && ManagedMaterial != Material)
        {
            Mesh->SetMaterial(MaterialIndex, ManagedMaterial);
        }
    }
}

void UAssetManageSubSystem::WriteLogAsync(const FString& Message) const
{
    const FString Line = FString::Printf(TEXT("[%s][AssetManageSubSystem] %s"), *FDateTime::Now().ToString(), *Message);
    UFileFunctionLibrary::AppendLineToFileAsync(Line, PATH_LOG);
}
