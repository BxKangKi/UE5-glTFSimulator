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
                "UMG",
                "SlateCore"
            });

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "InputCore",
                "Json",
                "IKRig",
                "Niagara",
                "RHI",
                "Slate",
                "ProceduralMeshComponent",
                "PhysicsCore",
                "ImageWrapper",
                "RenderCore",
                "glTFRuntime"
            });

        if (Target.bBuildEditor)
        {
            // Editor-only transaction reset is used to prevent PIE world leaks caused by REINST widget objects
            // being retained in the editor undo buffer during level travel.
            PrivateDependencyModuleNames.Add("UnrealEd");
        }
    }
}
