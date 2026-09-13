// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file ModelData.cpp
 * 역할: 모델·메시·노드의 런타임 데이터 구조를 정의합니다.
 * 핵심 기능: JSON 설정 변환, 메시 속성, 경계·노드·LOD 데이터.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "Model/ModelData.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "System/StringHelper.h"
#include "System/JsonHelper.h"
#include "System/MacroLibrary.h"

// ==========================================
// FModelCollider
// ==========================================

TSharedRef<FJsonObject> FModelCollider::Serialization() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    FJsonHelper::SetEnumField<EColliderType>(Json, TEXT("Type"), Collider);
    FJsonHelper::SetVector(Json, Center);
    FJsonHelper::SetVector(Json, Size, TEXT("D"));
    return Json;
}

bool FModelCollider::Deserialization(const TSharedPtr<FJsonObject> &Json)
{
    if (!Json.IsValid())
        return false;

    FString Type;
    FJsonHelper::TryGetEnumField<EColliderType>(Json, TEXT("Type"), Collider);
    FJsonHelper::TryGetVector(Json, Center);
    FJsonHelper::TryGetVector(Json, Size, TEXT("D"));
    return true;
}

// ==========================================
// FLightData
// ==========================================

TSharedRef<FJsonObject> FLightData::Serialization() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    FJsonHelper::SetVector(Json, Location);
    FJsonHelper::SetEnumField<ELightUnits>(Json, TEXT("Unit"), Unit);
    Json->SetNumberField(TEXT("Intensity"), Intensity);
    Json->SetNumberField(TEXT("SourceRadius"), SourceRadius);
    Json->SetNumberField(TEXT("SoftSourceRadius"), SoftSourceRadius);
    Json->SetNumberField(TEXT("AttenuationRadius"), AttenuationRadius);
    Json->SetNumberField(TEXT("Length"), Length);

    return Json;
}

bool FLightData::Deserialization(const TSharedPtr<FJsonObject> &Json)
{
    if (!Json.IsValid())
        return false;

    FJsonHelper::TryGetVector(Json, Location);
    FJsonHelper::TryGetEnumField<ELightUnits>(Json, TEXT("Unit"), Unit);
    Json->TryGetNumberField(TEXT("Intensity"), Intensity);
    Json->TryGetNumberField(TEXT("SourceRadius"), SourceRadius);
    Json->TryGetNumberField(TEXT("SoftSourceRadius"), SoftSourceRadius);
    Json->TryGetNumberField(TEXT("AttenuationRadius"), AttenuationRadius);
    Json->TryGetNumberField(TEXT("Length"), Length);
    return true;
}

// ==========================================
// FMeshData
// ==========================================

TSharedRef<FJsonObject> FMeshData::Serialization() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetBoolField(TEXT("ComplexCollision"), bComplexCollision);
    Json->SetBoolField(TEXT("SimpleCollision"), bSimpleCollision);
    Json->SetBoolField(TEXT("IsEntity"), bIsEntity);

    // Automates TArray struct serialization using a lambda capture.
    FJsonHelper::SetArray<FModelCollider>(Json, TEXT("Colliders"), Colliders, [](const FModelCollider &Item)
                                          { return Item.Serialization(); });

    FJsonHelper::SetArray<FLightData>(Json, TEXT("Lights"), Lights, [](const FLightData &Item)
                                             { return Item.Serialization(); });

    return Json;
}

bool FMeshData::Deserialization(const TSharedPtr<FJsonObject> &Json)
{
    if (!Json.IsValid())
        return false;

    Json->TryGetBoolField(TEXT("ComplexCollision"), bComplexCollision);
    Json->TryGetBoolField(TEXT("SimpleCollision"), bSimpleCollision);
    Json->TryGetBoolField(TEXT("IsEntity"), bIsEntity);

    // Automates TArray struct deserialization.
    FJsonHelper::TryGetArray<FModelCollider>(Json, TEXT("Colliders"), Colliders, [](const TSharedPtr<FJsonObject> &Obj, FModelCollider &OutItem)
                                             { return OutItem.Deserialization(Obj); });

    FJsonHelper::TryGetArray<FLightData>(Json, TEXT("Lights"), Lights, [](const TSharedPtr<FJsonObject> &Obj, FLightData &OutItem)
                                                { return OutItem.Deserialization(Obj); });

    return true;
}

// ==========================================
// FModelData
// ==========================================

TSharedRef<FJsonObject> FModelData::Serialization() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(JSON_VERSION_FIELD, JSON_SCHEMA_VERSION);
    // JSON is external, user-authored input. Program-owned bounds are emitted only into .gwd
    // metadata and must never be mixed into this settings document.
    // Automates TMap struct serialization without duplicate loops.
    FJsonHelper::SetMap<FMeshData>(Json, TEXT("MeshData"), MeshData, [](const FMeshData &Item)
                                          { return Item.Serialization(); });

    return Json;
}

bool FModelData::Deserialization(const TSharedPtr<FJsonObject> &Json)
{
    if (!Json.IsValid())
        return false;

    Center = FVector::ZeroVector;
    Size = FVector::ZeroVector;
    MeshData.Empty();
    // Automates TMap struct deserialization while safely restoring JSON keys as FName values.
    FJsonHelper::TryGetMap<FMeshData>(Json, TEXT("MeshData"), MeshData, [](const TSharedPtr<FJsonObject> &Obj, FMeshData &OutItem)
                                             { return OutItem.Deserialization(Obj); });

    return true;
}

#if WITH_EDITOR
FString FModelMeshData::ToString()
{
    FString Result;
    Result.Append(FStringHelper::ToString(Data.bComplexCollision));
    Result.Append(FStringHelper::ToString(Data.bSimpleCollision));
    Result.Append(FString::FromInt(LOD0));
    Result.Append(FString::FromInt(LOD1));
    Result.Append(FString::FromInt(LOD2));
    return Result;
}
#endif
