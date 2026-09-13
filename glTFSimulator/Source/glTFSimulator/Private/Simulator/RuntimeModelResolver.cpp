// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file RuntimeModelResolver.cpp
 * 역할: 런타임 모델 참조를 활성 아카이브에서 해석합니다.
 * 핵심 기능: UUID 조회, immutable 모델 정의·리더 전달.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
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
    OutModel.ArchiveReader = Database->GetArchiveReader();
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
