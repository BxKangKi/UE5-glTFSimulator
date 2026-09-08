#pragma once

#include "CoreMinimal.h"

enum class EModelDefinitionType : uint8
{
    None,
    Scene,
    Prefab,
    Item,
    Character,
    Entity
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
    EModelDefinitionType ModelType = EModelDefinitionType::None;
    EModelEntityType EntityType = EModelEntityType::None;
    EModelItemType ItemType = EModelItemType::None;
    TMap<FString, FString> Bones;

    bool IsLoadable() const { return ModelType != EModelDefinitionType::None; }
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
     * Generated definitions use ModelType=None and therefore can never spawn
     * a runtime object until a developer explicitly assigns a loadable type.
     *
     * This function performs file-system work only and is safe to call from a
     * worker thread. It never touches UObjects or world state.
     */
    GLTFSIMULATOR_API bool EnsureMissingDefinitions(
        const FString& ModelRootDirectory,
        TArray<FString>* OutCreatedJsonFiles = nullptr);

    /** Returns true only for model types that are allowed to instantiate. */
    GLTFSIMULATOR_API bool IsLoadableModelType(const FString& ModelType);

    /**
     * Strictly validates a definition. Unknown/missing ModelType, invalid UUID, mismatched subtype,
     * and a character without a Bones object are rejected and logged by the caller.
     */
    GLTFSIMULATOR_API bool LoadDefinition(
        const FString& JsonPath,
        const FString& ExpectedGlbPath,
        FModelDefinition& OutDefinition,
        FString& OutError);

    GLTFSIMULATOR_API FString ModelTypeToString(EModelDefinitionType Type);
}
