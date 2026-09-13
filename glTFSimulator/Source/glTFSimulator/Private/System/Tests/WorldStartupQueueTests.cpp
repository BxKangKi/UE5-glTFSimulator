// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldStartupQueueTests.cpp
 * 역할: 월드 빌드 native 큐의 대기·취소를 검증합니다.
 * 핵심 기능: busy gate 대기 후 실행, 취소 콜백 1회, 티켓 회수.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "System/glTFRuntimeSafety.h"
#include "UObject/StrongObjectPtr.h"
#include "glTFRuntimeAsset.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldStartupQueueWaitTest,
    "glTFSimulator.World.Startup.NativeGateWaits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStartupQueueWaitTest::RunTest(const FString& Parameters)
{
    // Run in an idle editor. These tickets exercise scheduling only, never plugin decoding.
    if (FglTFRuntimeSafety::GetPendingOperationCount() != 0 || FglTFRuntimeSafety::IsCircuitOpen())
    {
        AddError(TEXT("Run this test without an active world build or native circuit failure."));
        return false;
    }
    TStrongObjectPtr<UObject> Owner(NewObject<UObject>());
    TStrongObjectPtr<UglTFRuntimeAsset> Asset(NewObject<UglTFRuntimeAsset>());
    bool bFirstStarted = false;
    bool bSecondStarted = false;
    const uint64 First = FglTFRuntimeSafety::EnqueueOperation(Owner.Get(), Asset.Get(), TEXT("test hold"),
        [&bFirstStarted](uint64) { bFirstStarted = true; });
    const uint64 Second = FglTFRuntimeSafety::EnqueueOperation(Owner.Get(), Asset.Get(), TEXT("test wait"),
        [&bSecondStarted](uint64 Ticket)
        {
            bSecondStarted = true;
            FglTFRuntimeSafety::CompleteOperation(Ticket);
        });
    TestTrue(TEXT("first request acquired gate"), First != 0 && bFirstStarted);
    TestTrue(TEXT("busy gate retains second request"), Second != 0 && !bSecondStarted);
    FglTFRuntimeSafety::CompleteOperation(First);
    TestTrue(TEXT("waiting request runs after release"), bSecondStarted);
    TestEqual(TEXT("all tickets returned"), FglTFRuntimeSafety::GetPendingOperationCount(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldStartupQueueCancelTest,
    "glTFSimulator.World.Startup.CancelQueuedOwner",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldStartupQueueCancelTest::RunTest(const FString& Parameters)
{
    if (FglTFRuntimeSafety::GetPendingOperationCount() != 0 || FglTFRuntimeSafety::IsCircuitOpen())
    {
        AddError(TEXT("Run this test in an idle editor."));
        return false;
    }
    TStrongObjectPtr<UObject> ActiveOwner(NewObject<UObject>());
    TStrongObjectPtr<UObject> CancelledOwner(NewObject<UObject>());
    TStrongObjectPtr<UglTFRuntimeAsset> Asset(NewObject<UglTFRuntimeAsset>());
    const uint64 Active = FglTFRuntimeSafety::EnqueueOperation(ActiveOwner.Get(), Asset.Get(), TEXT("test hold"), [](uint64) {});
    bool bCancelledStarted = false;
    int32 Rejections = 0;
    FglTFRuntimeSafety::EnqueueOperation(CancelledOwner.Get(), Asset.Get(), TEXT("test cancel"),
        [&bCancelledStarted](uint64 Ticket)
        {
            bCancelledStarted = true;
            FglTFRuntimeSafety::CompleteOperation(Ticket);
        }, [&Rejections](const FString&) { ++Rejections; });
    FglTFRuntimeSafety::CancelQueuedOperations(CancelledOwner.Get());
    FglTFRuntimeSafety::CompleteOperation(Active);
    TestFalse(TEXT("cancelled build did not execute"), bCancelledStarted);
    TestEqual(TEXT("cancel notified exactly once"), Rejections, 1);
    TestEqual(TEXT("queue released"), FglTFRuntimeSafety::GetPendingOperationCount(), 0);
    return true;
}
#endif
