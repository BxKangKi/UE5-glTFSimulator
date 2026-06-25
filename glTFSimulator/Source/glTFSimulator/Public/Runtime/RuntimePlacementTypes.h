// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Runtime placement, generated mesh and save data structures.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "RuntimePlacementTypes.generated.h"

UENUM(BlueprintType)
enum class ERuntimePlacedObjectKind : uint8
{
    Prefab UMETA(DisplayName = "Prefab"),
    GeneratedMesh UMETA(DisplayName = "Generated Mesh"),
    Vehicle UMETA(DisplayName = "Vehicle")
};

USTRUCT(BlueprintType)
struct FRuntimePlacedObjectRecord
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite)
    FString RuntimeName;

    UPROPERTY(BlueprintReadWrite)
    FString BaseName;

    UPROPERTY(BlueprintReadWrite)
    FString SourceFile;

    UPROPERTY(BlueprintReadWrite)
    ERuntimePlacedObjectKind Kind = ERuntimePlacedObjectKind::Prefab;

    UPROPERTY(BlueprintReadWrite)
    FTransform Transform = FTransform::Identity;

    TSharedRef<FJsonObject> ToJson() const;
    bool FromJson(const TSharedPtr<FJsonObject>& Json);
};

USTRUCT(BlueprintType)
struct FRuntimeGeneratedMeshRecord
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite)
    FString RuntimeName;

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

struct FRuntimePlacementJson
{
    static void SetVector(const TSharedRef<FJsonObject>& Json, const FString& Prefix, const FVector& Value);
    static bool TryGetVector(const TSharedPtr<FJsonObject>& Json, const FString& Prefix, FVector& OutValue);
    static void SetRotator(const TSharedRef<FJsonObject>& Json, const FString& Prefix, const FRotator& Value);
    static bool TryGetRotator(const TSharedPtr<FJsonObject>& Json, const FString& Prefix, FRotator& OutValue);
    static void SetTransform(const TSharedRef<FJsonObject>& Json, const FTransform& Transform);
    static bool TryGetTransform(const TSharedPtr<FJsonObject>& Json, FTransform& OutTransform);
};
