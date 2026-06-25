// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "World/WorldData.h"

UWorldData::UWorldData()
{
    WorldName = TEXT("New World");
    WorldTime = 0.0f;
    Latitude = 38.0f;
    Longitude = 127.0f;
    AxialTilt = 23.5f;
    OneYearDays = 365.0f;
    OneDayTime = 24.0f * 60.0f * 60.0f;
    TimeSpeed = 60.0f;
    bOcean = false;
    PlayerLocation = FVector::ZeroVector;
    Player = TEXT("");
}

TSharedRef<FJsonObject> UWorldData::SerializeData(UWorldData *Data)
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(LEVELNAME, Data->WorldName);
    Json->SetNumberField(LEVELTIME, Data->WorldTime);
    Json->SetNumberField(LATITUDE, Data->Latitude);
    Json->SetNumberField(LONGITUDE, Data->Longitude);
    Json->SetNumberField(AXIAL_TILT, Data->AxialTilt);
    Json->SetNumberField(ONE_YEAR_DAYS, Data->OneYearDays);
    Json->SetNumberField(ONE_DAY_TIME, Data->OneDayTime);
    Json->SetNumberField(TIME_SPEED, Data->TimeSpeed);
    Json->SetNumberField(PLAYER_X, Data->PlayerLocation.X);
    Json->SetNumberField(PLAYER_Y, Data->PlayerLocation.Y);
    Json->SetNumberField(PLAYER_Z, Data->PlayerLocation.Z);
    Json->SetBoolField(OCEAN, Data->bOcean);
    Json->SetStringField(PLAYER_NAME, Data->Player);
    return Json;
}

bool UWorldData::DeserializeData(UWorldData *Data, TSharedPtr<FJsonObject> Json)
{
    if (Json.IsValid())
    {
        Json->TryGetStringField(LEVELNAME, Data->WorldName);
        Json->TryGetNumberField(LEVELTIME, Data->WorldTime);
        Json->TryGetNumberField(LATITUDE, Data->Latitude);
        Json->TryGetNumberField(LONGITUDE, Data->Longitude);
        Json->TryGetNumberField(AXIAL_TILT, Data->AxialTilt);
        Json->TryGetNumberField(ONE_YEAR_DAYS, Data->OneYearDays);
        Json->TryGetNumberField(ONE_DAY_TIME, Data->OneDayTime);
        Json->TryGetNumberField(TIME_SPEED, Data->TimeSpeed);
        Json->TryGetNumberField(PLAYER_X, Data->PlayerLocation.X);
        Json->TryGetNumberField(PLAYER_Y, Data->PlayerLocation.Y);
        Json->TryGetNumberField(PLAYER_Z, Data->PlayerLocation.Z);
        Json->TryGetBoolField(OCEAN, Data->bOcean);
        Json->TryGetStringField(PLAYER_NAME, Data->Player);
        return true;
    }
    return false;
}