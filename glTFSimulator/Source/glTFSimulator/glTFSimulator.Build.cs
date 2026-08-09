// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

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
                "IKRig",
                "Niagara",
                "RHI",
                "ProceduralMeshComponent",
                "PhysicsCore",
                "ImageWrapper",
                // MoviePlayer renders a pure-Slate loading screen while blocking map loads run.
                "MoviePlayer",
                "RenderCore"
            });

        if (Target.bBuildEditor)
        {
            // Editor-only transaction reset is used to prevent PIE world leaks caused by REINST widget objects
            // being retained in the editor undo buffer during level travel.
            PrivateDependencyModuleNames.Add("UnrealEd");
        }
    }
}
