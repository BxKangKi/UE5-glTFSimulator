// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

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

UCLASS(BlueprintType)
class GLTFSIMULATOR_API UWorldData : public UObject
{
    GENERATED_BODY()
    
public:
    UWorldData();
    FString WorldName;
    float WorldTime;
    float Latitude;
    float Longitude;
    float AxialTilt;
    float OneYearDays;
    float OneDayTime;
    float TimeSpeed;
    bool bOcean;
    FVector PlayerLocation;
    FString Player;
    static TSharedRef<FJsonObject> SerializeData(UWorldData *Data);
    static bool DeserializeData(UWorldData *Data, TSharedPtr<FJsonObject> Json);
};