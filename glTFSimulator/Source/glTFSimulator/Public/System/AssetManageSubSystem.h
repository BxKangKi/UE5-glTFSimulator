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
    TMap<uint32, FManagedStaticMeshEntry> StaticMeshSet;
    TMap<uint32, FManagedMaterialEntry> MaterialSet;
    TMap<uint32, FManagedTextureEntry> TextureSet;

    uint32 HashStaticMesh(const FName& MeshKey, const UStaticMesh* Mesh) const;
    uint32 HashMaterial(const UMaterialInterface* Material) const;
    uint32 HashTexture(const UTexture* Texture) const;
    void DeduplicateStaticMeshMaterials(UObject* WorldContextObject, UStaticMesh* Mesh);
    void WriteLogAsync(const FString& Message) const;
};
