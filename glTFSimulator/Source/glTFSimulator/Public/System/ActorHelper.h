// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"

class UStaticMesh;
class UBoxComponent;
class AActor;

struct FActorHelper
{
    template <typename T>
    static T *SpawnActorDeferred(UWorld *World, UClass *Class, FTransform const &Transform, FActorSpawnParameters Params)
    {
        if (!IsValid(World) || !IsValid(Class))
            return nullptr;
        static_assert(TIsDerivedFrom<T, AActor>::Value, "T must be a AActor");
        return World->SpawnActorDeferred<T>(Class, Transform, Params.Owner, Params.Instigator, Params.SpawnCollisionHandlingOverride);
    }

    template <typename T>
    static T *AddStaticMeshComponent(
        AActor *Actor,
        const FTransform &Transform,
        UStaticMesh *Mesh = nullptr,
        const ECollisionEnabled::Type &Collision = ECollisionEnabled::QueryAndPhysics,
        const ECollisionResponse &Response = ECR_Block)
    {
        // T가 UStaticMeshComponent를 상속받았는지 컴파일 타임에 체크
        static_assert(TIsDerivedFrom<T, UStaticMeshComponent>::Value, "T must be a UStaticMeshComponent");
        if (!IsValid(Actor))
            return nullptr;
        T *StaticMesh = NewObject<T>(Actor);
        if (!StaticMesh)
            return nullptr;
        Actor->AddInstanceComponent(StaticMesh);
        StaticMesh->SetupAttachment(Actor->GetRootComponent());
        StaticMesh->SetWorldTransform(Transform);
        if (IsValid(Mesh))
        {
            StaticMesh->SetStaticMesh(Mesh);
        }
        StaticMesh->SetCollisionEnabled(Collision);
        StaticMesh->SetCollisionResponseToAllChannels(Response);
        StaticMesh->RegisterComponent();
        return StaticMesh;
    }

    // Destroy Any Component
    static void DestroyComponent(AActor *Actor, UActorComponent *Comp);
    // Add Box Component
    static UBoxComponent *AddBoxComponent(
        AActor *Actor,
        const FTransform &Transform,
        const FVector &Size = FVector::ZeroVector,
        const FName &Profile = TEXT("BlockAll"));

    static void ChangeParent(USceneComponent *Child,
                             USceneComponent *Parent,
                             const FDetachmentTransformRules &DetachRules,
                             const FAttachmentTransformRules &AttachRules);

    static void DetachParent(USceneComponent *Child, const FDetachmentTransformRules &DetachRules);

    static void AttachParent(USceneComponent *Child,
                             USceneComponent *Parent,
                             const FAttachmentTransformRules &AttachRules);
};