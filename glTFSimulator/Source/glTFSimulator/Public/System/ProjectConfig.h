// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file ProjectConfig.h
 * Shared project-authoring metadata used by the Projects UI, build pipeline, and external asset loader.
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "ProjectConfig.generated.h"

#define PROJECT_TYPE_FIELD TEXT("ProjectType")
#define PROJECT_NAME_FIELD TEXT("ProjectName")
#define ALLOW_EXTERNAL_ASSETS_FIELD TEXT("bAllowExternalAssets")

UENUM(BlueprintType)
enum class EGlTFSimulatorProjectType : uint8
{
    World UMETA(DisplayName="World"),
    Character UMETA(DisplayName="Character"),
    Dynamic UMETA(DisplayName="Dynamic")
};

/** Lightweight validated row shown by the Projects UI. */
USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FGlTFSimulatorProjectSummary
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Project")
    FString FolderName;

    UPROPERTY(BlueprintReadOnly, Category="Project")
    FString DisplayName;

    UPROPERTY(BlueprintReadOnly, Category="Project")
    EGlTFSimulatorProjectType ProjectType = EGlTFSimulatorProjectType::World;

    UPROPERTY(BlueprintReadOnly, Category="Project")
    bool bAllowExternalAssets = false;
};

/** Parsed, validated view of one Projects/<Project>/config.json file. */
USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FGlTFSimulatorProjectConfig
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Project")
    EGlTFSimulatorProjectType ProjectType = EGlTFSimulatorProjectType::World;

    /** Human-readable project name. Falls back to the project folder name when omitted. */
    UPROPERTY(BlueprintReadOnly, Category="Project")
    FString ProjectName;

    /** World archive name. Defaults to the Projects/<folder> name when omitted. */
    UPROPERTY(BlueprintReadOnly, Category="Project")
    FString WorldName;

    /** World-only permission gate for loading .gasset archives from glTFSimulator/Resources. */
    UPROPERTY(BlueprintReadOnly, Category="Project")
    bool bAllowExternalAssets = false;

    FString GetDisplayName(const FString& FolderFallback = FString()) const;
};

namespace GlTFSimulatorProjectConfig
{
    /** ProjectType defaults to World when omitted. */
    GLTFSIMULATOR_API bool Parse(
        const TSharedPtr<FJsonObject>& Json,
        const FString& FolderFallback,
        FGlTFSimulatorProjectConfig& OutConfig,
        FString& OutError);

    GLTFSIMULATOR_API bool Load(
        const FString& ConfigPath,
        const FString& FolderFallback,
        FGlTFSimulatorProjectConfig& OutConfig,
        TSharedPtr<FJsonObject>* OutJson,
        FString& OutError);

    GLTFSIMULATOR_API FString ToString(EGlTFSimulatorProjectType ProjectType);
    GLTFSIMULATOR_API bool TryParseType(const FString& Value, EGlTFSimulatorProjectType& OutType);
    GLTFSIMULATOR_API bool IsExternalAssetProject(EGlTFSimulatorProjectType ProjectType);
}
