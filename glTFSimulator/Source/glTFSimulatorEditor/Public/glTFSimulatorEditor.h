// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file glTFSimulatorEditor.h
 * Primary editor module for glTFSimulator.
 *
 * UnrealEd-only behavior and editor automation tests live in this module. The
 * runtime module communicates through IGlTFSimulatorEditorServices and never links UnrealEd.
 */

#pragma once

#include "CoreMinimal.h"
#include "Editor/GlTFSimulatorEditorServices.h"
#include "Modules/ModuleManager.h"

class UPhysicsAsset;
struct FWorldContext;

class GLTFSIMULATOREDITOR_API FglTFSimulatorEditorModule final
    : public IModuleInterface
    , public IGlTFSimulatorEditorServices
{
public:
    static FglTFSimulatorEditorModule& Get();
    static bool IsAvailable();

    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    // IGlTFSimulatorEditorServices
    virtual void RefreshPhysicsAsset(UPhysicsAsset* PhysicsAsset) override;

private:
    /** Clears editor undo references only for loads belonging to a PIE world context. */
    void HandlePreLoadMap(const FWorldContext& WorldContext, const FString& MapName);

    bool bEditorServicesRegistered = false;
    FDelegateHandle PreLoadMapHandle;
};
