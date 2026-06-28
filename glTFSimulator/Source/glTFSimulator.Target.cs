// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class glTFSimulatorTarget : TargetRules
{
    public glTFSimulatorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.V6;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
        ExtraModuleNames.AddRange( new string[] { "glTFSimulator"} );
    }
}
