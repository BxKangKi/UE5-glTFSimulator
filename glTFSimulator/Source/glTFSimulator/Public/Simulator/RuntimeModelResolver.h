// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file RuntimeModelResolver.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Simulator/ModelDefinitionJson.h"

class FGWorldArchiveReader;
class UWorldBakedModelAsset;

/** Immutable lookup result safe to capture in a worker after it was assembled on the game thread. */
struct GLTFSIMULATOR_API FResolvedRuntimeModel
{
    FGuid UUID;
    FModelDefinition Definition;
    FString Reference;
    FString DefinitionJson;
    TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> ArchiveReader;

    bool IsValid() const;
};

/**
 * Single runtime gateway for built models. It intentionally has no filename fallback: source GLBs
 * are consumed only by the world builder, never by gameplay actors after startup.
 */
class GLTFSIMULATOR_API FRuntimeModelResolver
{
public:
    /** Game-thread lookup against the immutable model directory. */
    static bool Resolve(
        const UObject* WorldContextObject,
        const FString& Reference,
        FResolvedRuntimeModel& OutModel,
        FString& OutError);

    /**
     * Creates a lightweight facade containing only node metadata and .dat ranges. No GLB byte
     * array or source-document parser is created at runtime.
     */
    static UWorldBakedModelAsset* LoadAssetSynchronously(
        const FResolvedRuntimeModel& Model,
        FString& OutError);
};
