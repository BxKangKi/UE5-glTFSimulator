// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "System/ActorHelper.h"
#include "GameFramework/Actor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/BoxComponent.h"

void FActorHelper::DestroyComponent(AActor *Actor, UActorComponent *Comp)
{
    Actor->RemoveInstanceComponent(Comp);
    Comp->UnregisterComponent();
    Comp->DestroyComponent();
}

UBoxComponent *FActorHelper::AddBoxComponent(AActor *Actor, const FTransform &Transform, const FVector &Size, const FName &Profile)
{
    if (!IsValid(Actor))
        return nullptr;
    UBoxComponent *BoxCollider = NewObject<UBoxComponent>(Actor);
    Actor->AddInstanceComponent(BoxCollider);
    // 박스 콜라이더를 루트 컴포넌트에 붙임
    BoxCollider->SetupAttachment(Actor->GetRootComponent());
    BoxCollider->SetWorldTransform(Transform);
    // 콜라이더 크기 설정 (예: 100x100x100)
    BoxCollider->InitBoxExtent(Size);
    // 콜라이더 활성화 설정
    BoxCollider->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    BoxCollider->SetCollisionProfileName(Profile);
    BoxCollider->RegisterComponent();
    return BoxCollider;
}

void FActorHelper::ChangeParent(USceneComponent *Child,
                                             USceneComponent *Parent,
                                             const FDetachmentTransformRules &DetachRules,
                                             const FAttachmentTransformRules &AttachRules)
{
    Child->DetachFromComponent(DetachRules);
    Child->AttachToComponent(Parent, AttachRules, NAME_None);
}

void FActorHelper::DetachParent(USceneComponent *Child, const FDetachmentTransformRules &DetachRules)
{
    Child->DetachFromComponent(DetachRules);
}

void FActorHelper::AttachParent(USceneComponent *Child,
                                             USceneComponent *Parent,
                                             const FAttachmentTransformRules &AttachRules)
{
    Child->AttachToComponent(Parent, AttachRules, NAME_None);
}