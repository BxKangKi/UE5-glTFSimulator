// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldData.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "WorldData.generated.h"

#define CONFIG_WORLD_NAME_FIELD TEXT("WorldName")
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
#define LEVEL_FILE_NAME TEXT("config.json")

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

    /** Real seconds per simulation weather tick. This is the Minecraft-style discrete weather clock. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Weather", meta=(ClampMin="0.05", Units="s"))
    float TickIntervalSeconds = 1.0f;

    /** Automatically choose clear/rain/snow again when the current weather duration expires. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Weather")
    bool bAutoCycle = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Weather", meta=(ClampMin="1"))
    int32 MinDurationTicks = 300;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Weather", meta=(ClampMin="1"))
    int32 MaxDurationTicks = 1200;

    /** Relative weights used only when bAutoCycle is true. They do not need to sum to 1. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Weather", meta=(ClampMin="0.0"))
    float ClearWeight = 0.55f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Weather", meta=(ClampMin="0.0"))
    float RainWeight = 0.35f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Weather", meta=(ClampMin="0.0"))
    float SnowWeight = 0.10f;

    TSharedRef<FJsonObject> ToJson() const;
    bool FromJson(const TSharedPtr<FJsonObject>& Json);
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FLevelGameplaySettings
{
    GENERATED_BODY()

    /**
     * Runtime rule-mode key consumed by GameManagerSubSystem. Accepted values are Default, Creator,
     * and RealLife. This field does not choose Unreal's AGameModeBase.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Gameplay", meta=(DisplayName="Runtime Play Mode Key"))
    FString WorldGameMode = TEXT("Default");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Gameplay")
    bool bCheatsEnabled = false;

    /** Map-author setting. Current player health is mutable state stored in WorldName.dat. */
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

    /** Mutable runtime time. Persisted only in WorldName.dat, never in config.json. */
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

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Water")
    bool bOcean;

    /** Fixed global-ocean surface height. The ocean follows the local camera in XY only. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level|Water", meta=(EditCondition="bOcean", Units="cm"))
    double OceanHeightCm = 0.0;

    /** Allows this world to discover and mount Character/Dynamic .gasset packs from glTFSimulator/Resources. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Level")
    bool bAllowExternalAssets = false;

    /** Runtime player location mirror. All player transforms persist in WorldName.dat. */
    UPROPERTY(Transient, BlueprintReadWrite, Category="Level|Runtime")
    FVector PlayerLocation;

    /** Selected built character UUID. Persisted only in WorldName.dat. */
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
