// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/** Primary game module that drains tracked runtime work before the module DLL is unloaded. */
class FglTFSimulatorModule final : public FDefaultGameModuleImpl
{
public:
    virtual void ShutdownModule() override;
};
