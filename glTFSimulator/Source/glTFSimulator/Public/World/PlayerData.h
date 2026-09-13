// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file PlayerData.h
 * 역할: 플레이어별 저장 상태를 표현합니다.
 * 핵심 기능: 플레이어 기록 변환, entity 저장 상태 연계.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "PlayerData.generated.h"

/** Persistent per-player runtime record stored with game time in WorldName.dat. */
USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FWorldPlayerRecord
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Player")
    FString PlayerId = TEXT("Player");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Player")
    FString DisplayName = TEXT("Player");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Player")
    FVector Location = FVector::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Player")
    FRotator Rotation = FRotator::ZeroRotator;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Player")
    float Health = 100.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Player")
    int32 Level = 1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Player")
    FString PlayerGameMode = TEXT("Default");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Player")
    TArray<FString> Items;

    /** Arbitrary user/project JSON payload serialized under the Custom field. */
    TSharedPtr<FJsonObject> CustomJson;

};

UCLASS(BlueprintType)
class GLTFSIMULATOR_API UPlayerData : public UObject
{
    GENERATED_BODY()

public:
    UPlayerData();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Player")
    FString Version;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Player")
    TArray<FWorldPlayerRecord> Players;

    FWorldPlayerRecord* FindPlayer(const FString& PlayerId);
    const FWorldPlayerRecord* FindPlayer(const FString& PlayerId) const;
    FWorldPlayerRecord& FindOrAddPlayer(const FString& PlayerId);

};
