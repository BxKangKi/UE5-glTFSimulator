// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file glTFSimulator.h
 * 역할: 프로젝트 게임 모듈의 수명을 관리합니다.
 * 핵심 기능: 모듈 시작·종료, 로딩 화면, 추적 작업 종료 대기.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * Primary game module.
 *
 * Besides draining tracked runtime work during shutdown, the module prepares a pure-Slate
 * MoviePlayer screen before the initial map and every blocking map load. The screen contains no
 * UObjects, so it is safe while the game thread is occupied by package loading.
 */
class FglTFSimulatorModule final : public FDefaultGameModuleImpl
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    /** Rebuilds the lightweight MoviePlayer screen when the engine requests one. */
    void PrepareLoadingScreen();

    /** Prepares the screen before a blocking map load starts. */
    void HandlePreLoadMap(const FString& MapName);

    /** Delegate handles are removed before module shutdown to prevent callbacks into unloaded code. */
    FDelegateHandle PrepareLoadingScreenHandle;
    FDelegateHandle PreLoadMapHandle;
};
