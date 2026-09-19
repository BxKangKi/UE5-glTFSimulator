/**
 * @file NodeTokenTests.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Simulator/NodeTokenLibrary.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimulatorNodeTokenContractTest,
    "glTFSimulator.Runtime.NodeTokenContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSimulatorNodeTokenContractTest::RunTest(const FString& Parameters)
{
    const FSimulatorParsedNodeName Alias = USimulatorNodeTokenLibrary::ParseNodeName(TEXT("wheel_front"));
    TestFalse(TEXT("Legacy wheel underscore aliases are not directives"), Alias.HasEffectiveToken(TEXT("WHEEL")));

    const FSimulatorParsedNodeName FirstWins = USimulatorNodeTokenLibrary::ParseNodeName(TEXT("Body;LOD1;LOD0"));
    TestTrue(TEXT("First render token wins"), FirstWins.HasEffectiveToken(TEXT("LOD1")));
    TestFalse(TEXT("Later conflicting render token is ignored"), FirstWins.HasEffectiveToken(TEXT("LOD0")));

    const FSimulatorParsedNodeName DifferentFamilies = USimulatorNodeTokenLibrary::ParseNodeName(TEXT("WheelFL;WHEEL;LOD0;NCOL"));
    TestTrue(TEXT("Node role remains effective"), DifferentFamilies.HasEffectiveToken(TEXT("WHEEL")));
    TestTrue(TEXT("Render mode can coexist"), DifferentFamilies.HasEffectiveToken(TEXT("LOD0")));
    TestTrue(TEXT("Collision mode can coexist"), DifferentFamilies.HasEffectiveToken(TEXT("NCOL")));
    TestEqual(TEXT("Base name is preserved"), DifferentFamilies.BaseName, FString(TEXT("WheelFL")));

    const FSimulatorParsedNodeName InstancedLod0 = USimulatorNodeTokenLibrary::ParseNodeName(TEXT("Tree;LOD0;INST"));
    TestTrue(TEXT("INST is recognized as an independent token family"),
        InstancedLod0.HasEffectiveToken(TEXT("INST")));
    TestTrue(TEXT("INST can coexist with LOD0"),
        InstancedLod0.HasEffectiveToken(TEXT("LOD0")));
    TestEqual(TEXT("INST keeps the canonical base name"),
        InstancedLod0.BaseName, FString(TEXT("Tree")));
    TestFalse(TEXT("INST substring/underscore aliases are not directives"),
        USimulatorNodeTokenLibrary::ParseNodeName(TEXT("Tree_INST")).HasEffectiveToken(TEXT("INST")));
    const FSimulatorParsedNodeName UnsupportedLod = USimulatorNodeTokenLibrary::ParseNodeName(TEXT("Tree;LOD4"));
    TestFalse(TEXT("Only LOD0 through LOD3 are supported"), UnsupportedLod.HasEffectiveToken(TEXT("LOD4")));
    TestTrue(TEXT("Unsupported numeric LOD is retained as an invalid segment"),
        UnsupportedLod.InvalidSegments.Contains(TEXT("LOD4")));

    const FSimulatorParsedNodeName Water = USimulatorNodeTokenLibrary::ParseNodeName(TEXT("River;WATER"));
    TestTrue(TEXT("WATER is recognized only as a semicolon token"), Water.HasEffectiveToken(TEXT("WATER")));
    TestFalse(TEXT("WATER substring is not a directive"),
        USimulatorNodeTokenLibrary::ParseNodeName(TEXT("WATERFALL")).HasEffectiveToken(TEXT("WATER")));

    TestTrue(TEXT("Full-name helper preserves the first render token"),
        USimulatorNodeTokenLibrary::HasEffectiveToken(TEXT("Body;LOD1;LOD0"), FName(TEXT("LOD1"))));
    return true;
}
#endif
