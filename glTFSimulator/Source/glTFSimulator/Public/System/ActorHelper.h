// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file ActorHelper.h
 * 역할: 공통 액터 생성·조작 기능을 제공합니다.
 * 핵심 기능: deferred spawn 등 액터 보조 연산.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

class UStaticMesh;
class UBoxComponent;
class AActor;

struct FActorHelper
{
    template <typename T>
    static T *SpawnActorDeferred(UWorld *World, UClass *Class, FTransform const &Transform, FActorSpawnParameters Params)
    {
        if (!IsValid(World) || !IsValid(Class))
        {
            return nullptr;
        }
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
        // Compile-time check that T derives from UStaticMeshComponent.
        static_assert(TIsDerivedFrom<T, UStaticMeshComponent>::Value, "T must be a UStaticMeshComponent");
        if (!IsValid(Actor) || !IsValid(Actor->GetRootComponent()))
        {
            return nullptr;
        }

        T *StaticMesh = NewObject<T>(Actor);
        if (!IsValid(StaticMesh))
        {
            return nullptr;
        }

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
