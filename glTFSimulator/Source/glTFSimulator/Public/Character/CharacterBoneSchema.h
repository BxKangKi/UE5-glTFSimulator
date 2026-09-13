// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file CharacterBoneSchema.h
 * Canonical character-bone schema shared by JSON validation, world baking, and runtime loading.
 */

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class USkeleton;
struct FReferenceSkeleton;

namespace CharacterBoneSchema
{
    /** The canonical JSON keys are fixed. Source bone-name values are author-defined. */
    GLTFSIMULATOR_API const TArray<FName>& GetRequiredKeys();

    /**
     * Validates a canonical-key -> source-bone JSON object and converts it to the source -> canonical
     * alias map required by glTFRuntime. Missing, extra, empty, or duplicate source mappings fail.
     */
    GLTFSIMULATOR_API bool BuildSourceToCanonicalMap(
        const TSharedPtr<FJsonObject>& BonesObject,
        TMap<FString, FString>& OutAliases,
        FString& OutError);

    /** Validates the internal source-bone -> canonical-key representation stored in built archives. */
    GLTFSIMULATOR_API bool ValidateSourceToCanonicalMap(
        const TMap<FString, FString>& Aliases,
        FString& OutError);

    /** Every required canonical bone must exist exactly once in the reference skeleton. */
    GLTFSIMULATOR_API bool ValidateCanonicalReferenceSkeleton(
        const FReferenceSkeleton& ReferenceSkeleton,
        FString& OutError);

    GLTFSIMULATOR_API bool ValidateCanonicalSkeleton(
        const USkeleton* Skeleton,
        FString& OutError);

    /** Ensures the final runtime mesh preserved the canonical target skeleton hierarchy. */
    GLTFSIMULATOR_API bool ValidateCanonicalHierarchyMatches(
        const FReferenceSkeleton& Candidate,
        const FReferenceSkeleton& Target,
        FString& OutError);
}
