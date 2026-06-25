// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

struct FJsonHelper
{
    static TArray<FString> GetAllKeysFromJsonObject(const TSharedPtr<FJsonObject> &JsonObject);

    // 1. FVector -> JSON Object에 주입
    static void SetVector(const TSharedRef<FJsonObject> &Json, const FVector &Vector, const FString &KeyPrefix = TEXT(""));

    // 2. JSON Object -> FVector 추출
    static void TryGetVector(const TSharedPtr<FJsonObject> &Json, FVector &OutVector, const FString &KeyPrefix = TEXT(""));

    // 3. TArray 내 아이템들을 JSON 배열로 직렬화 (템플릿 + 레퍼런스/포인터 전달용 람다)
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

    // 4. JSON 배열을 TArray 객체로 역직렬화
    template<typename T>
    static void TryGetArray(const TSharedPtr<FJsonObject>& Json, const FString& Key, TArray<T>& OutArray, TFunctionRef<bool(const TSharedPtr<FJsonObject>&, T&)> DeserializeFunc)
    {
        if (!Json.IsValid()) return;

        const TArray<TSharedPtr<FJsonValue>>* JsonArrayPtr = nullptr;
        if (Json->TryGetArrayField(Key, JsonArrayPtr) && JsonArrayPtr)
        {
            OutArray.Empty(JsonArrayPtr->Num()); // 메모리 미리 할당 최적화
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

        // 5. TMap<FName, T> -> JSON Object 직렬화
    template<typename T>
    static void SetMap(const TSharedRef<FJsonObject>& Json, const FString& Key, const TMap<FName, T>& Map, TFunctionRef<TSharedRef<FJsonObject>(const T&)> SerializeFunc)
    {
        TSharedRef<FJsonObject> MapJsonObj = MakeShared<FJsonObject>();
        
        for (const TPair<FName, T>& KVP : Map)
        {
            // FName을 String Key로 변환하여 오브젝트에 주입
            MapJsonObj->SetObjectField(KVP.Key.ToString(), SerializeFunc(KVP.Value));
        }
        
        Json->SetObjectField(Key, MapJsonObj);
    }

    // 6. JSON Object -> TMap<FName, T> 역직렬화
    template<typename T>
    static void TryGetMap(const TSharedPtr<FJsonObject>& Json, const FString& Key, TMap<FName, T>& OutMap, TFunctionRef<bool(const TSharedPtr<FJsonObject>&, T&)> DeserializeFunc)
    {
        if (!Json.IsValid()) return;

        const TSharedPtr<FJsonObject>* MapJsonObjPtr = nullptr;
        if (Json->TryGetObjectField(Key, MapJsonObjPtr) && MapJsonObjPtr && MapJsonObjPtr->IsValid())
        {
            OutMap.Empty((*MapJsonObjPtr)->Values.Num()); // 메모리 예약 최적화
            
            // JSON 내부의 모든 Key-Value 쌍을 순회
            for (const TPair<FString, TSharedPtr<FJsonValue>>& KVP : (*MapJsonObjPtr)->Values)
            {
                if (KVP.Value.IsValid() && KVP.Value->Type == EJson::Object)
                {
                    T Item;
                    if (DeserializeFunc(KVP.Value->AsObject(), Item))
                    {
                        // String Key를 FName으로 되돌려 TMap에 추가
                        OutMap.Add(FName(*KVP.Key), Item);
                    }
                }
            }
        }
    }

    /**
     * Enum 값을 JSON 오브젝트에 문자열로 저장합니다.
     * @param JsonObject 저장할 대상 JSON 오브젝트
     * @param FieldName JSON 키값
     * @param Value 저장할 Enum 값
     */
    template <typename TEnum>
    static void SetEnumField(TSharedPtr<FJsonObject> JsonObject, const FString &FieldName, TEnum Value)
    {
        if (!JsonObject.IsValid())
            return;

        const UEnum *EnumPtr = StaticEnum<TEnum>();
        if (EnumPtr)
        {
            // GetNameStringByValue는 "EMyEnum::Value"에서 앞부분을 뗀 "Value" 문자열만 반환합니다.
            FString EnumString = EnumPtr->GetNameStringByValue(static_cast<int64>(Value));
            JsonObject->SetStringField(FieldName, EnumString);
        }
    }

    /**
     * JSON 오브젝트에서 문자열을 읽어 Enum 값으로 변환합니다.
     * @param JsonObject 읽어올 대상 JSON 오브젝트
     * @param FieldName JSON 키값
     * @param OutValue 변환된 Enum 값을 받아올 참조 변수
     * @return 성공 여부 (필드가 없거나 변환 실패 시 false)
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
                // 완전한 형태(EItemType::Weapon)와 짧은 형태(Weapon) 모두 검색하기 위해 FName으로 컨버팅
                int64 Value = EnumPtr->GetValueByName(FName(*StringValue));

                // 찾지 못했다면 범위 내에서 네임스페이스를 붙여서 재시도
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