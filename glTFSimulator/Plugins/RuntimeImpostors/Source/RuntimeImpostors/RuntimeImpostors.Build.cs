// Copyright 2026 OpenAI. Licensed under the MIT License.

using UnrealBuildTool;

public class RuntimeImpostors : ModuleRules
{
    public RuntimeImpostors(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "ProceduralMeshComponent",
            "glTFRuntime"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "RenderCore",
            "RHI"
        });

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new string[]
            {
                "AssetRegistry",
                "UnrealEd"
            });
        }
    }
}
