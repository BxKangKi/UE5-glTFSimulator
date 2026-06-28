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
                "EnhancedInput"
            });

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "CoreUObject",
                "Engine",
                "InputCore",
                "Json",
                "IKRig",
                "Niagara",
                "UMG",
                "RHI",
                "Slate",
                "SlateCore",
                "ProceduralMeshComponent",
                "PhysicsCore",
                "ImageWrapper",
                "RenderCore",
                "glTFRuntime"
            });
        

        // Uncomment if you are using online features
        // PrivateDependencyModuleNames.Add("OnlineSubsystem");

        // To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
    }
}
