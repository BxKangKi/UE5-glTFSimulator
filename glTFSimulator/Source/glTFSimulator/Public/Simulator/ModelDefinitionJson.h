/**
 * @file ModelDefinitionJson.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"

enum class EModelDefinitionType : uint8
{
    Invalid,
    Static,
    Dynamic,
    Character
};

enum class EModelEntityType : uint8
{
    None,
    Vehicle,
    Prop,
    Animal
};

enum class EModelItemType : uint8
{
    None,
    Weapon,
    Tool,
    Misc
};

/** Strict, immutable view of one author-owned model JSON document. */
struct GLTFSIMULATOR_API FModelDefinition
{
    FGuid UUID;
    FString Name;
    FString DisplayName;
    FString GlbPath;
    FString JsonPath;
    EModelDefinitionType ModelType = EModelDefinitionType::Invalid;
    EModelEntityType EntityType = EModelEntityType::None;
    EModelItemType ItemType = EModelItemType::None;
    TMap<FString, FString> Bones;

    bool IsLoadable() const
    {
        return ModelType == EModelDefinitionType::Static
            || ModelType == EModelDefinitionType::Dynamic
            || ModelType == EModelDefinitionType::Character;
    }
};

/**
 * Disk-side helpers for model definition JSON files.
 *
 * The model directory is intentionally category-free. Every subdirectory is
 * scanned recursively, and a GLB is paired only with the JSON that has the
 * same base filename in the same directory.
 */
namespace ModelDefinitionJson
{
    /**
     * Creates a definition for every GLB whose sibling JSON is missing.
     * Generated definitions use ModelType=Static so a newly added map GLB participates in the
     * first Play build immediately. Developers may assign another explicit type in that JSON.
     * OutDiscoveredGlbFiles receives the exact normalized, de-duplicated recursive scan used for
     * generation, allowing the builder to consume the same source snapshot without another walk.
     *
     * This function performs file-system work only and is safe to call from a
     * worker thread. It never touches UObjects or world state.
     */
    GLTFSIMULATOR_API bool EnsureMissingDefinitions(
        const FString& ModelRootDirectory,
        TArray<FString>* OutCreatedJsonFiles = nullptr,
        TArray<FString>* OutDiscoveredGlbFiles = nullptr);

    /** Returns true only for model types that are allowed to instantiate. */
    GLTFSIMULATOR_API bool IsLoadableModelType(const FString& ModelType);

    /**
     * Strictly validates a definition. Missing/empty/None ModelType is normalized to Static because
     * every discovered GLB is a build input; unknown types, invalid UUIDs, mismatched subtypes and
     * a character without a Bones object are rejected and logged by the caller.
     * OutCanonicalJson receives the normalized snapshot of the exact DOM that passed validation.
     */
    GLTFSIMULATOR_API bool LoadDefinition(
        const FString& JsonPath,
        const FString& ExpectedGlbPath,
        FModelDefinition& OutDefinition,
        FString& OutError,
        FString* OutCanonicalJson = nullptr);

    GLTFSIMULATOR_API FString ModelTypeToString(EModelDefinitionType Type);
}
