/**
 * @file InteractionJsonLibrary.cpp
 * 역할: 상호작용 설정과 JSON을 변환합니다.
 * 핵심 기능: 상호작용 데이터 직렬화·역직렬화.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "Simulator/InteractionJsonLibrary.h"

#include "JsonObjectConverter.h"

namespace
{
    constexpr int32 SupportedInteractionSchemaVersion = 1;

    bool ValidateSchemaVersion(const int32 SchemaVersion, FString& OutError)
    {
        if (SchemaVersion <= 0)
        {
            OutError = TEXT("schemaVersion must be greater than zero.");
            return false;
        }

        if (SchemaVersion > SupportedInteractionSchemaVersion)
        {
            OutError = FString::Printf(
                TEXT("Unsupported interaction schemaVersion %d. Maximum supported version is %d."),
                SchemaVersion,
                SupportedInteractionSchemaVersion);
            return false;
        }

        return true;
    }

    template <typename StructType>
    bool ParseStructJson(const FString& Json, StructType& OutStruct, FString& OutError)
    {
        OutError.Reset();
        if (Json.TrimStartAndEnd().IsEmpty())
        {
            OutError = TEXT("JSON text is empty.");
            return false;
        }

        StructType Parsed{};
        if (!FJsonObjectConverter::JsonObjectStringToUStruct(Json, &Parsed, 0, 0))
        {
            OutError = TEXT("Failed to parse interaction JSON. Check property names, enum values, and value types.");
            return false;
        }

        OutStruct = MoveTemp(Parsed);
        return true;
    }

    template <typename StructType>
    bool StructToJson(const StructType& StructValue, FString& OutJson)
    {
        OutJson.Reset();
        return FJsonObjectConverter::UStructToJsonObjectString(StructValue, OutJson, 0, 0);
    }
}

bool USimulatorInteractionJsonLibrary::ParseCharacterInteractionJson(
    const FString& Json,
    FSimulatorCharacterInteractionConfig& OutConfig,
    FString& OutError)
{
    FSimulatorCharacterInteractionConfig Parsed;
    if (!ParseStructJson(Json, Parsed, OutError))
    {
        return false;
    }

    if (!ValidateSchemaVersion(Parsed.SchemaVersion, OutError))
    {
        return false;
    }

    OutConfig = MoveTemp(Parsed);
    return true;
}

bool USimulatorInteractionJsonLibrary::ParseEquipmentInteractionJson(
    const FString& Json,
    FSimulatorEquipmentInteractionConfig& OutConfig,
    FString& OutError)
{
    FSimulatorEquipmentInteractionConfig Parsed;
    if (!ParseStructJson(Json, Parsed, OutError))
    {
        return false;
    }

    if (!ValidateSchemaVersion(Parsed.SchemaVersion, OutError))
    {
        return false;
    }

    Parsed.Sanitize();
    OutConfig = MoveTemp(Parsed);
    return true;
}

bool USimulatorInteractionJsonLibrary::CharacterInteractionToJson(
    const FSimulatorCharacterInteractionConfig& Config,
    FString& OutJson)
{
    FSimulatorCharacterInteractionConfig Sanitized = Config;
    if (Sanitized.SchemaVersion <= 0)
    {
        Sanitized.SchemaVersion = SupportedInteractionSchemaVersion;
    }
    return StructToJson(Sanitized, OutJson);
}

bool USimulatorInteractionJsonLibrary::EquipmentInteractionToJson(
    const FSimulatorEquipmentInteractionConfig& Config,
    FString& OutJson)
{
    FSimulatorEquipmentInteractionConfig Sanitized = Config;
    if (Sanitized.SchemaVersion <= 0)
    {
        Sanitized.SchemaVersion = SupportedInteractionSchemaVersion;
    }
    Sanitized.Sanitize();
    return StructToJson(Sanitized, OutJson);
}
