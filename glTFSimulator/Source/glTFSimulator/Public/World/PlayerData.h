// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file PlayerData.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
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
