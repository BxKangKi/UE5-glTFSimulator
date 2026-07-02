// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Gameplay/PlacementTypes.h"
#include "UObject/UnrealType.h"

namespace PlacementTypesInternal
{
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
    if (!Json.IsValid())
    {
        return false;
    }

    double X = OutValue.X;
    double Y = OutValue.Y;
    double Z = OutValue.Z;
    bool bFound = false;
    bFound |= Json->TryGetNumberField(Prefix + TEXT("X"), X);
    bFound |= Json->TryGetNumberField(Prefix + TEXT("Y"), Y);
    bFound |= Json->TryGetNumberField(Prefix + TEXT("Z"), Z);
    OutValue = FVector(X, Y, Z);
    return bFound;
}

void FPlacementJson::SetRotator(const TSharedRef<FJsonObject>& Json, const FString& Prefix, const FRotator& Value)
{
    Json->SetNumberField(Prefix + TEXT("Pitch"), Value.Pitch);
    Json->SetNumberField(Prefix + TEXT("Yaw"), Value.Yaw);
    Json->SetNumberField(Prefix + TEXT("Roll"), Value.Roll);
}

bool FPlacementJson::TryGetRotator(const TSharedPtr<FJsonObject>& Json, const FString& Prefix, FRotator& OutValue)
{
    if (!Json.IsValid())
    {
        return false;
    }

    double Pitch = OutValue.Pitch;
    double Yaw = OutValue.Yaw;
    double Roll = OutValue.Roll;
    bool bFound = false;
    bFound |= Json->TryGetNumberField(Prefix + TEXT("Pitch"), Pitch);
    bFound |= Json->TryGetNumberField(Prefix + TEXT("Yaw"), Yaw);
    bFound |= Json->TryGetNumberField(Prefix + TEXT("Roll"), Roll);
    OutValue = FRotator(Pitch, Yaw, Roll);
    return bFound;
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
    TryGetVector(*TransformJsonPtr, TEXT("Location"), Location);
    TryGetRotator(*TransformJsonPtr, TEXT("Rotation"), Rotation);
    TryGetVector(*TransformJsonPtr, TEXT("Scale"), Scale);
    OutTransform = FTransform(Rotation, Location, Scale);
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

    Json->TryGetStringField(TEXT("ObjectName"), ObjectName);
    Json->TryGetStringField(TEXT("BaseName"), BaseName);
    Json->TryGetStringField(TEXT("SourceFile"), SourceFile);
    FString KindString;
    if (Json->TryGetStringField(TEXT("Kind"), KindString))
    {
        Kind = PlacementTypesInternal::StringToKind(KindString);
    }
    FPlacementJson::TryGetTransform(Json, Transform);
    return !ObjectName.IsEmpty();
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

    Json->TryGetStringField(TEXT("ObjectName"), ObjectName);
    Json->TryGetStringField(TEXT("BaseName"), BaseName);
    FPlacementJson::TryGetTransform(Json, Transform);

    Vertices.Empty();
    const TArray<TSharedPtr<FJsonValue>>* VertexValues = nullptr;
    if (Json->TryGetArrayField(TEXT("Vertices"), VertexValues) && VertexValues)
    {
        Vertices.Reserve(VertexValues->Num());
        for (const TSharedPtr<FJsonValue>& Value : *VertexValues)
        {
            FVector Vertex = FVector::ZeroVector;
            if (Value.IsValid() && Value->Type == EJson::Object)
            {
                FPlacementJson::TryGetVector(Value->AsObject(), TEXT(""), Vertex);
                Vertices.Add(Vertex);
            }
        }
    }

    Triangles.Empty();
    const TArray<TSharedPtr<FJsonValue>>* TriangleValues = nullptr;
    if (Json->TryGetArrayField(TEXT("Triangles"), TriangleValues) && TriangleValues)
    {
        Triangles.Reserve(TriangleValues->Num());
        for (const TSharedPtr<FJsonValue>& Value : *TriangleValues)
        {
            if (Value.IsValid())
            {
                Triangles.Add(static_cast<int32>(Value->AsNumber()));
            }
        }
    }

    return !ObjectName.IsEmpty();
}
