// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file glTFSimulator.Target.cs
 // UE 5.8 target configuration for glTFSimulator.
 // UE 5.8 target configuration for glTFSimulator.
 */

using UnrealBuildTool;
using System.Collections.Generic;

public class glTFSimulatorTarget : TargetRules
{
    public glTFSimulatorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.AddRange( new string[] { "glTFSimulator"} );
    }
}
