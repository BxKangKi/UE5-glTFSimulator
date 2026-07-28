// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "AssetManageSubSystem.generated.h"

class UMaterialInterface;
class UStaticMesh;
class UTexture;

struct FManagedStaticMeshKey
{
    TWeakObjectPtr<UObject> Scope;
    FName MeshKey = NAME_None;

    FManagedStaticMeshKey() = default;
    FManagedStaticMeshKey(UObject* InScope, const FName InMeshKey)
        : Scope(InScope)
        , MeshKey(InMeshKey)
    {
    }

    bool operator==(const FManagedStaticMeshKey& Other) const
    {
        return Scope == Other.Scope && MeshKey == Other.MeshKey;
    }

    friend uint32 GetTypeHash(const FManagedStaticMeshKey& Key)
    {
        return HashCombine(GetTypeHash(Key.Scope), GetTypeHash(Key.MeshKey));
    }
};

struct FManagedAssetEntryBase
{
    int32 RefCount = 0;
};

struct FManagedStaticMeshEntry : public FManagedAssetEntryBase
{
    TWeakObjectPtr<UStaticMesh> Asset;
};

struct FManagedMaterialEntry : public FManagedAssetEntryBase
{
    TWeakObjectPtr<UMaterialInterface> Asset;
};

struct FManagedTextureEntry : public FManagedAssetEntryBase
{
    TWeakObjectPtr<UTexture> Asset;
};

UCLASS()
class GLTFSIMULATOR_API UAssetManageSubSystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    static UAssetManageSubSystem* Get(UObject* WorldContextObject);

    virtual void Deinitialize() override;

    void ActivateForMainWorld(UObject* WorldContextObject);
    void DeactivateAndRelease();

    UStaticMesh* AcquireStaticMesh(UObject* WorldContextObject, const FName& MeshKey, UStaticMesh* GeneratedMeshAsset);
    void ReleaseStaticMesh(UObject* WorldContextObject, UStaticMesh* GeneratedMeshAsset);

    UMaterialInterface* AcquireMaterial(UObject* WorldContextObject, UMaterialInterface* GeneratedMaterialAsset);
    void ReleaseMaterial(UObject* WorldContextObject, UMaterialInterface* GeneratedMaterialAsset);

    UTexture* AcquireTexture(UObject* WorldContextObject, UTexture* GeneratedTextureAsset);
    void ReleaseTexture(UObject* WorldContextObject, UTexture* GeneratedTextureAsset);

    bool IsActive() const { return bActive; }

private:
    bool bActive = false;
    FCriticalSection RegistryLock;

    // Static meshes are shared only inside the same runtime owner/scope and mesh key. Materials
    // and textures are tracked by object identity, never by class/object name.
    TMap<FManagedStaticMeshKey, FManagedStaticMeshEntry> StaticMeshSet;
    TMap<TWeakObjectPtr<UMaterialInterface>, FManagedMaterialEntry> MaterialSet;
    TMap<TWeakObjectPtr<UTexture>, FManagedTextureEntry> TextureSet;

    void WriteLogAsync(const FString& Message) const;
};
