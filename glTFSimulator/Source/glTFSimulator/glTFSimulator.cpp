// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "glTFSimulator.h"

#include "System/SafeFileIO.h"
#include "System/glTFRuntimeSafety.h"

void FglTFSimulatorModule::ShutdownModule()
{
    // Stop accepting new native mesh work first, then reject queued requests. An active plugin job
    // is allowed to reach its terminal callback because interrupting it could free parser memory
    // while a glTFRuntime worker still references it.
    FglTFRuntimeSafety::BeginShutdown();

    // Stop new disk transactions and wait for already accepted atomic saves to finish. This greatly
    // reduces the chance of leaving only an incomplete temporary file during a normal application exit.
    FSafeFileIO::BeginShutdown();

    if (!FglTFRuntimeSafety::FlushPendingOperations(30.0))
    {
        UE_LOG(LogTemp, Error, TEXT("Timed out while draining an active glTFRuntime operation during shutdown."));
    }
    if (!FSafeFileIO::FlushPendingOperations(30.0))
    {
        UE_LOG(LogTemp, Error, TEXT("Timed out while draining asynchronous file transactions during shutdown."));
    }

    FDefaultGameModuleImpl::ShutdownModule();
}

IMPLEMENT_PRIMARY_GAME_MODULE(FglTFSimulatorModule, glTFSimulator, "glTFSimulator");
