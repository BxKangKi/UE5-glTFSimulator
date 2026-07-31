// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Placement, generated mesh and save data structures.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "PlacementTypes.generated.h"

/** Shared limits for user-authored procedural mesh data across parse, save, and render paths. */
namespace PlacementSafetyLimits
{
    inline constexpr int32 MaxGeneratedMeshVertices = 250000;
    inline constexpr int32 MaxGeneratedMeshTriangles = 250000;
    inline constexpr int32 MaxGeneratedMeshTriangleIndices = MaxGeneratedMeshTriangles * 3;
}

UENUM(BlueprintType)
enum class EPlacedObjectKind : uint8
{
    Prefab UMETA(DisplayName = "Prefab"),
    GeneratedMesh UMETA(DisplayName = "Generated Mesh"),
    Vehicle UMETA(DisplayName = "Vehicle")
};

USTRUCT(BlueprintType)
struct FPlacedObjectRecord
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite)
    FString ObjectName;

    UPROPERTY(BlueprintReadWrite)
    FString BaseName;

    UPROPERTY(BlueprintReadWrite)
    FString SourceFile;

    UPROPERTY(BlueprintReadWrite)
    EPlacedObjectKind Kind = EPlacedObjectKind::Prefab;

    UPROPERTY(BlueprintReadWrite)
    FTransform Transform = FTransform::Identity;

    TSharedRef<FJsonObject> ToJson() const;
    bool FromJson(const TSharedPtr<FJsonObject>& Json);
};

USTRUCT(BlueprintType)
struct FGeneratedMeshRecord
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite)
    FString ObjectName;

    UPROPERTY(BlueprintReadWrite)
    FString BaseName;

    UPROPERTY(BlueprintReadWrite)
    FTransform Transform = FTransform::Identity;

    UPROPERTY(BlueprintReadWrite)
    TArray<FVector> Vertices;

    UPROPERTY(BlueprintReadWrite)
    TArray<int32> Triangles;

    TSharedRef<FJsonObject> ToJson() const;
    bool FromJson(const TSharedPtr<FJsonObject>& Json);
};

struct FPlacementJson
{
    static void SetVector(const TSharedRef<FJsonObject>& Json, const FString& Prefix, const FVector& Value);
    static bool TryGetVector(const TSharedPtr<FJsonObject>& Json, const FString& Prefix, FVector& OutValue);
    static void SetRotator(const TSharedRef<FJsonObject>& Json, const FString& Prefix, const FRotator& Value);
    static bool TryGetRotator(const TSharedPtr<FJsonObject>& Json, const FString& Prefix, FRotator& OutValue);
    static void SetTransform(const TSharedRef<FJsonObject>& Json, const FTransform& Transform);
    static bool TryGetTransform(const TSharedPtr<FJsonObject>& Json, FTransform& OutTransform);
};
