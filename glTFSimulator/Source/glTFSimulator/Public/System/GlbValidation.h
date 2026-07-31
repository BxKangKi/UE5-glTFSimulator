// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

/** Lightweight validation helpers for untrusted binary glTF files. */
namespace GlbValidation
{
    /** Converts a supplied file path to a normalized absolute path. */
    GLTFSIMULATOR_API FString NormalizePath(const FString& FilePath);

    /** Validates the outer GLB container and every chunk boundary without parsing mesh data. */
    GLTFSIMULATOR_API bool ValidateFile(const FString& FilePath, FString& OutReason);

    /**
     * Performs the expensive runtime-mesh preflight on a worker thread before glTFRuntime is called.
     * The check validates JSON types, buffers, buffer views, accessors, primitive counts, LOD build
     * totals and estimated allocation limits so malformed input cannot request impossible TArray sizes.
     */
    GLTFSIMULATOR_API bool ValidateRuntimeMeshFile(const FString& FilePath, FString& OutReason);

    /** Validates a runtime mesh file and the additional node/skin requirements for a character GLB. */
    GLTFSIMULATOR_API bool ValidateCharacterFile(const FString& FilePath, FString& OutReason);
}
