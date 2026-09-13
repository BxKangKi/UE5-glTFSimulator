// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/ProjectConfig.h"

#include "System/SafeFileIO.h"
#include "World/WorldData.h"

FString FGlTFSimulatorProjectConfig::GetDisplayName(const FString& FolderFallback) const
{
    if (ProjectType == EGlTFSimulatorProjectType::World && !WorldName.IsEmpty())
    {
        return WorldName;
    }
    if (!ProjectName.IsEmpty())
    {
        return ProjectName;
    }
    return FolderFallback;
}

FString GlTFSimulatorProjectConfig::ToString(const EGlTFSimulatorProjectType ProjectType)
{
    switch (ProjectType)
    {
    case EGlTFSimulatorProjectType::Character:
        return TEXT("Character");
    case EGlTFSimulatorProjectType::Dynamic:
        return TEXT("Dynamic");
    case EGlTFSimulatorProjectType::World:
    default:
        return TEXT("World");
    }
}

bool GlTFSimulatorProjectConfig::TryParseType(
    const FString& Value,
    EGlTFSimulatorProjectType& OutType)
{
    FString NormalizedValue = Value;
    NormalizedValue.TrimStartAndEndInline();
    if (NormalizedValue.IsEmpty() || NormalizedValue.Equals(TEXT("World"), ESearchCase::IgnoreCase))
    {
        OutType = EGlTFSimulatorProjectType::World;
        return true;
    }
    if (NormalizedValue.Equals(TEXT("Character"), ESearchCase::IgnoreCase))
    {
        OutType = EGlTFSimulatorProjectType::Character;
        return true;
    }
    if (NormalizedValue.Equals(TEXT("Dynamic"), ESearchCase::IgnoreCase))
    {
        OutType = EGlTFSimulatorProjectType::Dynamic;
        return true;
    }
    return false;
}

bool GlTFSimulatorProjectConfig::IsExternalAssetProject(
    const EGlTFSimulatorProjectType ProjectType)
{
    return ProjectType == EGlTFSimulatorProjectType::Character
        || ProjectType == EGlTFSimulatorProjectType::Dynamic;
}

bool GlTFSimulatorProjectConfig::Parse(
    const TSharedPtr<FJsonObject>& Json,
    const FString& FolderFallback,
    FGlTFSimulatorProjectConfig& OutConfig,
    FString& OutError)
{
    OutConfig = FGlTFSimulatorProjectConfig();
    OutError.Reset();
    if (!Json.IsValid())
    {
        OutError = TEXT("Project config.json is invalid.");
        return false;
    }

    FString ProjectTypeText;
    Json->TryGetStringField(PROJECT_TYPE_FIELD, ProjectTypeText);
    if (!TryParseType(ProjectTypeText, OutConfig.ProjectType))
    {
        OutError = FString::Printf(
            TEXT("Unsupported ProjectType '%s'. Expected World, Character, or Dynamic."),
            *ProjectTypeText);
        return false;
    }

    Json->TryGetStringField(PROJECT_NAME_FIELD, OutConfig.ProjectName);
    OutConfig.ProjectName.TrimStartAndEndInline();
    if (OutConfig.ProjectName.IsEmpty())
    {
        OutConfig.ProjectName = FolderFallback.TrimStartAndEnd();
    }

    Json->TryGetStringField(CONFIG_WORLD_NAME_FIELD, OutConfig.WorldName);
    OutConfig.WorldName.TrimStartAndEndInline();
    if (OutConfig.ProjectType == EGlTFSimulatorProjectType::World && OutConfig.WorldName.IsEmpty())
    {
        OutError = TEXT("World projects require a non-empty WorldName string.");
        return false;
    }

    bool bAllowExternalAssets = false;
    if (Json->TryGetBoolField(ALLOW_EXTERNAL_ASSETS_FIELD, bAllowExternalAssets))
    {
        OutConfig.bAllowExternalAssets = bAllowExternalAssets;
    }
    if (OutConfig.ProjectType != EGlTFSimulatorProjectType::World)
    {
        // External asset packs cannot recursively opt into loading other packs.
        OutConfig.bAllowExternalAssets = false;
    }

    return true;
}

bool GlTFSimulatorProjectConfig::Load(
    const FString& ConfigPath,
    const FString& FolderFallback,
    FGlTFSimulatorProjectConfig& OutConfig,
    TSharedPtr<FJsonObject>* OutJson,
    FString& OutError)
{
    FSafeJsonLimits Limits;
    Limits.MaxFileBytes = 64ll * 1024ll * 1024ll;
    Limits.MaxDepth = 32;
    Limits.MaxValues = 131072;
    Limits.MaxContainerEntries = 65536;
    Limits.MaxStringCharacters = 32768;
    Limits.bAllowBackupRecovery = false;

    const FSafeJsonLoadResult Loaded = FSafeFileIO::LoadJsonBlocking(ConfigPath, Limits);
    if (!Loaded.IsSuccess() || !Loaded.JsonObject.IsValid())
    {
        OutError = Loaded.Error.IsEmpty()
            ? TEXT("Project config.json could not be read.")
            : Loaded.Error;
        OutConfig = FGlTFSimulatorProjectConfig();
        if (OutJson) OutJson->Reset();
        return false;
    }

    if (!Parse(Loaded.JsonObject, FolderFallback, OutConfig, OutError))
    {
        if (OutJson) *OutJson = Loaded.JsonObject;
        return false;
    }
    if (OutJson) *OutJson = Loaded.JsonObject;
    return true;
}
