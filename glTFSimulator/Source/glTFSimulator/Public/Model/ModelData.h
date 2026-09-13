// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file ModelData.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Engine/Scene.h"
#include "ModelData.generated.h"

class UShapeComponent;
class ULightComponent;
class AWaterActor;

UENUM(BlueprintType)
enum class EColliderType : uint8
{
    None UMETA(DisplayName = "None"),
    Sphere UMETA(DisplayName = "Sphere"),
    Capsule UMETA(DisplayName = "Capsule"),
    Box UMETA(DisplayName = "Box"),
};

USTRUCT(BlueprintType)
struct FModelCollider
{
    GENERATED_BODY()

    UPROPERTY()
    EColliderType Collider = EColliderType::None;
    UPROPERTY()
    FVector Center = FVector::ZeroVector;
    UPROPERTY()
    FVector Size = FVector::ZeroVector;

    TSharedRef<FJsonObject> Serialization() const;
    bool Deserialization(const TSharedPtr<FJsonObject> &Json);
};

USTRUCT(BlueprintType)
struct FLightData
{
    GENERATED_BODY()

    UPROPERTY()
    FVector Location = FVector::ZeroVector;
    UPROPERTY()
    float Intensity = 10.0f;
    UPROPERTY()
    float SourceRadius = 10.0f;
    UPROPERTY()
    float SoftSourceRadius = 10.0f;
    UPROPERTY()
    float AttenuationRadius = 1000.0f;
    UPROPERTY()
    float Length = 10.0f;
    UPROPERTY()
    ELightUnits Unit = ELightUnits::Unitless;

    TSharedRef<FJsonObject>
    Serialization() const;
    bool Deserialization(const TSharedPtr<FJsonObject> &Json);
};

USTRUCT(BlueprintType)
struct FMeshData
{
    GENERATED_BODY()

    UPROPERTY()
    bool bComplexCollision = true;

    UPROPERTY()
    bool bSimpleCollision = false;

    UPROPERTY()
    bool bIsEntity = false;

    UPROPERTY()
    TArray<FModelCollider> Colliders;

    UPROPERTY()
    TArray<FLightData> Lights;

    TSharedRef<FJsonObject> Serialization() const;
    bool Deserialization(const TSharedPtr<FJsonObject> &Json);
};

USTRUCT(BlueprintType)
struct FModelData
{
    GENERATED_BODY()

    /** Build-derived model center stored in the .gwd directory, never in author JSON. */
    UPROPERTY()
    FVector Center = FVector::ZeroVector;

    /** Build-derived full model size stored in the .gwd directory, never in author JSON. */
    UPROPERTY()
    FVector Size = FVector::ZeroVector;

    /** User-authored read-only JSON settings, keyed by base mesh name. */
    UPROPERTY()
    TMap<FName, FMeshData> MeshData;

    TSharedRef<FJsonObject> Serialization() const;
    bool Deserialization(const TSharedPtr<FJsonObject> &Json);
};

USTRUCT(BlueprintType)
struct FModelMeshData
{
    GENERATED_BODY()

    UPROPERTY()
    int32 LOD0 = INDEX_NONE;

    UPROPERTY()
    int32 LOD1 = INDEX_NONE;

    UPROPERTY()
    int32 LOD2 = INDEX_NONE;

    UPROPERTY()
    int32 LOD3 = INDEX_NONE;

    UPROPERTY()
    FMeshData Data;

    /** Unscaled local-space mesh half size; the .gwd metadata member also stores full Size. */
    UPROPERTY()
    FVector Extent = FVector::ZeroVector;

    /** Full local-space mesh size retained for existing streaming calculations. */
    UPROPERTY()
    FVector Size = FVector::ZeroVector;
#if WITH_EDITOR
    FString ToString();
#endif
};

USTRUCT(BlueprintType)
struct FModelNodeData
{
    GENERATED_BODY()

    UPROPERTY()
    FName MeshName = NAME_None;

    UPROPERTY()
    FTransform Transform = FTransform::Identity;

    /** 8192 m model-space coarse chunk. */
    UPROPERTY()
    FIntVector CoarseChunk = FIntVector::ZeroValue;

    /** 512 m child chunk, stored as 0..15 coordinates relative to CoarseChunk. */
    UPROPERTY()
    FIntVector FineChunk = FIntVector::ZeroValue;

    /** A single transformed mesh larger than 8192 m is retained for the scene lifetime. */
    UPROPERTY()
    bool bAlwaysLoaded = false;
};


USTRUCT(BlueprintType)
struct FWaterStreamNodeData
{
    GENERATED_BODY()

    UPROPERTY()
    FTransform Transform = FTransform::Identity;

    UPROPERTY()
    float StreamRadius = 65536.0f;
};

/** Render-oriented scene tables decoded from one model's metadata.dat member. */
USTRUCT(BlueprintType)
struct FGWorldSceneRenderData
{
    GENERATED_BODY()

    /** True only when definition parsing and the complete bounded node scan finished. */
    UPROPERTY()
    bool bSuccess = false;

    UPROPERTY()
    TMap<FName, FModelNodeData> NodeMap;

    UPROPERTY()
    TMap<FName, FWaterStreamNodeData> WaterNodeMap;

    UPROPERTY()
    TMap<FName, FModelMeshData> MeshMap;

    UPROPERTY()
    FModelData ModelData;
};

/** Runtime collider/light components owned by one streamed scene node. */
USTRUCT()
struct FComponentGroup
{
    GENERATED_BODY()

    UPROPERTY()
    TArray<TObjectPtr<UShapeComponent>> Colliders;
    UPROPERTY()
    TArray<TObjectPtr<ULightComponent>> Lights;
};

/** Terminal result from one mesh-group or water-group .gwd streaming pass. */
USTRUCT(BlueprintType)
struct FWorldSceneStreamResult
{
    GENERATED_BODY()

    /** Mesh name processed by this action. NAME_None identifies the water-only group. */
    UPROPERTY()
    FName GroupName;

    UPROPERTY()
    bool bWaterGroup = false;

    UPROPERTY()
    bool bGroupFailed = false;

    UPROPERTY()
    TSet<FName> LoadedWaterNodes;
    UPROPERTY()
    TMap<FName, FWaterStreamNodeData> WaterNodeMap;
    UPROPERTY()
    TMap<FName, TObjectPtr<AWaterActor>> WaterActorMap;
};
