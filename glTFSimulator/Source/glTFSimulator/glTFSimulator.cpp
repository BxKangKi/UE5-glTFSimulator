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

    // A timeout followed by DLL unload is more dangerous than a slow shutdown: a native parser or
    // queued lambda could still return into unloaded module code. Drain both lifetime trackers fully.
    FglTFRuntimeSafety::FlushPendingOperations(-1.0);
    FSafeFileIO::FlushPendingOperations(-1.0);

    FDefaultGameModuleImpl::ShutdownModule();
}

IMPLEMENT_PRIMARY_GAME_MODULE(FglTFSimulatorModule, glTFSimulator, "glTFSimulator");
