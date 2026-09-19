// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file glTFSimulator.Build.cs
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 */

using UnrealBuildTool;

public class glTFSimulator : ModuleRules
{
    public glTFSimulator(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "EnhancedInput",
                "InputCore",
                "Json",
                "glTFRuntime",
                "Slate",
                "UMG",
                "SlateCore"
            });

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "Niagara",
                "JsonUtilities",
                "RHI",
                "ProceduralMeshComponent",
                "PhysicsCore",
                // MoviePlayer renders a pure-Slate loading screen while blocking map loads run.
                "MoviePlayer"
            });

    }
}
