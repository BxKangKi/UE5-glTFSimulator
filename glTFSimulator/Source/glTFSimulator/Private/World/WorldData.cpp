// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "World/WorldData.h"
#include "System/MacroLibrary.h"

namespace WorldDataJson
{
    static void SetColor(const TSharedRef<FJsonObject>& Json, const FString& FieldName, const FLinearColor& Color)
    {
        TSharedRef<FJsonObject> ColorJson = MakeShared<FJsonObject>();
        ColorJson->SetNumberField(TEXT("R"), Color.R);
        ColorJson->SetNumberField(TEXT("G"), Color.G);
        ColorJson->SetNumberField(TEXT("B"), Color.B);
        ColorJson->SetNumberField(TEXT("A"), Color.A);
        Json->SetObjectField(FieldName, ColorJson);
    }

    static void TryGetColor(const TSharedPtr<FJsonObject>& Json, const FString& FieldName, FLinearColor& OutColor)
    {
        const TSharedPtr<FJsonObject>* ColorObject = nullptr;
        if (!Json.IsValid() || !Json->TryGetObjectField(FieldName, ColorObject) || !ColorObject || !ColorObject->IsValid())
        {
            return;
        }

        double R = OutColor.R;
        double G = OutColor.G;
        double B = OutColor.B;
        double A = OutColor.A;
        (*ColorObject)->TryGetNumberField(TEXT("R"), R);
        (*ColorObject)->TryGetNumberField(TEXT("G"), G);
        (*ColorObject)->TryGetNumberField(TEXT("B"), B);
        (*ColorObject)->TryGetNumberField(TEXT("A"), A);
        OutColor = FLinearColor(R, G, B, A);
    }
}

TSharedRef<FJsonObject> FLevelCloudSettings::ToJson() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(JSON_VERSION_FIELD, JSON_SCHEMA_VERSION);
    Json->SetBoolField(TEXT("bEnabled"), bEnabled);
    Json->SetNumberField(TEXT("Coverage"), Coverage);
    Json->SetNumberField(TEXT("Density"), Density);
    Json->SetNumberField(TEXT("Opacity"), Opacity);
    Json->SetNumberField(TEXT("WindSpeed"), WindSpeed);
    WorldDataJson::SetColor(Json, TEXT("Tint"), Tint);
    return Json;
}

bool FLevelCloudSettings::FromJson(const TSharedPtr<FJsonObject>& Json)
{
    if (!Json.IsValid())
    {
        return false;
    }

    Json->TryGetBoolField(TEXT("bEnabled"), bEnabled);
    Json->TryGetNumberField(TEXT("Coverage"), Coverage);
    Json->TryGetNumberField(TEXT("Density"), Density);
    Json->TryGetNumberField(TEXT("Opacity"), Opacity);
    Json->TryGetNumberField(TEXT("WindSpeed"), WindSpeed);
    WorldDataJson::TryGetColor(Json, TEXT("Tint"), Tint);
    return true;
}

TSharedRef<FJsonObject> FLevelWeatherSettings::ToJson() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(JSON_VERSION_FIELD, JSON_SCHEMA_VERSION);
    Json->SetBoolField(TEXT("bEnabled"), bEnabled);
    Json->SetStringField(TEXT("Preset"), Preset);
    Json->SetNumberField(TEXT("Intensity"), Intensity);
    return Json;
}

bool FLevelWeatherSettings::FromJson(const TSharedPtr<FJsonObject>& Json)
{
    if (!Json.IsValid())
    {
        return false;
    }

    Json->TryGetBoolField(TEXT("bEnabled"), bEnabled);
    Json->TryGetStringField(TEXT("Preset"), Preset);
    Json->TryGetNumberField(TEXT("Intensity"), Intensity);
    return true;
}

TSharedRef<FJsonObject> FLevelGameplaySettings::ToJson() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(JSON_VERSION_FIELD, JSON_SCHEMA_VERSION);
    Json->SetStringField(TEXT("WorldGameMode"), WorldGameMode);
    Json->SetBoolField(TEXT("bCheatsEnabled"), bCheatsEnabled);
    return Json;
}

bool FLevelGameplaySettings::FromJson(const TSharedPtr<FJsonObject>& Json)
{
    if (!Json.IsValid())
    {
        return false;
    }

    Json->TryGetStringField(TEXT("WorldGameMode"), WorldGameMode);
    Json->TryGetBoolField(TEXT("bCheatsEnabled"), bCheatsEnabled);
    return true;
}

UWorldData::UWorldData()
{
    Version = JSON_SCHEMA_VERSION;
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
    Player = TEXT("Player");
}

TSharedRef<FJsonObject> UWorldData::SerializeData(UWorldData *Data)
{
    if (!Data)
    {
        Data = NewObject<UWorldData>();
    }

    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(JSON_VERSION_FIELD, !Data->Version.IsEmpty() ? Data->Version : FString(JSON_SCHEMA_VERSION));
    Json->SetStringField(LEVELNAME, Data->WorldName);
    Json->SetNumberField(LEVELTIME, Data->WorldTime);
    Json->SetNumberField(LATITUDE, Data->Latitude);
    Json->SetNumberField(LONGITUDE, Data->Longitude);
    Json->SetNumberField(AXIAL_TILT, Data->AxialTilt);
    Json->SetNumberField(ONE_YEAR_DAYS, Data->OneYearDays);
    Json->SetNumberField(ONE_DAY_TIME, Data->OneDayTime);
    Json->SetNumberField(TIME_SPEED, Data->TimeSpeed);
    Json->SetBoolField(OCEAN, Data->bOcean);

    // Deprecated legacy fields remain readable, but player state is now saved in player.json.
    Json->SetNumberField(PLAYER_X, Data->PlayerLocation.X);
    Json->SetNumberField(PLAYER_Y, Data->PlayerLocation.Y);
    Json->SetNumberField(PLAYER_Z, Data->PlayerLocation.Z);
    Json->SetStringField(PLAYER_NAME, Data->Player);

    Json->SetObjectField(TEXT("Cloud"), Data->Cloud.ToJson());
    Json->SetObjectField(TEXT("Weather"), Data->Weather.ToJson());
    Json->SetObjectField(TEXT("Gameplay"), Data->Gameplay.ToJson());
    return Json;
}

bool UWorldData::DeserializeData(UWorldData *Data, TSharedPtr<FJsonObject> Json)
{
    if (Json.IsValid())
    {
        Data->Version = JSON_SCHEMA_VERSION;
        Json->TryGetStringField(JSON_VERSION_FIELD, Data->Version);
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

        const TSharedPtr<FJsonObject>* CloudObject = nullptr;
        if (Json->TryGetObjectField(TEXT("Cloud"), CloudObject) && CloudObject && CloudObject->IsValid())
        {
            Data->Cloud.FromJson(*CloudObject);
        }

        const TSharedPtr<FJsonObject>* WeatherObject = nullptr;
        if (Json->TryGetObjectField(TEXT("Weather"), WeatherObject) && WeatherObject && WeatherObject->IsValid())
        {
            Data->Weather.FromJson(*WeatherObject);
        }

        const TSharedPtr<FJsonObject>* GameplayObject = nullptr;
        if (Json->TryGetObjectField(TEXT("Gameplay"), GameplayObject) && GameplayObject && GameplayObject->IsValid())
        {
            Data->Gameplay.FromJson(*GameplayObject);
        }
        return true;
    }
    return false;
}
