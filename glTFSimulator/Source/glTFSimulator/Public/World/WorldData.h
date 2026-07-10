// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "WorldData.generated.h"

#define LEVELNAME TEXT("WorldName")
#define LEVELTIME TEXT("WorldTime")
#define LATITUDE TEXT("Latitude")
#define LONGITUDE TEXT("Longitude")
#define AXIAL_TILT TEXT("AxialTilt")
#define ONE_YEAR_DAYS TEXT("OneYearDays")
#define ONE_DAY_TIME TEXT("OneDayTime")
#define TIME_SPEED TEXT("TimeSpeed")
#define OCEAN TEXT("bOcean")
#define PLAYER_X TEXT("X")
#define PLAYER_Y TEXT("Y")
#define PLAYER_Z TEXT("Z")
#define PLAYER_NAME TEXT("Player")
#define LEVEL_FILE_NAME TEXT("/level.json")

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FLevelCloudSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Cloud")
    bool bEnabled = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Cloud")
    float Coverage = 0.55f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Cloud")
    float Density = 0.70f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Cloud")
    float Opacity = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Cloud")
    float WindSpeed = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Cloud")
    FLinearColor Tint = FLinearColor::White;

    TSharedRef<FJsonObject> ToJson() const;
    bool FromJson(const TSharedPtr<FJsonObject>& Json);
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FLevelWeatherSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Weather")
    bool bEnabled = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Weather")
    FString Preset = TEXT("Rain");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Weather")
    float Intensity = 1.0f;

    TSharedRef<FJsonObject> ToJson() const;
    bool FromJson(const TSharedPtr<FJsonObject>& Json);
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FLevelGameplaySettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Gameplay")
    FString WorldGameMode = TEXT("SinglePlayer");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Gameplay")
    bool bCheatsEnabled = false;

    TSharedRef<FJsonObject> ToJson() const;
    bool FromJson(const TSharedPtr<FJsonObject>& Json);
};

UCLASS(BlueprintType)
class GLTFSIMULATOR_API UWorldData : public UObject
{
    GENERATED_BODY()

public:
    UWorldData();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level")
    FString Version;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level")
    FString WorldName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level")
    float WorldTime;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level")
    float Latitude;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level")
    float Longitude;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level")
    float AxialTilt;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level")
    float OneYearDays;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level")
    float OneDayTime;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level")
    float TimeSpeed;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level")
    bool bOcean;

    /** Legacy field kept for old level.json files. New saves use player.json. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Legacy")
    FVector PlayerLocation;

    /** Legacy field kept for old level.json files. New saves use player.json. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Legacy")
    FString Player;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Cloud")
    FLevelCloudSettings Cloud;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Weather")
    FLevelWeatherSettings Weather;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Gameplay")
    FLevelGameplaySettings Gameplay;

    static TSharedRef<FJsonObject> SerializeData(UWorldData *Data);
    static bool DeserializeData(UWorldData *Data, TSharedPtr<FJsonObject> Json);
};
