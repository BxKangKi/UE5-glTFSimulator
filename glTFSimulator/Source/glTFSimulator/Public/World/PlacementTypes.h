// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Runtime prefab/vehicle placement records and legacy JSON migration helpers.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "PlacementTypes.generated.h"

UENUM(BlueprintType)
enum class EPlacedObjectKind : uint8
{
    Prefab = 0 UMETA(DisplayName = "Prefab"),
    // Value 1 is intentionally reserved for the removed generated-mesh format.
    Vehicle = 2 UMETA(DisplayName = "Vehicle")
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

struct FPlacementJson
{
    static void SetVector(const TSharedRef<FJsonObject>& Json, const FString& Prefix, const FVector& Value);
    static bool TryGetVector(const TSharedPtr<FJsonObject>& Json, const FString& Prefix, FVector& OutValue);
    static void SetRotator(const TSharedRef<FJsonObject>& Json, const FString& Prefix, const FRotator& Value);
    static bool TryGetRotator(const TSharedPtr<FJsonObject>& Json, const FString& Prefix, FRotator& OutValue);
    static void SetTransform(const TSharedRef<FJsonObject>& Json, const FTransform& Transform);
    static bool TryGetTransform(const TSharedPtr<FJsonObject>& Json, FTransform& OutTransform);
};
