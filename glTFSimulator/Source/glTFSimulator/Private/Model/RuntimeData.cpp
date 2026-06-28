// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Model/RuntimeData.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "System/StringHelper.h"
#include "System/JsonHelper.h"

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
// FRuntimeLightData
// ==========================================

TSharedRef<FJsonObject> FRuntimeLightData::Serialization() const
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

bool FRuntimeLightData::Deserialization(const TSharedPtr<FJsonObject> &Json)
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
// FRuntimeMeshData
// ==========================================

TSharedRef<FJsonObject> FRuntimeMeshData::Serialization() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetBoolField(TEXT("ComplexCollision"), bComplexCollision);
    Json->SetBoolField(TEXT("SimpleCollision"), bSimpleCollision);
    Json->SetBoolField(TEXT("IsEntity"), bIsEntity);

    // TArray 구조체 직렬화 자동화 적용 (람다 캡처 이용)
    FJsonHelper::SetArray<FModelCollider>(Json, TEXT("Colliders"), Colliders, [](const FModelCollider &Item)
                                          { return Item.Serialization(); });

    FJsonHelper::SetArray<FRuntimeLightData>(Json, TEXT("Lights"), Lights, [](const FRuntimeLightData &Item)
                                             { return Item.Serialization(); });

    return Json;
}

bool FRuntimeMeshData::Deserialization(const TSharedPtr<FJsonObject> &Json)
{
    if (!Json.IsValid())
        return false;

    Json->TryGetBoolField(TEXT("ComplexCollision"), bComplexCollision);
    Json->TryGetBoolField(TEXT("SimpleCollision"), bSimpleCollision);
    Json->TryGetBoolField(TEXT("IsEntity"), bIsEntity);

    // TArray 구조체 역직렬화 자동화 적용
    FJsonHelper::TryGetArray<FModelCollider>(Json, TEXT("Colliders"), Colliders, [](const TSharedPtr<FJsonObject> &Obj, FModelCollider &OutItem)
                                             { return OutItem.Deserialization(Obj); });

    FJsonHelper::TryGetArray<FRuntimeLightData>(Json, TEXT("Lights"), Lights, [](const TSharedPtr<FJsonObject> &Obj, FRuntimeLightData &OutItem)
                                                { return OutItem.Deserialization(Obj); });

    return true;
}

// ==========================================
// FRuntimeModelData
// ==========================================

TSharedRef<FJsonObject> FRuntimeModelData::Serialization() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    FJsonHelper::SetVector(Json, Center);

    // TMap 구조체 직렬화 자동화 적용 (이제 중복 루프 없이 한 줄로 처리됩니다)
    FJsonHelper::SetMap<FRuntimeMeshData>(Json, TEXT("MeshData"), MeshData, [](const FRuntimeMeshData &Item)
                                          { return Item.Serialization(); });

    return Json;
}

bool FRuntimeModelData::Deserialization(const TSharedPtr<FJsonObject> &Json)
{
    if (!Json.IsValid())
        return false;

    MeshData.Empty();
    FJsonHelper::TryGetVector(Json, Center);

    // TMap 구조체 역직렬화 자동화 적용 (JSON의 Key가 FName으로 안전하게 복원됩니다)
    FJsonHelper::TryGetMap<FRuntimeMeshData>(Json, TEXT("MeshData"), MeshData, [](const TSharedPtr<FJsonObject> &Obj, FRuntimeMeshData &OutItem)
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