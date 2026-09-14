// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file RuntimeModelResolver.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Simulator/RuntimeModelResolver.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Simulator/ModelDatabaseSubsystem.h"
#include "System/WorldArchive.h"
#include "System/WorldBakedModelAsset.h"

namespace RuntimeModelResolverPrivate
{
    /**
     * Reuse one baked-model facade for every placement of the same immutable model build.
     * This avoids re-reading metadata/manifest rows and, more importantly, shares the runtime
     * material/texture caches across Static/Dynamic/Vehicle/Weapon/Character users. The resolver
     * is game-thread only, so the weak cache does not need an additional lock.
     */
    TMap<FString, TWeakObjectPtr<UWorldBakedModelAsset>>& GetFacadeCache()
    {
        static TMap<FString, TWeakObjectPtr<UWorldBakedModelAsset>> Cache;
        return Cache;
    }

    FString MakeFacadeCacheKey(const FResolvedRuntimeModel& Model)
    {
        return FString::Printf(
            TEXT("%s|%s|%s"),
            *Model.ArchiveReader->GetArchivePath(),
            *Model.ArchiveReader->GetBuildId().ToString(EGuidFormats::Digits),
            *Model.UUID.ToString(EGuidFormats::Digits));
    }

    void PruneFacadeCache(TMap<FString, TWeakObjectPtr<UWorldBakedModelAsset>>& Cache)
    {
        // Keys are tiny, but long editor sessions can rebuild many worlds. Keep the table bounded
        // without holding any UObject alive solely for this cache.
        if (Cache.Num() < 256)
        {
            return;
        }
        for (auto It = Cache.CreateIterator(); It; ++It)
        {
            if (!It.Value().IsValid())
            {
                It.RemoveCurrent();
            }
        }
    }
}

bool FResolvedRuntimeModel::IsValid() const
{
    FGuid ParsedUUID;
    return UUID.IsValid() && Definition.UUID == UUID && Definition.IsLoadable()
        && FGWorldArchive::ParseModelReference(Reference, ParsedUUID) && ParsedUUID == UUID
        && !DefinitionJson.IsEmpty() && ArchiveReader.IsValid();
}

bool FRuntimeModelResolver::Resolve(
    const UObject* WorldContextObject,
    const FString& Reference,
    FResolvedRuntimeModel& OutModel,
    FString& OutError)
{
    check(IsInGameThread());
    OutModel = FResolvedRuntimeModel();
    OutError.Reset();
    const UWorld* World = IsValid(WorldContextObject) ? WorldContextObject->GetWorld() : nullptr;
    if (!World)
    {
        OutError = TEXT("runtime model resolver has no valid UWorld context");
        return false;
    }

    const UGameInstance* GameInstance = World->GetGameInstance();
    if (!GameInstance)
    {
        OutError = TEXT("runtime model resolver has no valid UGameInstance context");
        return false;
    }

    const UModelDatabaseSubsystem* Database = GameInstance->GetSubsystem<UModelDatabaseSubsystem>();
    if (!Database)
    {
        OutError = TEXT("the model database subsystem is unavailable");
        return false;
    }
    if (!Database->IsBuiltWorld())
    {
        OutError = TEXT("a verified .gwd model database is not open");
        return false;
    }

    if (!Database->FindUUIDForReference(Reference, OutModel.UUID)
        || !Database->ResolveLoadable(
            OutModel.UUID, OutModel.Definition, OutModel.Reference)
        || !Database->LoadDefinitionDetails(
            OutModel.UUID, OutModel.Definition, OutModel.DefinitionJson, OutError))
    {
        if (OutError.IsEmpty())
        {
            OutError = FString::Printf(TEXT("built model reference is unresolved: %s"), *Reference);
        }
        OutModel = FResolvedRuntimeModel();
        return false;
    }
    OutModel.ArchiveReader = Database->GetArchiveReaderForModel(OutModel.UUID);
    if (!OutModel.IsValid())
    {
        OutError = FString::Printf(TEXT("built model record is incomplete: %s"), *Reference);
        OutModel = FResolvedRuntimeModel();
        return false;
    }
    return true;
}

UWorldBakedModelAsset* FRuntimeModelResolver::LoadAssetSynchronously(
    const FResolvedRuntimeModel& Model,
    FString& OutError)
{
    check(IsInGameThread());
    OutError.Reset();
    if (!Model.IsValid())
    {
        OutError = TEXT("cannot load an invalid built model record");
        return nullptr;
    }

    using namespace RuntimeModelResolverPrivate;
    TMap<FString, TWeakObjectPtr<UWorldBakedModelAsset>>& FacadeCache = GetFacadeCache();
    PruneFacadeCache(FacadeCache);
    const FString CacheKey = MakeFacadeCacheKey(Model);
    if (TWeakObjectPtr<UWorldBakedModelAsset>* Cached = FacadeCache.Find(CacheKey))
    {
        if (UWorldBakedModelAsset* Existing = Cached->Get(); IsValid(Existing))
        {
            return Existing;
        }
        FacadeCache.Remove(CacheKey);
    }

    UWorldBakedModelAsset* Asset = NewObject<UWorldBakedModelAsset>(
        GetTransientPackage(), NAME_None, RF_Transient);
    if (!IsValid(Asset) || !Asset->Initialize(Model, OutError))
    {
        if (OutError.IsEmpty()) OutError = TEXT("could not initialize the baked model facade");
        return nullptr;
    }
    FacadeCache.Add(CacheKey, Asset);
    return Asset;
}
