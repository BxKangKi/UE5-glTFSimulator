// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

struct FJsonHelper
{
    static TArray<FString> GetAllKeysFromJsonObject(const TSharedPtr<FJsonObject> &JsonObject);

    // 1. Write an FVector into a JSON object.
    static void SetVector(const TSharedRef<FJsonObject> &Json, const FVector &Vector, const FString &KeyPrefix = TEXT(""));

    // 2. Read an FVector from a JSON object.
    static void TryGetVector(const TSharedPtr<FJsonObject> &Json, FVector &OutVector, const FString &KeyPrefix = TEXT(""));

    // 3. Serialize TArray items into a JSON array using template/lambda helpers.
    template<typename T>
    static void SetArray(const TSharedRef<FJsonObject>& Json, const FString& Key, const TArray<T>& Array, TFunctionRef<TSharedRef<FJsonObject>(const T&)> SerializeFunc)
    {
        TArray<TSharedPtr<FJsonValue>> JsonArray;
        for (const T& Item : Array)
        {
            JsonArray.Add(MakeShared<FJsonValueObject>(SerializeFunc(Item)));
        }
        Json->SetArrayField(Key, JsonArray);
    }

    // 4. Deserialize a JSON array into a TArray.
    template<typename T>
    static void TryGetArray(const TSharedPtr<FJsonObject>& Json, const FString& Key, TArray<T>& OutArray, TFunctionRef<bool(const TSharedPtr<FJsonObject>&, T&)> DeserializeFunc)
    {
        if (!Json.IsValid()) return;

        const TArray<TSharedPtr<FJsonValue>>* JsonArrayPtr = nullptr;
        if (Json->TryGetArrayField(Key, JsonArrayPtr) && JsonArrayPtr)
        {
            OutArray.Empty(JsonArrayPtr->Num()); // Reserve memory up front for efficiency.
            for (const TSharedPtr<FJsonValue>& Value : *JsonArrayPtr)
            {
                if (Value.IsValid() && Value->Type == EJson::Object)
                {
                    T Item;
                    if (DeserializeFunc(Value->AsObject(), Item))
                    {
                        OutArray.Add(Item);
                    }
                }
            }
        }
    }

        // 5. Serialize TMap<FName, T> into a JSON object.
    template<typename T>
    static void SetMap(const TSharedRef<FJsonObject>& Json, const FString& Key, const TMap<FName, T>& Map, TFunctionRef<TSharedRef<FJsonObject>(const T&)> SerializeFunc)
    {
        TSharedRef<FJsonObject> MapJsonObj = MakeShared<FJsonObject>();
        
        for (const TPair<FName, T>& KVP : Map)
        {
            // Convert FName to a string key before writing it into the object.
            MapJsonObj->SetObjectField(KVP.Key.ToString(), SerializeFunc(KVP.Value));
        }
        
        Json->SetObjectField(Key, MapJsonObj);
    }

    // 6. Deserialize a JSON object into TMap<FName, T>.
    template<typename T>
    static void TryGetMap(const TSharedPtr<FJsonObject>& Json, const FString& Key, TMap<FName, T>& OutMap, TFunctionRef<bool(const TSharedPtr<FJsonObject>&, T&)> DeserializeFunc)
    {
        if (!Json.IsValid()) return;

        const TSharedPtr<FJsonObject>* MapJsonObjPtr = nullptr;
        if (Json->TryGetObjectField(Key, MapJsonObjPtr) && MapJsonObjPtr && MapJsonObjPtr->IsValid())
        {
            OutMap.Empty((*MapJsonObjPtr)->Values.Num()); // Reserve map storage for efficiency.
            
            // Iterate over every key-value pair in the JSON object.
            for (const auto& KVP : (*MapJsonObjPtr)->Values)
            {
                if (KVP.Value.IsValid() && KVP.Value->Type == EJson::Object)
                {
                    T Item;
                    if (DeserializeFunc(KVP.Value->AsObject(), Item))
                    {
                        // Convert the string key back to FName before adding it to the map.
                        OutMap.Add(FName(*FString(KVP.Key)), Item);
                    }
                }
            }
        }
    }

    /**
     * Stores an enum value in a JSON object as a string.
     * @param JsonObject Target JSON object to write to.
     * @param FieldName JSON key name.
     * @param Value Enum value to store.
     */
    template <typename TEnum>
    static void SetEnumField(TSharedPtr<FJsonObject> JsonObject, const FString &FieldName, TEnum Value)
    {
        if (!JsonObject.IsValid())
            return;

        const UEnum *EnumPtr = StaticEnum<TEnum>();
        if (EnumPtr)
        {
            // GetNameStringByValue returns only "Value" instead of the fully qualified "EMyEnum::Value".
            FString EnumString = EnumPtr->GetNameStringByValue(static_cast<int64>(Value));
            JsonObject->SetStringField(FieldName, EnumString);
        }
    }

    /**
     * Reads a string from a JSON object and converts it to an enum value.
     * @param JsonObject Source JSON object to read from.
     * @param FieldName JSON key name.
     * @param OutValue Output reference that receives the converted enum value.
     * @return True on success; false if the field is missing or conversion fails.
     */
    template <typename TEnum>
    static bool TryGetEnumField(const TSharedPtr<FJsonObject> &JsonObject, const FString &FieldName, TEnum &OutValue)
    {
        if (!JsonObject.IsValid() || !JsonObject->HasField(FieldName))
        {
            return false;
        }

        FString StringValue;
        if (JsonObject->TryGetStringField(FieldName, StringValue))
        {
            const UEnum *EnumPtr = StaticEnum<TEnum>();
            if (EnumPtr)
            {
                // Convert to FName so both full and short enum forms can be searched.
                int64 Value = EnumPtr->GetValueByName(FName(*StringValue));

                // If the short form is not found, retry with scoped enum names.
                if (Value == INDEX_NONE)
                {
                    FString FullEnumName = FString::Printf(TEXT("%s::%s"), *EnumPtr->GetName(), *StringValue);
                    Value = EnumPtr->GetValueByName(FName(*FullEnumName));
                }

                if (Value != INDEX_NONE)
                {
                    OutValue = static_cast<TEnum>(Value);
                    return true;
                }
            }
        }
        return false;
    }
};