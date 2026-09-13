// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file SettingsFieldTypes.h
 * Shared reflected enums used by settings menus and generated settings-row widgets.
 */

#pragma once

#include "CoreMinimal.h"
#include "SettingsFieldTypes.generated.h"

UENUM(BlueprintType)
enum class ESettingsField : uint8
{
    BloomIntensity UMETA(DisplayName="Bloom Intensity"),
    BloomThreshold UMETA(DisplayName="Bloom Threshold"),
    AmbientOcclusionIntensity UMETA(DisplayName="Ambient Occlusion"),
    RayTracing UMETA(DisplayName="Ray Tracing"),
    HeightFog UMETA(DisplayName="Height Fog"),
    Cloud UMETA(DisplayName="Cloud"),
    ShadowQuality UMETA(DisplayName="Shadow Quality"),
    TextureQuality UMETA(DisplayName="Texture Quality"),
    MaxTextureResolution UMETA(DisplayName="Max Texture Resolution"),
    ViewDistanceQuality UMETA(DisplayName="View Distance Quality"),
    StreamingDistanceMultiplier UMETA(DisplayName="Streaming Distance Multiplier"),
    StreamingUnloadDistanceMultiplier UMETA(DisplayName="Streaming Unload Multiplier"),
    ObjectStreamingRadiusMeters UMETA(DisplayName="Object Streaming Radius"),
    StreamingSceneSpawnBudget UMETA(DisplayName="Scene Spawn Budget"),
    StreamingNodeBudgetPerFrame UMETA(DisplayName="Node Budget Per Frame"),
    AntiAliasingQuality UMETA(DisplayName="Anti Aliasing Quality"),
    PostProcessingQuality UMETA(DisplayName="Post Processing Quality"),
    EffectsQuality UMETA(DisplayName="Effects Quality"),
    FoliageQuality UMETA(DisplayName="Foliage Quality"),
    ShadingQuality UMETA(DisplayName="Shading Quality"),
    GlobalIlluminationQuality UMETA(DisplayName="GI Quality"),
    ReflectionQuality UMETA(DisplayName="Reflection Quality"),
    DynamicGlobalIlluminationMethod UMETA(DisplayName="GI Method"),
    ReflectionMethod UMETA(DisplayName="Reflection Method")
};

UENUM(BlueprintType)
enum class ESettingsControlType : uint8
{
    Slider UMETA(DisplayName="Slider"),
    Dropdown UMETA(DisplayName="Dropdown"),
    Toggle UMETA(DisplayName="Toggle Button")
};
