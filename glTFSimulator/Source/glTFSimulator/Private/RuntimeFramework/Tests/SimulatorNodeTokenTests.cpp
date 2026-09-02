#include "RuntimeFramework/SimulatorNodeTokenLibrary.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimulatorNodeTokenContractTest,
    "glTFSimulator.Runtime.NodeTokenContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSimulatorNodeTokenContractTest::RunTest(const FString& Parameters)
{
    const FSimulatorParsedNodeName Bare = USimulatorNodeTokenLibrary::ParseNodeName(TEXT("BodyINST"));
    TestFalse(TEXT("A bare suffix is not a directive"), Bare.HasEffectiveToken(TEXT("INST")));

    const FSimulatorParsedNodeName Alias = USimulatorNodeTokenLibrary::ParseNodeName(TEXT("wheel_front"));
    TestFalse(TEXT("Legacy wheel underscore aliases are not directives"), Alias.HasEffectiveToken(TEXT("WHEEL")));

    const FSimulatorParsedNodeName FirstWins = USimulatorNodeTokenLibrary::ParseNodeName(TEXT("Body;INST;LOD0"));
    TestTrue(TEXT("First render token wins"), FirstWins.HasEffectiveToken(TEXT("INST")));
    TestFalse(TEXT("Later conflicting render token is ignored"), FirstWins.HasEffectiveToken(TEXT("LOD0")));

    const FSimulatorParsedNodeName DifferentFamilies = USimulatorNodeTokenLibrary::ParseNodeName(TEXT("WheelFL;WHEEL;LOD0;NCOL"));
    TestTrue(TEXT("Node role remains effective"), DifferentFamilies.HasEffectiveToken(TEXT("WHEEL")));
    TestTrue(TEXT("Render mode can coexist"), DifferentFamilies.HasEffectiveToken(TEXT("LOD0")));
    TestTrue(TEXT("Collision mode can coexist"), DifferentFamilies.HasEffectiveToken(TEXT("NCOL")));
    TestEqual(TEXT("Base name is preserved"), DifferentFamilies.BaseName, FString(TEXT("WheelFL")));

    const FSimulatorParsedNodeName Water = USimulatorNodeTokenLibrary::ParseNodeName(TEXT("River;WATER"));
    TestTrue(TEXT("WATER is recognized only as a semicolon token"), Water.HasEffectiveToken(TEXT("WATER")));
    TestFalse(TEXT("WATER substring is not a directive"),
        USimulatorNodeTokenLibrary::ParseNodeName(TEXT("WATERFALL")).HasEffectiveToken(TEXT("WATER")));

    TestTrue(TEXT("Full-name helper preserves the first token after the delimiter"),
        USimulatorNodeTokenLibrary::HasEffectiveToken(TEXT("Body;INST;LOD0"), FName(TEXT("INST"))));
    return true;
}
#endif
