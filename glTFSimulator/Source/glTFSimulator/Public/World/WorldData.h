// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "WorldData.generated.h"

#define LEVELNAME TEXT("WorldName")
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

    /**
     * Runtime rule-mode key consumed by GameManagerSubSystem. Accepted explicit values are Creator
     * and RealLife; empty, Default, or legacy SinglePlayer uses the current map's GameManagerActor
     * default. This field does not choose Unreal's AGameModeBase.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Gameplay", meta=(DisplayName="Runtime Play Mode Key"))
    FString WorldGameMode = TEXT("Default");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Gameplay")
    bool bCheatsEnabled = false;

    /** Map-author setting. Current player health is mutable state stored in data/players.dat. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Gameplay", meta=(ClampMin="1.0"))
    float PlayerMaxHealth = 100.0f;

    /**
     * Map-authored non-ragdoll character mass in kilograms. CharacterMovement uses this value for
     * momentum, standing weight, object impacts, and the mass-aware push-force calculation.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Gameplay", meta=(ClampMin="1.0", ClampMax="10000.0"))
    float PlayerMassKg = 80.0f;

    /**
     * Effective horizontal traction used while a walking character pushes a simulated body.
     * The sustained push-force limit is PlayerMassKg * gravity * this coefficient.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Gameplay", meta=(ClampMin="0.0", ClampMax="2.0"))
    float PlayerPushTractionCoefficient = 0.30f;

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

    /** Mutable runtime time. Persisted only in data/world.dat, never in level.json. */
    UPROPERTY(Transient, BlueprintReadWrite, Category="Level|Runtime")
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

    /** Runtime compatibility value. Player transforms persist in data/players.dat. */
    UPROPERTY(Transient, BlueprintReadWrite, Category="Level|Runtime")
    FVector PlayerLocation;

    /** Selected external player asset. Persisted only in data/world.dat. */
    UPROPERTY(Transient, BlueprintReadWrite, Category="Level|Runtime")
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
