// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Character/RagdollRecoveryMath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRagdollRebaseWorldPoseTest,
    "glTFSimulator.Character.Ragdoll.RebasePreservesWorldPose",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRagdollRebaseWorldPoseTest::RunTest(const FString& Parameters)
{
    for (double Distance : {0.0, 1000000.0, -1000000.0})
    {
        const FTransform OldFrame(FRotator(10, 55, 25), FVector(Distance, -420, -800), FVector(1.2));
        const FTransform NewFrame(FRotator(0, 145, 0), FVector(Distance + 110, -390, -900), FVector(1.2));
        const FTransform Root(FRotator(15, -20, 4), FVector(35, -12, 80));
        const FTransform Child(FRotator(5, 0, 2), FVector(0, 0, 24));
        const FTransform Rebased = RagdollRecoveryMath::RebaseRoot(Root, OldFrame, NewFrame);
        TestTrue(TEXT("Root world pose survives frame change"), (Rebased * NewFrame).Equals(Root * OldFrame, 0.001));
        TestTrue(TEXT("Descendant world pose survives frame change"),
            (Child * Rebased * NewFrame).Equals(Child * Root * OldFrame, 0.001));
    }
    return true;
}

#endif
