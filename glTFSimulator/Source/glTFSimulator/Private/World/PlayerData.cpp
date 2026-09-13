// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file PlayerData.cpp
 * 역할: 플레이어별 저장 상태를 표현합니다.
 * 핵심 기능: 플레이어 기록 변환, entity 저장 상태 연계.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

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
