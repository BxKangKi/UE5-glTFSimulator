// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "PlayerData.generated.h"

#define LEGACY_PLAYER_FILE_NAME TEXT("/player.json")
#define LEGACY_PLAYERS_FILE_NAME TEXT("/players.json")

/** Persistent per-player runtime record stored in data/players.dat. */
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

    TSharedRef<FJsonObject> ToJson() const;
    bool FromJson(const TSharedPtr<FJsonObject>& Json);
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

    /** Legacy migration helpers. Runtime saves never write player JSON. */
    static TSharedRef<FJsonObject> SerializeData(const UPlayerData* Data);
    static bool DeserializeData(UPlayerData* Data, const TSharedPtr<FJsonObject>& Json);
};
