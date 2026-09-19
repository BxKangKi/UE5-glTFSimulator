// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file GlbValidation.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"

/** Lightweight validation helpers for untrusted binary glTF files. */
namespace GlbValidation
{
    /** Converts a supplied file path to a normalized absolute path. */
    GLTFSIMULATOR_API FString NormalizePath(const FString& FilePath);

    /** Validates a binary glTF container without parsing mesh data. */
    GLTFSIMULATOR_API bool ValidateFile(const FString& FilePath, FString& OutReason);

    /**
     * Performs the expensive authoring preflight on a worker before the build-only glTFRuntime
     * parser is called. JSON types, buffer ranges, accessors, primitives and allocation estimates
     * are bounded so malformed source data cannot request impossible native allocations.
     */
    GLTFSIMULATOR_API bool ValidateBuildSourceFile(const FString& FilePath, FString& OutReason);
}
