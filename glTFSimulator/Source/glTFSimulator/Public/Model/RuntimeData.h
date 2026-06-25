// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "RuntimeData.generated.h"

class UInstancedStaticMeshComponent;
class UBoxComponent;

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
struct FRuntimeLightData
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
    ELightUnits Unit;

    TSharedRef<FJsonObject>
    Serialization() const;
    bool Deserialization(const TSharedPtr<FJsonObject> &Json);
};

USTRUCT(BlueprintType)
struct FRuntimeMeshData
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
    TArray<FRuntimeLightData> Lights;

    TSharedRef<FJsonObject> Serialization() const;
    bool Deserialization(const TSharedPtr<FJsonObject> &Json);
};

USTRUCT(BlueprintType)
struct FRuntimeModelData
{
    GENERATED_BODY()

    UPROPERTY()
    FVector Center = FVector::ZeroVector;

    UPROPERTY()
    TMap<FName, FRuntimeMeshData> MeshData;

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
    FRuntimeMeshData Data;

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
struct FLoadAsyncWrapper
{
    GENERATED_BODY()

    UPROPERTY()
    TMap<FName, FModelNodeData> NodeMap;

    UPROPERTY()
    TMap<FName, FModelMeshData> MeshMap;
};

// [수정] 박스 컴포넌트까지 통합하여 관리하도록 그룹 구조체 확장
USTRUCT()
struct FRuntimeComponentGroup
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
    TMap<FName, FModelNodeData> NodeMap;
    UPROPERTY()
    TMap<FName, TObjectPtr<UInstancedStaticMeshComponent>> InstanceMap;
    UPROPERTY()
    TMap<FName, TObjectPtr<UBoxComponent>> UnloadBoxMap;
    UPROPERTY()
    TMap<FName, FRuntimeComponentGroup> DynamicComponentMap;
};
