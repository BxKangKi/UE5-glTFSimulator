// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Runtime/RuntimePlacementTypes.h"
#include "UObject/UnrealType.h"

namespace RuntimePlacementTypesInternal
{
    static FString KindToString(ERuntimePlacedObjectKind Kind)
    {
        switch (Kind)
        {
        case ERuntimePlacedObjectKind::GeneratedMesh:
            return TEXT("GeneratedMesh");
        case ERuntimePlacedObjectKind::Vehicle:
            return TEXT("Vehicle");
        case ERuntimePlacedObjectKind::Prefab:
        default:
            return TEXT("Prefab");
        }
    }

    static ERuntimePlacedObjectKind StringToKind(const FString& Value)
    {
        if (Value.Equals(TEXT("GeneratedMesh"), ESearchCase::IgnoreCase))
        {
            return ERuntimePlacedObjectKind::GeneratedMesh;
        }
        if (Value.Equals(TEXT("Vehicle"), ESearchCase::IgnoreCase))
        {
            return ERuntimePlacedObjectKind::Vehicle;
        }
        return ERuntimePlacedObjectKind::Prefab;
    }
}

void FRuntimePlacementJson::SetVector(const TSharedRef<FJsonObject>& Json, const FString& Prefix, const FVector& Value)
{
    Json->SetNumberField(Prefix + TEXT("X"), Value.X);
    Json->SetNumberField(Prefix + TEXT("Y"), Value.Y);
    Json->SetNumberField(Prefix + TEXT("Z"), Value.Z);
}

bool FRuntimePlacementJson::TryGetVector(const TSharedPtr<FJsonObject>& Json, const FString& Prefix, FVector& OutValue)
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

void FRuntimePlacementJson::SetRotator(const TSharedRef<FJsonObject>& Json, const FString& Prefix, const FRotator& Value)
{
    Json->SetNumberField(Prefix + TEXT("Pitch"), Value.Pitch);
    Json->SetNumberField(Prefix + TEXT("Yaw"), Value.Yaw);
    Json->SetNumberField(Prefix + TEXT("Roll"), Value.Roll);
}

bool FRuntimePlacementJson::TryGetRotator(const TSharedPtr<FJsonObject>& Json, const FString& Prefix, FRotator& OutValue)
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

void FRuntimePlacementJson::SetTransform(const TSharedRef<FJsonObject>& Json, const FTransform& Transform)
{
    TSharedRef<FJsonObject> TransformJson = MakeShared<FJsonObject>();
    SetVector(TransformJson, TEXT("Location"), Transform.GetLocation());
    SetRotator(TransformJson, TEXT("Rotation"), Transform.Rotator());
    SetVector(TransformJson, TEXT("Scale"), Transform.GetScale3D());
    Json->SetObjectField(TEXT("Transform"), TransformJson);
}

bool FRuntimePlacementJson::TryGetTransform(const TSharedPtr<FJsonObject>& Json, FTransform& OutTransform)
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

TSharedRef<FJsonObject> FRuntimePlacedObjectRecord::ToJson() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("RuntimeName"), RuntimeName);
    Json->SetStringField(TEXT("BaseName"), BaseName);
    Json->SetStringField(TEXT("SourceFile"), SourceFile);
    Json->SetStringField(TEXT("Kind"), RuntimePlacementTypesInternal::KindToString(Kind));
    FRuntimePlacementJson::SetTransform(Json, Transform);
    return Json;
}

bool FRuntimePlacedObjectRecord::FromJson(const TSharedPtr<FJsonObject>& Json)
{
    if (!Json.IsValid())
    {
        return false;
    }

    Json->TryGetStringField(TEXT("RuntimeName"), RuntimeName);
    Json->TryGetStringField(TEXT("BaseName"), BaseName);
    Json->TryGetStringField(TEXT("SourceFile"), SourceFile);
    FString KindString;
    if (Json->TryGetStringField(TEXT("Kind"), KindString))
    {
        Kind = RuntimePlacementTypesInternal::StringToKind(KindString);
    }
    FRuntimePlacementJson::TryGetTransform(Json, Transform);
    return !RuntimeName.IsEmpty();
}

TSharedRef<FJsonObject> FRuntimeGeneratedMeshRecord::ToJson() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("RuntimeName"), RuntimeName);
    Json->SetStringField(TEXT("BaseName"), BaseName);
    FRuntimePlacementJson::SetTransform(Json, Transform);

    TArray<TSharedPtr<FJsonValue>> VertexValues;
    VertexValues.Reserve(Vertices.Num());
    for (const FVector& Vertex : Vertices)
    {
        TSharedRef<FJsonObject> VertexJson = MakeShared<FJsonObject>();
        FRuntimePlacementJson::SetVector(VertexJson, TEXT(""), Vertex);
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

bool FRuntimeGeneratedMeshRecord::FromJson(const TSharedPtr<FJsonObject>& Json)
{
    if (!Json.IsValid())
    {
        return false;
    }

    Json->TryGetStringField(TEXT("RuntimeName"), RuntimeName);
    Json->TryGetStringField(TEXT("BaseName"), BaseName);
    FRuntimePlacementJson::TryGetTransform(Json, Transform);

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
                FRuntimePlacementJson::TryGetVector(Value->AsObject(), TEXT(""), Vertex);
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

    return !RuntimeName.IsEmpty();
}
