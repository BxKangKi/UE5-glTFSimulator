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
    const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
    const UModelDatabaseSubsystem* Database = GameInstance
        ? GameInstance->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    if (!Database || !Database->IsBuiltWorld())
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

    UWorldBakedModelAsset* Asset = NewObject<UWorldBakedModelAsset>(
        GetTransientPackage(), NAME_None, RF_Transient);
    if (!IsValid(Asset) || !Asset->Initialize(Model, OutError))
    {
        if (OutError.IsEmpty()) OutError = TEXT("could not initialize the baked model facade");
        return nullptr;
    }
    return Asset;
}
