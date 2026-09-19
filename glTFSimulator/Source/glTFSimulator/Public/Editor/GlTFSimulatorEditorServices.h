// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file GlTFSimulatorEditorServices.h
 * Runtime-facing optional bridge to services implemented by the glTFSimulatorEditor module.
 *
 * The runtime module deliberately owns only this dependency-free interface. Packaged/game builds
 * simply have no registered implementation, so requests become cheap no-ops without linking UnrealEd.
 */

#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeature.h"
#include "Features/IModularFeatures.h"

class UPhysicsAsset;

class GLTFSIMULATOR_API IGlTFSimulatorEditorServices : public IModularFeature
{
public:
    virtual ~IGlTFSimulatorEditorServices() = default;

    static FName GetModularFeatureName()
    {
        static const FName FeatureName(TEXT("glTFSimulatorEditorServices"));
        return FeatureName;
    }

    /** Rebuilds editor-side PhysicsAsset derived/cooked state after runtime asset mutation. */
    virtual void RefreshPhysicsAsset(UPhysicsAsset* PhysicsAsset) = 0;
};

/**
 * Small runtime helper that resolves the optional editor implementation through ModularFeatures.
 * No editor module header, UnrealEd type, or editor-only symbol is referenced by the game module.
 */
class GLTFSIMULATOR_API FGlTFSimulatorEditorServices
{
public:
    static IGlTFSimulatorEditorServices* Get()
    {
        IModularFeatures& Features = IModularFeatures::Get();
        const FName FeatureName = IGlTFSimulatorEditorServices::GetModularFeatureName();
        if (!Features.IsModularFeatureAvailable(FeatureName))
        {
            return nullptr;
        }
        return &Features.GetModularFeature<IGlTFSimulatorEditorServices>(FeatureName);
    }

    static void RefreshPhysicsAsset(UPhysicsAsset* PhysicsAsset)
    {
        if (IGlTFSimulatorEditorServices* Services = Get())
        {
            Services->RefreshPhysicsAsset(PhysicsAsset);
        }
    }
};
