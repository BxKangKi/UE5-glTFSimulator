// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once

#include "CoreMinimal.h"
#include "Simulator/AssetDefinitionTypes.h"

/** Strict immutable view of one Sound JSON + same-basename WAV pair. */
struct GLTFSIMULATOR_API FSoundDefinition
{
    EAssetDefinitionType AssetType = EAssetDefinitionType::Sound;
    FGuid UUID;
    FString Name;
    FString DisplayName;
    FString WavPath;
    FString JsonPath;

    bool IsLoadable() const
    {
        return AssetType == EAssetDefinitionType::Sound && UUID.IsValid()
            && !Name.IsEmpty() && !WavPath.IsEmpty();
    }
};

/** Disk-side helpers for Sound assets. A Sound is exactly <base>.json + <base>.wav. */
namespace SoundDefinitionJson
{
    /** Recursively discovers WAV files and creates a missing same-basename Sound JSON. */
    GLTFSIMULATOR_API bool EnsureMissingDefinitions(
        const FString& AssetRootDirectory,
        TArray<FString>* OutCreatedJsonFiles = nullptr,
        TArray<FString>* OutDiscoveredWavFiles = nullptr);

    /** Strictly validates AssetType=Sound and the exact same-basename WAV pairing. */
    GLTFSIMULATOR_API bool LoadDefinition(
        const FString& JsonPath,
        const FString& ExpectedWavPath,
        FSoundDefinition& OutDefinition,
        FString& OutError,
        FString* OutCanonicalJson = nullptr);
}
