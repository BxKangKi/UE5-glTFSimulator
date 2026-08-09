// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "glTFSimulator.h"

#include "MoviePlayer.h"
#include "Framework/Application/SlateApplication.h"
#include "System/SafeFileIO.h"
#include "System/glTFRuntimeSafety.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    /**
     * Commandlets and dedicated servers do not create a game viewport. Avoid touching MoviePlayer
     * in those processes, and respect platforms/build modes where the engine disables it.
     */
    bool CanUseGlTFSimulatorLoadingScreen()
    {
        return !IsRunningCommandlet() &&
            !IsRunningDedicatedServer() &&
            IsMoviePlayerEnabled();
    }
}

void FglTFSimulatorModule::StartupModule()
{
    FDefaultGameModuleImpl::StartupModule();

    if (!CanUseGlTFSimulatorLoadingScreen())
    {
        return;
    }

    // The primary game module is loaded before the startup map. Register both paths because
    // OnPrepareLoadingScreen covers engine-driven startup preparation, while PreLoadMap covers
    // later OpenLevel calls made by gameplay code.
    PreLoadMapHandle = FCoreUObjectDelegates::PreLoadMap.AddRaw(
        this,
        &FglTFSimulatorModule::HandlePreLoadMap);

    if (IGameMoviePlayer* MoviePlayer = GetMoviePlayer())
    {
        PrepareLoadingScreenHandle = MoviePlayer->OnPrepareLoadingScreen().AddRaw(
            this,
            &FglTFSimulatorModule::PrepareLoadingScreen);
    }

    // Prepare immediately as well, so the very first game window does not wait for a map actor or
    // UMG widget to reach BeginPlay before it has something visible to render.
    PrepareLoadingScreen();
}

void FglTFSimulatorModule::PrepareLoadingScreen()
{
    if (!CanUseGlTFSimulatorLoadingScreen())
    {
        return;
    }

    // StartupModule can be entered before Slate on unusual launch paths. Building an SWidget before
    // FSlateApplication exists is unsafe, so let the registered engine callbacks retry later.
    if (!FSlateApplication::IsInitialized())
    {
        return;
    }

    IGameMoviePlayer* MoviePlayer = GetMoviePlayer();
    if (!MoviePlayer || MoviePlayer->IsMovieCurrentlyPlaying())
    {
        return;
    }

    FLoadingScreenAttributes LoadingScreen;
    LoadingScreen.bAllowEngineTick = false;
    LoadingScreen.bAllowInEarlyStartup = true;
    LoadingScreen.bAutoCompleteWhenLoadingCompletes = true;
    LoadingScreen.bMoviesAreSkippable = false;
    LoadingScreen.bWaitForManualStop = false;
    LoadingScreen.MinimumLoadingScreenDisplayTime = 0.0f;

    // Epic's built-in test widget is a pure-Slate loading indicator. Using it here deliberately
    // avoids loading a UMG class, texture UObject, or project package before the startup map exists.
    LoadingScreen.WidgetLoadingScreen = FLoadingScreenAttributes::NewTestLoadingScreenWidget();
    MoviePlayer->SetupLoadingScreen(LoadingScreen);
}

void FglTFSimulatorModule::HandlePreLoadMap(const FString& MapName)
{
    // MapName is intentionally not dereferenced or resolved here: package lookup can create UObjects,
    // while this callback only needs to arm the already self-contained Slate loading screen.
    UE_LOG(LogTemp, VeryVerbose, TEXT("Preparing blocking loading screen for map: %s"), *MapName);
    PrepareLoadingScreen();
}

void FglTFSimulatorModule::ShutdownModule()
{
    // Remove all engine callbacks first. This guarantees that no late map-load notification can call
    // into the game module after its shutdown sequence has begun.
    if (PreLoadMapHandle.IsValid())
    {
        FCoreUObjectDelegates::PreLoadMap.Remove(PreLoadMapHandle);
        PreLoadMapHandle.Reset();
    }

    if (PrepareLoadingScreenHandle.IsValid())
    {
        // A valid handle proves that registration succeeded earlier. Remove it even if the engine's
        // enabled flag changed during shutdown, otherwise MoviePlayer could retain a raw module pointer.
        if (IGameMoviePlayer* MoviePlayer = GetMoviePlayer())
        {
            MoviePlayer->OnPrepareLoadingScreen().Remove(PrepareLoadingScreenHandle);
        }
        PrepareLoadingScreenHandle.Reset();
    }

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
