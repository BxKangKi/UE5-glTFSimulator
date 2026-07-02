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
    static void MarkGeneratedObjectAsGarbage(TObjectType* Object)
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

    for (TPair<uint32, FManagedStaticMeshEntry>& Pair : StaticMeshSet)
    {
        MarkGeneratedObjectAsGarbage(Pair.Value.Asset.Get());
    }
    for (TPair<uint32, FManagedMaterialEntry>& Pair : MaterialSet)
    {
        MarkGeneratedObjectAsGarbage(Pair.Value.Asset.Get());
    }
    for (TPair<uint32, FManagedTextureEntry>& Pair : TextureSet)
    {
        MarkGeneratedObjectAsGarbage(Pair.Value.Asset.Get());
    }

    StaticMeshSet.Empty();
    MaterialSet.Empty();
    TextureSet.Empty();
    bActive = false;
    WriteLogAsync(TEXT("AssetManageSubSystem deactivated and runtime asset registries were cleared"));
}

UStaticMesh* UAssetManageSubSystem::AcquireStaticMesh(UObject* WorldContextObject, const FName& MeshKey, UStaticMesh* GeneratedMeshAsset)
{
    if (!bActive || !IsValid(GeneratedMeshAsset) || GeneratedMeshAsset->IsAsset())
    {
        return GeneratedMeshAsset;
    }

    DeduplicateStaticMeshMaterials(WorldContextObject, GeneratedMeshAsset);

    const uint32 Hash = HashStaticMesh(MeshKey, GeneratedMeshAsset);
    FScopeLock Lock(&RegistryLock);

    if (FManagedStaticMeshEntry* ExistingEntry = StaticMeshSet.Find(Hash))
    {
        if (ExistingEntry->Asset.IsValid())
        {
            ++ExistingEntry->RefCount;
            UStaticMesh* ExistingMesh = ExistingEntry->Asset.Get();
            if (ExistingMesh != GeneratedMeshAsset)
            {
                MarkGeneratedObjectAsGarbage(GeneratedMeshAsset);
            }
            WriteLogAsync(FString::Printf(TEXT("StaticMesh deduplicated. Key=%s Hash=%u RefCount=%d"), *MeshKey.ToString(), Hash, ExistingEntry->RefCount));
            return ExistingMesh;
        }
    }

    FManagedStaticMeshEntry NewEntry;
    NewEntry.Asset = GeneratedMeshAsset;
    NewEntry.RefCount = 1;
    StaticMeshSet.Add(Hash, NewEntry);
    WriteLogAsync(FString::Printf(TEXT("StaticMesh registered. Key=%s Hash=%u"), *MeshKey.ToString(), Hash));
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
            WriteLogAsync(FString::Printf(TEXT("StaticMesh released. Hash=%u RefCount=%d"), It.Key(), Entry.RefCount));
            if (Entry.RefCount <= 0)
            {
                MarkGeneratedObjectAsGarbage(GeneratedMeshAsset);
                It.RemoveCurrent();
            }
            return;
        }
    }
}

UMaterialInterface* UAssetManageSubSystem::AcquireMaterial(UObject* WorldContextObject, UMaterialInterface* GeneratedMaterialAsset)
{
    if (!bActive || !IsValid(GeneratedMaterialAsset) || GeneratedMaterialAsset->IsAsset())
    {
        return GeneratedMaterialAsset;
    }

    const uint32 Hash = HashMaterial(GeneratedMaterialAsset);
    FScopeLock Lock(&RegistryLock);

    if (FManagedMaterialEntry* ExistingEntry = MaterialSet.Find(Hash))
    {
        if (ExistingEntry->Asset.IsValid())
        {
            ++ExistingEntry->RefCount;
            UMaterialInterface* ExistingMaterial = ExistingEntry->Asset.Get();
            if (ExistingMaterial != GeneratedMaterialAsset)
            {
                MarkGeneratedObjectAsGarbage(GeneratedMaterialAsset);
            }
            return ExistingMaterial;
        }
    }

    FManagedMaterialEntry NewEntry;
    NewEntry.Asset = GeneratedMaterialAsset;
    NewEntry.RefCount = 1;
    MaterialSet.Add(Hash, NewEntry);
    return GeneratedMaterialAsset;
}

void UAssetManageSubSystem::ReleaseMaterial(UObject* WorldContextObject, UMaterialInterface* GeneratedMaterialAsset)
{
    if (!bActive || !IsValid(GeneratedMaterialAsset) || GeneratedMaterialAsset->IsAsset())
    {
        return;
    }

    FScopeLock Lock(&RegistryLock);
    for (auto It = MaterialSet.CreateIterator(); It; ++It)
    {
        FManagedMaterialEntry& Entry = It.Value();
        if (Entry.Asset.Get() == GeneratedMaterialAsset)
        {
            Entry.RefCount = FMath::Max(0, Entry.RefCount - 1);
            if (Entry.RefCount <= 0)
            {
                MarkGeneratedObjectAsGarbage(GeneratedMaterialAsset);
                It.RemoveCurrent();
            }
            return;
        }
    }
}

UTexture* UAssetManageSubSystem::AcquireTexture(UObject* WorldContextObject, UTexture* GeneratedTextureAsset)
{
    if (!bActive || !IsValid(GeneratedTextureAsset) || GeneratedTextureAsset->IsAsset())
    {
        return GeneratedTextureAsset;
    }

    const uint32 Hash = HashTexture(GeneratedTextureAsset);
    FScopeLock Lock(&RegistryLock);

    if (FManagedTextureEntry* ExistingEntry = TextureSet.Find(Hash))
    {
        if (ExistingEntry->Asset.IsValid())
        {
            ++ExistingEntry->RefCount;
            UTexture* ExistingTexture = ExistingEntry->Asset.Get();
            if (ExistingTexture != GeneratedTextureAsset)
            {
                MarkGeneratedObjectAsGarbage(GeneratedTextureAsset);
            }
            return ExistingTexture;
        }
    }

    FManagedTextureEntry NewEntry;
    NewEntry.Asset = GeneratedTextureAsset;
    NewEntry.RefCount = 1;
    TextureSet.Add(Hash, NewEntry);
    return GeneratedTextureAsset;
}

void UAssetManageSubSystem::ReleaseTexture(UObject* WorldContextObject, UTexture* GeneratedTextureAsset)
{
    if (!bActive || !IsValid(GeneratedTextureAsset) || GeneratedTextureAsset->IsAsset())
    {
        return;
    }

    FScopeLock Lock(&RegistryLock);
    for (auto It = TextureSet.CreateIterator(); It; ++It)
    {
        FManagedTextureEntry& Entry = It.Value();
        if (Entry.Asset.Get() == GeneratedTextureAsset)
        {
            Entry.RefCount = FMath::Max(0, Entry.RefCount - 1);
            if (Entry.RefCount <= 0)
            {
                MarkGeneratedObjectAsGarbage(GeneratedTextureAsset);
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
    UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("AssetManageSubSystem"), Message);
}
