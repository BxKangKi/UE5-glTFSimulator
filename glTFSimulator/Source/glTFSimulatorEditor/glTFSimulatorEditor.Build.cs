// Copyright © 2026 BxKangKi. Licensed under the MIT License.

using UnrealBuildTool;

public class glTFSimulatorEditor : ModuleRules
{
    public glTFSimulatorEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // Editor-only services, automation, and UnrealEd integrations live in this module.
        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "glTFSimulator"
            });

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "UnrealEd",
                "glTFRuntime",
                "Json",
                "JsonUtilities",
                "RHI"
            });
    }
}
