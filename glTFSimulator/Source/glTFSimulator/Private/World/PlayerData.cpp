// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "World/PlayerData.h"
#include "System/MacroLibrary.h"

namespace PlayerDataJson
{
    static void SetVector(const TSharedRef<FJsonObject>& Json, const FString& Prefix, const FVector& Value)
    {
        Json->SetNumberField(Prefix + TEXT("X"), Value.X);
        Json->SetNumberField(Prefix + TEXT("Y"), Value.Y);
        Json->SetNumberField(Prefix + TEXT("Z"), Value.Z);
    }

    static void TryGetVector(const TSharedPtr<FJsonObject>& Json, const FString& Prefix, FVector& OutValue)
    {
        if (!Json.IsValid())
        {
            return;
        }

        double X = OutValue.X;
        double Y = OutValue.Y;
        double Z = OutValue.Z;
        Json->TryGetNumberField(Prefix + TEXT("X"), X);
        Json->TryGetNumberField(Prefix + TEXT("Y"), Y);
        Json->TryGetNumberField(Prefix + TEXT("Z"), Z);
        OutValue = FVector(X, Y, Z);
    }

    static void SetRotator(const TSharedRef<FJsonObject>& Json, const FString& Prefix, const FRotator& Value)
    {
        Json->SetNumberField(Prefix + TEXT("Pitch"), Value.Pitch);
        Json->SetNumberField(Prefix + TEXT("Yaw"), Value.Yaw);
        Json->SetNumberField(Prefix + TEXT("Roll"), Value.Roll);
    }

    static void TryGetRotator(const TSharedPtr<FJsonObject>& Json, const FString& Prefix, FRotator& OutValue)
    {
        if (!Json.IsValid())
        {
            return;
        }

        double Pitch = OutValue.Pitch;
        double Yaw = OutValue.Yaw;
        double Roll = OutValue.Roll;
        Json->TryGetNumberField(Prefix + TEXT("Pitch"), Pitch);
        Json->TryGetNumberField(Prefix + TEXT("Yaw"), Yaw);
        Json->TryGetNumberField(Prefix + TEXT("Roll"), Roll);
        OutValue = FRotator(Pitch, Yaw, Roll);
    }
}

TSharedRef<FJsonObject> FWorldPlayerRecord::ToJson() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(JSON_VERSION_FIELD, JSON_SCHEMA_VERSION);
    Json->SetStringField(TEXT("PlayerId"), PlayerId);
    Json->SetStringField(TEXT("DisplayName"), DisplayName);
    PlayerDataJson::SetVector(Json, TEXT("Location"), Location);
    PlayerDataJson::SetRotator(Json, TEXT("Rotation"), Rotation);
    Json->SetNumberField(TEXT("Health"), Health);
    Json->SetNumberField(TEXT("Level"), Level);
    Json->SetStringField(TEXT("PlayerGameMode"), PlayerGameMode);

    TArray<TSharedPtr<FJsonValue>> ItemValues;
    ItemValues.Reserve(Items.Num());
    for (const FString& Item : Items)
    {
        ItemValues.Add(MakeShared<FJsonValueString>(Item));
    }
    Json->SetArrayField(TEXT("Items"), ItemValues);

    Json->SetObjectField(TEXT("Custom"), CustomJson.IsValid() ? CustomJson.ToSharedRef() : MakeShared<FJsonObject>());
    return Json;
}

bool FWorldPlayerRecord::FromJson(const TSharedPtr<FJsonObject>& Json)
{
    if (!Json.IsValid())
    {
        return false;
    }

    Json->TryGetStringField(TEXT("PlayerId"), PlayerId);
    Json->TryGetStringField(TEXT("DisplayName"), DisplayName);
    PlayerDataJson::TryGetVector(Json, TEXT("Location"), Location);
    PlayerDataJson::TryGetRotator(Json, TEXT("Rotation"), Rotation);
    Json->TryGetNumberField(TEXT("Health"), Health);
    double LoadedLevel = static_cast<double>(Level);
    Json->TryGetNumberField(TEXT("Level"), LoadedLevel);
    Level = FMath::Max(1, FMath::RoundToInt(LoadedLevel));
    Json->TryGetStringField(TEXT("PlayerGameMode"), PlayerGameMode);

    Items.Empty();
    const TArray<TSharedPtr<FJsonValue>>* ItemValues = nullptr;
    if (Json->TryGetArrayField(TEXT("Items"), ItemValues) && ItemValues)
    {
        Items.Reserve(ItemValues->Num());
        for (const TSharedPtr<FJsonValue>& Value : *ItemValues)
        {
            if (Value.IsValid() && Value->Type == EJson::String)
            {
                Items.Add(Value->AsString());
            }
        }
    }

    const TSharedPtr<FJsonObject>* CustomObject = nullptr;
    if (Json->TryGetObjectField(TEXT("Custom"), CustomObject) && CustomObject && CustomObject->IsValid())
    {
        CustomJson = *CustomObject;
    }
    else
    {
        CustomJson = MakeShared<FJsonObject>();
    }

    if (PlayerId.IsEmpty())
    {
        PlayerId = DisplayName.IsEmpty() ? FString(TEXT("Player")) : DisplayName;
    }
    if (DisplayName.IsEmpty())
    {
        DisplayName = PlayerId;
    }
    return true;
}

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

TSharedRef<FJsonObject> UPlayerData::SerializeData(const UPlayerData* Data)
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(JSON_VERSION_FIELD, IsValid(Data) && !Data->Version.IsEmpty() ? Data->Version : FString(JSON_SCHEMA_VERSION));

    TArray<TSharedPtr<FJsonValue>> PlayerValues;
    if (IsValid(Data))
    {
        PlayerValues.Reserve(Data->Players.Num());
        for (const FWorldPlayerRecord& Record : Data->Players)
        {
            PlayerValues.Add(MakeShared<FJsonValueObject>(Record.ToJson()));
        }
    }
    Json->SetArrayField(TEXT("Players"), PlayerValues);
    return Json;
}

bool UPlayerData::DeserializeData(UPlayerData* Data, const TSharedPtr<FJsonObject>& Json)
{
    if (!IsValid(Data) || !Json.IsValid())
    {
        return false;
    }

    Data->Version = JSON_SCHEMA_VERSION;
    Json->TryGetStringField(JSON_VERSION_FIELD, Data->Version);
    Data->Players.Empty();

    const TArray<TSharedPtr<FJsonValue>>* PlayerValues = nullptr;
    if (Json->TryGetArrayField(TEXT("Players"), PlayerValues) && PlayerValues)
    {
        Data->Players.Reserve(PlayerValues->Num());
        for (const TSharedPtr<FJsonValue>& Value : *PlayerValues)
        {
            if (Value.IsValid() && Value->Type == EJson::Object)
            {
                FWorldPlayerRecord Record;
                if (Record.FromJson(Value->AsObject()))
                {
                    Data->Players.Add(MoveTemp(Record));
                }
            }
        }
    }

    return true;
}
