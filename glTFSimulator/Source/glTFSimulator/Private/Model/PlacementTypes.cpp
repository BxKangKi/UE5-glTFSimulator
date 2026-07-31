// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "World/PlacementTypes.h"
#include "UObject/UnrealType.h"
#include "System/MacroLibrary.h"

namespace PlacementTypesInternal
{
    // Generated-mesh JSON is user-controlled save data. These limits are intentionally below
    // int32/TArray limits so malformed manifests cannot request multi-gigabyte allocations.
    constexpr int32 MaxRecordNameCharacters = 1024;
    constexpr int32 MaxSourcePathCharacters = 32768;

    static bool IsFiniteBoundedNumber(const double Value)
    {
        return FMath::IsFinite(Value) &&
            FMath::Abs(Value) <= static_cast<double>(WORLD_MAX_SIZE);
    }

    static bool IsFiniteBoundedVector(const FVector& Value)
    {
        return IsFiniteBoundedNumber(Value.X) &&
            IsFiniteBoundedNumber(Value.Y) &&
            IsFiniteBoundedNumber(Value.Z);
    }

    static bool IsFiniteTransform(const FTransform& Transform)
    {
        const FQuat Rotation = Transform.GetRotation();
        return IsFiniteBoundedVector(Transform.GetLocation()) &&
            IsFiniteBoundedVector(Transform.GetScale3D()) &&
            FMath::IsFinite(Rotation.X) &&
            FMath::IsFinite(Rotation.Y) &&
            FMath::IsFinite(Rotation.Z) &&
            FMath::IsFinite(Rotation.W) &&
            Rotation.IsNormalized();
    }

    /**
     * Reads one optional number without conflating a missing field with a present field of the
     * wrong type. Existing manifests may omit individual transform components, but any component
     * that is present must be a finite, world-bounded number.
     */
    static bool TryReadOptionalFiniteNumber(
        const TSharedPtr<FJsonObject>& Json,
        const FString& FieldName,
        double& InOutValue,
        bool& bOutFound)
    {
        if (!Json.IsValid())
        {
            return false;
        }
        if (!Json->HasField(FieldName))
        {
            return true;
        }

        double ParsedValue = 0.0;
        if (!Json->TryGetNumberField(FieldName, ParsedValue) ||
            !IsFiniteBoundedNumber(ParsedValue))
        {
            return false;
        }

        InOutValue = ParsedValue;
        bOutFound = true;
        return true;
    }

    static bool TryReadFiniteVector(
        const TSharedPtr<FJsonObject>& Json,
        const FString& Prefix,
        FVector& OutValue,
        bool& bOutFound,
        const bool bRequireAllComponents)
    {
        double X = OutValue.X;
        double Y = OutValue.Y;
        double Z = OutValue.Z;
        bool bFoundX = false;
        bool bFoundY = false;
        bool bFoundZ = false;

        if (!TryReadOptionalFiniteNumber(Json, Prefix + TEXT("X"), X, bFoundX) ||
            !TryReadOptionalFiniteNumber(Json, Prefix + TEXT("Y"), Y, bFoundY) ||
            !TryReadOptionalFiniteNumber(Json, Prefix + TEXT("Z"), Z, bFoundZ))
        {
            return false;
        }

        bOutFound = bFoundX || bFoundY || bFoundZ;
        if (bRequireAllComponents && (!bFoundX || !bFoundY || !bFoundZ))
        {
            return false;
        }
        if (!bOutFound)
        {
            return true;
        }

        const FVector ParsedValue(X, Y, Z);
        if (!IsFiniteBoundedVector(ParsedValue))
        {
            return false;
        }
        OutValue = ParsedValue;
        return true;
    }

    static bool TryReadFiniteRotator(
        const TSharedPtr<FJsonObject>& Json,
        const FString& Prefix,
        FRotator& OutValue,
        bool& bOutFound)
    {
        double Pitch = OutValue.Pitch;
        double Yaw = OutValue.Yaw;
        double Roll = OutValue.Roll;
        bool bFoundPitch = false;
        bool bFoundYaw = false;
        bool bFoundRoll = false;

        if (!TryReadOptionalFiniteNumber(Json, Prefix + TEXT("Pitch"), Pitch, bFoundPitch) ||
            !TryReadOptionalFiniteNumber(Json, Prefix + TEXT("Yaw"), Yaw, bFoundYaw) ||
            !TryReadOptionalFiniteNumber(Json, Prefix + TEXT("Roll"), Roll, bFoundRoll))
        {
            return false;
        }

        bOutFound = bFoundPitch || bFoundYaw || bFoundRoll;
        if (bOutFound)
        {
            OutValue = FRotator(Pitch, Yaw, Roll);
        }
        return true;
    }

    static bool TryReadOptionalString(
        const TSharedPtr<FJsonObject>& Json,
        const FString& FieldName,
        const int32 MaxCharacters,
        FString& OutValue)
    {
        if (!Json.IsValid())
        {
            return false;
        }
        if (!Json->HasField(FieldName))
        {
            OutValue.Reset();
            return true;
        }
        return Json->TryGetStringField(FieldName, OutValue) &&
            OutValue.Len() <= MaxCharacters;
    }

    static bool IsGeneratedMeshTopologyValid(
        const TArray<FVector>& Vertices,
        const TArray<int32>& TriangleIndices)
    {
        if (Vertices.Num() < 3 ||
            Vertices.Num() > PlacementSafetyLimits::MaxGeneratedMeshVertices ||
            TriangleIndices.Num() < 3 ||
            TriangleIndices.Num() > PlacementSafetyLimits::MaxGeneratedMeshTriangleIndices ||
            (TriangleIndices.Num() % 3) != 0)
        {
            return false;
        }

        for (const FVector& Vertex : Vertices)
        {
            if (!IsFiniteBoundedVector(Vertex))
            {
                return false;
            }
        }

        for (int32 TriangleOffset = 0;
            TriangleOffset < TriangleIndices.Num();
            TriangleOffset += 3)
        {
            const int32 A = TriangleIndices[TriangleOffset];
            const int32 B = TriangleIndices[TriangleOffset + 1];
            const int32 C = TriangleIndices[TriangleOffset + 2];
            if (!Vertices.IsValidIndex(A) ||
                !Vertices.IsValidIndex(B) ||
                !Vertices.IsValidIndex(C) ||
                A == B || B == C || C == A)
            {
                return false;
            }

            const FVector Cross = FVector::CrossProduct(Vertices[B] - Vertices[A], Vertices[C] - Vertices[A]);
            const double CrossSizeSquared = Cross.SizeSquared();
            if (!FMath::IsFinite(CrossSizeSquared) || CrossSizeSquared <= 1.0)
            {
                return false;
            }
        }
        return true;
    }

    static FString KindToString(EPlacedObjectKind Kind)
    {
        switch (Kind)
        {
        case EPlacedObjectKind::GeneratedMesh:
            return TEXT("GeneratedMesh");
        case EPlacedObjectKind::Vehicle:
            return TEXT("Vehicle");
        case EPlacedObjectKind::Prefab:
        default:
            return TEXT("Prefab");
        }
    }

    static EPlacedObjectKind StringToKind(const FString& Value)
    {
        if (Value.Equals(TEXT("GeneratedMesh"), ESearchCase::IgnoreCase))
        {
            return EPlacedObjectKind::GeneratedMesh;
        }
        if (Value.Equals(TEXT("Vehicle"), ESearchCase::IgnoreCase))
        {
            return EPlacedObjectKind::Vehicle;
        }
        return EPlacedObjectKind::Prefab;
    }
}

void FPlacementJson::SetVector(const TSharedRef<FJsonObject>& Json, const FString& Prefix, const FVector& Value)
{
    Json->SetNumberField(Prefix + TEXT("X"), Value.X);
    Json->SetNumberField(Prefix + TEXT("Y"), Value.Y);
    Json->SetNumberField(Prefix + TEXT("Z"), Value.Z);
}

bool FPlacementJson::TryGetVector(const TSharedPtr<FJsonObject>& Json, const FString& Prefix, FVector& OutValue)
{
    bool bFound = false;
    return PlacementTypesInternal::TryReadFiniteVector(
        Json, Prefix, OutValue, bFound, false) && bFound;
}

void FPlacementJson::SetRotator(const TSharedRef<FJsonObject>& Json, const FString& Prefix, const FRotator& Value)
{
    Json->SetNumberField(Prefix + TEXT("Pitch"), Value.Pitch);
    Json->SetNumberField(Prefix + TEXT("Yaw"), Value.Yaw);
    Json->SetNumberField(Prefix + TEXT("Roll"), Value.Roll);
}

bool FPlacementJson::TryGetRotator(const TSharedPtr<FJsonObject>& Json, const FString& Prefix, FRotator& OutValue)
{
    bool bFound = false;
    return PlacementTypesInternal::TryReadFiniteRotator(
        Json, Prefix, OutValue, bFound) && bFound;
}

void FPlacementJson::SetTransform(const TSharedRef<FJsonObject>& Json, const FTransform& Transform)
{
    TSharedRef<FJsonObject> TransformJson = MakeShared<FJsonObject>();
    SetVector(TransformJson, TEXT("Location"), Transform.GetLocation());
    SetRotator(TransformJson, TEXT("Rotation"), Transform.Rotator());
    SetVector(TransformJson, TEXT("Scale"), Transform.GetScale3D());
    Json->SetObjectField(TEXT("Transform"), TransformJson);
}

bool FPlacementJson::TryGetTransform(const TSharedPtr<FJsonObject>& Json, FTransform& OutTransform)
{
    if (!Json.IsValid())
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* TransformJsonPtr = nullptr;
    if (!Json->TryGetObjectField(TEXT("Transform"), TransformJsonPtr) || !TransformJsonPtr || !TransformJsonPtr->IsValid())
    {
        return false;
    }

    FVector Location = OutTransform.GetLocation();
    FVector Scale = OutTransform.GetScale3D();
    FRotator Rotation = OutTransform.Rotator();
    bool bFoundLocation = false;
    bool bFoundRotation = false;
    bool bFoundScale = false;
    if (!PlacementTypesInternal::TryReadFiniteVector(
            *TransformJsonPtr, TEXT("Location"), Location, bFoundLocation, false) ||
        !PlacementTypesInternal::TryReadFiniteRotator(
            *TransformJsonPtr, TEXT("Rotation"), Rotation, bFoundRotation) ||
        !PlacementTypesInternal::TryReadFiniteVector(
            *TransformJsonPtr, TEXT("Scale"), Scale, bFoundScale, false))
    {
        return false;
    }

    const FTransform ParsedTransform(Rotation, Location, Scale);
    if (!PlacementTypesInternal::IsFiniteTransform(ParsedTransform))
    {
        return false;
    }
    OutTransform = ParsedTransform;
    return true;
}

TSharedRef<FJsonObject> FPlacedObjectRecord::ToJson() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("ObjectName"), ObjectName);
    Json->SetStringField(TEXT("BaseName"), BaseName);
    Json->SetStringField(TEXT("SourceFile"), SourceFile);
    Json->SetStringField(TEXT("Kind"), PlacementTypesInternal::KindToString(Kind));
    FPlacementJson::SetTransform(Json, Transform);
    return Json;
}

bool FPlacedObjectRecord::FromJson(const TSharedPtr<FJsonObject>& Json)
{
    if (!Json.IsValid())
    {
        return false;
    }

    FString ParsedObjectName;
    FString ParsedBaseName;
    FString ParsedSourceFile;
    if (!PlacementTypesInternal::TryReadOptionalString(
            Json, TEXT("ObjectName"), PlacementTypesInternal::MaxRecordNameCharacters, ParsedObjectName) ||
        ParsedObjectName.IsEmpty() ||
        !PlacementTypesInternal::TryReadOptionalString(
            Json, TEXT("BaseName"), PlacementTypesInternal::MaxRecordNameCharacters, ParsedBaseName) ||
        !PlacementTypesInternal::TryReadOptionalString(
            Json, TEXT("SourceFile"), PlacementTypesInternal::MaxSourcePathCharacters, ParsedSourceFile))
    {
        return false;
    }

    FString KindString;
    EPlacedObjectKind ParsedKind = EPlacedObjectKind::Prefab;
    if (!PlacementTypesInternal::TryReadOptionalString(Json, TEXT("Kind"), 64, KindString))
    {
        return false;
    }
    if (!KindString.IsEmpty())
    {
        ParsedKind = PlacementTypesInternal::StringToKind(KindString);
    }

    FTransform ParsedTransform = FTransform::Identity;
    if (Json->HasField(TEXT("Transform")) &&
        !FPlacementJson::TryGetTransform(Json, ParsedTransform))
    {
        return false;
    }

    ObjectName = MoveTemp(ParsedObjectName);
    BaseName = MoveTemp(ParsedBaseName);
    SourceFile = MoveTemp(ParsedSourceFile);
    Kind = ParsedKind;
    Transform = ParsedTransform;
    return true;
}

TSharedRef<FJsonObject> FGeneratedMeshRecord::ToJson() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("ObjectName"), ObjectName);
    Json->SetStringField(TEXT("BaseName"), BaseName);
    FPlacementJson::SetTransform(Json, Transform);

    TArray<TSharedPtr<FJsonValue>> VertexValues;
    VertexValues.Reserve(Vertices.Num());
    for (const FVector& Vertex : Vertices)
    {
        TSharedRef<FJsonObject> VertexJson = MakeShared<FJsonObject>();
        FPlacementJson::SetVector(VertexJson, TEXT(""), Vertex);
        VertexValues.Add(MakeShared<FJsonValueObject>(VertexJson));
    }
    Json->SetArrayField(TEXT("Vertices"), VertexValues);

    TArray<TSharedPtr<FJsonValue>> TriangleValues;
    TriangleValues.Reserve(Triangles.Num());
    for (const int32 Index : Triangles)
    {
        TriangleValues.Add(MakeShared<FJsonValueNumber>(Index));
    }
    Json->SetArrayField(TEXT("Triangles"), TriangleValues);
    return Json;
}

bool FGeneratedMeshRecord::FromJson(const TSharedPtr<FJsonObject>& Json)
{
    if (!Json.IsValid())
    {
        return false;
    }

    FString ParsedObjectName;
    FString ParsedBaseName;
    if (!PlacementTypesInternal::TryReadOptionalString(
            Json, TEXT("ObjectName"), PlacementTypesInternal::MaxRecordNameCharacters, ParsedObjectName) ||
        ParsedObjectName.IsEmpty() ||
        !PlacementTypesInternal::TryReadOptionalString(
            Json, TEXT("BaseName"), PlacementTypesInternal::MaxRecordNameCharacters, ParsedBaseName))
    {
        return false;
    }

    FTransform ParsedTransform = FTransform::Identity;
    if (Json->HasField(TEXT("Transform")) &&
        !FPlacementJson::TryGetTransform(Json, ParsedTransform))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* VertexValues = nullptr;
    if (!Json->TryGetArrayField(TEXT("Vertices"), VertexValues) ||
        !VertexValues ||
        VertexValues->Num() < 3 ||
        VertexValues->Num() > PlacementSafetyLimits::MaxGeneratedMeshVertices)
    {
        return false;
    }

    TArray<FVector> ParsedVertices;
    ParsedVertices.Reserve(VertexValues->Num());
    for (const TSharedPtr<FJsonValue>& Value : *VertexValues)
    {
        if (!Value.IsValid() || Value->Type != EJson::Object)
        {
            return false;
        }

        FVector Vertex = FVector::ZeroVector;
        bool bFoundVertex = false;
        if (!PlacementTypesInternal::TryReadFiniteVector(
                Value->AsObject(), TEXT(""), Vertex, bFoundVertex, true) ||
            !bFoundVertex)
        {
            return false;
        }
        ParsedVertices.Add(Vertex);
    }

    const TArray<TSharedPtr<FJsonValue>>* TriangleValues = nullptr;
    if (!Json->TryGetArrayField(TEXT("Triangles"), TriangleValues) ||
        !TriangleValues ||
        TriangleValues->Num() < 3 ||
        TriangleValues->Num() > PlacementSafetyLimits::MaxGeneratedMeshTriangleIndices ||
        (TriangleValues->Num() % 3) != 0)
    {
        return false;
    }

    TArray<int32> ParsedTriangles;
    ParsedTriangles.Reserve(TriangleValues->Num());
    for (const TSharedPtr<FJsonValue>& Value : *TriangleValues)
    {
        if (!Value.IsValid() || Value->Type != EJson::Number)
        {
            return false;
        }

        const double NumericIndex = Value->AsNumber();
        if (!FMath::IsFinite(NumericIndex) ||
            NumericIndex < 0.0 ||
            NumericIndex >= static_cast<double>(ParsedVertices.Num()))
        {
            return false;
        }

        // The range check above makes this cast safe; comparing it back rejects fractional values.
        const int32 IntegralIndex = static_cast<int32>(NumericIndex);
        if (static_cast<double>(IntegralIndex) != NumericIndex)
        {
            return false;
        }
        ParsedTriangles.Add(IntegralIndex);
    }

    if (!PlacementTypesInternal::IsGeneratedMeshTopologyValid(
            ParsedVertices, ParsedTriangles))
    {
        return false;
    }

    ObjectName = MoveTemp(ParsedObjectName);
    BaseName = MoveTemp(ParsedBaseName);
    Transform = ParsedTransform;
    Vertices = MoveTemp(ParsedVertices);
    Triangles = MoveTemp(ParsedTriangles);
    return true;
}
