// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file glTFSimulator.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
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
