// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Engine/Scene.h"
#include "ModelData.generated.h"

class UInstancedStaticMeshComponent;
class UBoxComponent;
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

    /** Runtime/cache-derived model center. This value is never read from or written to JSON. */
    UPROPERTY()
    FVector Center = FVector::ZeroVector;

    /** Runtime/cache-derived full model size. This value is never read from or written to JSON. */
    UPROPERTY()
    FVector Size = FVector::ZeroVector;

    /** User-authored read-only JSON settings, keyed by base mesh name. */
    UPROPERTY()
    TMap<FName, FMeshData> MeshData;

    /** Optional prefab name referenced by this model. The corresponding prefab JSON must declare AssetType="prefab". */
    UPROPERTY()
    FString Prefab;

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

    /** Unscaled local-space mesh half size loaded from or written to <model>.scz. */
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
    FTransform Transform;
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

USTRUCT(BlueprintType)
struct FLoadAsyncWrapper
{
    GENERATED_BODY()

    UPROPERTY()
    TMap<FName, FModelNodeData> NodeMap;

    UPROPERTY()
    TMap<FName, FWaterStreamNodeData> WaterNodeMap;

    UPROPERTY()
    TMap<FName, FModelMeshData> MeshMap;

    UPROPERTY()
    FModelData ModelData;
};

// Component group now also owns box components.
USTRUCT()
struct FComponentGroup
{
    GENERATED_BODY()

    UPROPERTY()
    TArray<TObjectPtr<UShapeComponent>> Colliders;
    UPROPERTY()
    TArray<TObjectPtr<ULightComponent>> Lights;
};

USTRUCT(BlueprintType)
struct FStreamAsyncWrapper
{
    GENERATED_BODY()

    UPROPERTY()
    TSet<FName> LoadedNodes;
    UPROPERTY()
    TSet<FName> LoadedWaterNodes;
    UPROPERTY()
    TMap<FName, FModelNodeData> NodeMap;
    UPROPERTY()
    TMap<FName, FWaterStreamNodeData> WaterNodeMap;
    UPROPERTY()
    TMap<FName, TObjectPtr<UInstancedStaticMeshComponent>> InstanceMap;
    UPROPERTY()
    TMap<FName, TObjectPtr<UBoxComponent>> UnloadBoxMap;
    UPROPERTY()
    TMap<FName, FComponentGroup> DynamicComponentMap;
    UPROPERTY()
    TMap<FName, TObjectPtr<AWaterActor>> WaterActorMap;
};
