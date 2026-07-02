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

struct FManagedRuntimeAssetEntryBase
{
    int32 RefCount = 0;
};

struct FManagedRuntimeStaticMeshEntry : public FManagedRuntimeAssetEntryBase
{
    TWeakObjectPtr<UStaticMesh> Asset;
};

struct FManagedRuntimeMaterialEntry : public FManagedRuntimeAssetEntryBase
{
    TWeakObjectPtr<UMaterialInterface> Asset;
};

struct FManagedRuntimeTextureEntry : public FManagedRuntimeAssetEntryBase
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

    UStaticMesh* AcquireStaticMesh(UObject* WorldContextObject, const FName& MeshKey, UStaticMesh* RuntimeMesh);
    void ReleaseStaticMesh(UObject* WorldContextObject, UStaticMesh* RuntimeMesh);

    UMaterialInterface* AcquireMaterial(UObject* WorldContextObject, UMaterialInterface* RuntimeMaterial);
    void ReleaseMaterial(UObject* WorldContextObject, UMaterialInterface* RuntimeMaterial);

    UTexture* AcquireTexture(UObject* WorldContextObject, UTexture* RuntimeTexture);
    void ReleaseTexture(UObject* WorldContextObject, UTexture* RuntimeTexture);

    bool IsActive() const { return bActive; }

private:
    bool bActive = false;
    FCriticalSection RegistryLock;
    TMap<uint32, FManagedRuntimeStaticMeshEntry> StaticMeshSet;
    TMap<uint32, FManagedRuntimeMaterialEntry> MaterialSet;
    TMap<uint32, FManagedRuntimeTextureEntry> TextureSet;

    uint32 HashStaticMesh(const FName& MeshKey, const UStaticMesh* Mesh) const;
    uint32 HashMaterial(const UMaterialInterface* Material) const;
    uint32 HashTexture(const UTexture* Texture) const;
    void DeduplicateStaticMeshMaterials(UObject* WorldContextObject, UStaticMesh* Mesh);
    void WriteLogAsync(const FString& Message) const;
};
