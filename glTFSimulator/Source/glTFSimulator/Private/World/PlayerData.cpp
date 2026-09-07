// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "World/PlayerData.h"
#include "System/MacroLibrary.h"

UPlayerData::UPlayerData()
{
    Version = JSON_SCHEMA_VERSION;
}

FWorldPlayerRecord* UPlayerData::FindPlayer(const FString& PlayerId)
{
    return Players.FindByPredicate([&PlayerId](const FWorldPlayerRecord& Record)
    {
        return Record.PlayerId.Equals(PlayerId, ESearchCase::IgnoreCase);
    });
}

const FWorldPlayerRecord* UPlayerData::FindPlayer(const FString& PlayerId) const
{
    return Players.FindByPredicate([&PlayerId](const FWorldPlayerRecord& Record)
    {
        return Record.PlayerId.Equals(PlayerId, ESearchCase::IgnoreCase);
    });
}

FWorldPlayerRecord& UPlayerData::FindOrAddPlayer(const FString& PlayerId)
{
    const FString SafeId = PlayerId.IsEmpty() ? FString(TEXT("Player")) : PlayerId;
    if (FWorldPlayerRecord* Existing = FindPlayer(SafeId))
    {
        return *Existing;
    }

    FWorldPlayerRecord& NewRecord = Players.AddDefaulted_GetRef();
    NewRecord.PlayerId = SafeId;
    NewRecord.DisplayName = SafeId;
    NewRecord.CustomJson = MakeShared<FJsonObject>();
    return NewRecord;
}
