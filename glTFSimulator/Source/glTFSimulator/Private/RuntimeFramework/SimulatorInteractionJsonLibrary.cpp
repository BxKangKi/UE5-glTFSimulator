#include "RuntimeFramework/SimulatorInteractionJsonLibrary.h"

#include "JsonObjectConverter.h"

namespace
{
    constexpr int32 SupportedInteractionSchemaVersion = 1;

    void SanitizeFullBodyIK(FSimulatorFullBodyIKConfig& Config)
    {
        Config.TransformInterpSpeed = FMath::Max(0.001f, Config.TransformInterpSpeed);
        Config.AlphaInterpSpeed = FMath::Max(0.001f, Config.AlphaInterpSpeed);
        Config.MaxArmStretchRatio = FMath::Clamp(Config.MaxArmStretchRatio, 0.0f, 2.0f);
        Config.MaxSpineYawDegrees = FMath::Clamp(Config.MaxSpineYawDegrees, 0.0f, 90.0f);
        Config.MaxSpinePitchDegrees = FMath::Clamp(Config.MaxSpinePitchDegrees, 0.0f, 90.0f);
        Config.MaxTurnLeanDegrees = FMath::Clamp(Config.MaxTurnLeanDegrees, 0.0f, 45.0f);
        Config.MaxMoveLeanDegrees = FMath::Clamp(Config.MaxMoveLeanDegrees, 0.0f, 45.0f);
        Config.PelvisWeight = FMath::Clamp(Config.PelvisWeight, 0.0f, 1.0f);
        Config.SpineWeight = FMath::Clamp(Config.SpineWeight, 0.0f, 1.0f);
        Config.ShoulderWeight = FMath::Clamp(Config.ShoulderWeight, 0.0f, 1.0f);
        Config.MaxYawRateForFullLean = FMath::Max(0.0f, Config.MaxYawRateForFullLean);
    }

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

    SanitizeFullBodyIK(Parsed.FullBodyIK);
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
    SanitizeFullBodyIK(Parsed.FullBodyIK);
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
    SanitizeFullBodyIK(Sanitized.FullBodyIK);
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
    SanitizeFullBodyIK(Sanitized.FullBodyIK);
    return StructToJson(Sanitized, OutJson);
}
