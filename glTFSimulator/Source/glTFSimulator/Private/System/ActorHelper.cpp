// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "System/ActorHelper.h"
#include "GameFramework/Actor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/BoxComponent.h"

void FActorHelper::DestroyComponent(AActor *Actor, UActorComponent *Comp)
{
    if (!IsValid(Actor) || !IsValid(Comp))
    {
        return;
    }

    // Remove, unregister, and destroy in one guarded helper so callers do not duplicate lifecycle checks.
    Actor->RemoveInstanceComponent(Comp);
    Comp->UnregisterComponent();
    Comp->DestroyComponent();
}

UBoxComponent *FActorHelper::AddBoxComponent(AActor *Actor, const FTransform &Transform, const FVector &Size, const FName &Profile)
{
    if (!IsValid(Actor) || !IsValid(Actor->GetRootComponent()))
    {
        return nullptr;
    }

    UBoxComponent *BoxCollider = NewObject<UBoxComponent>(Actor);
    if (!IsValid(BoxCollider))
    {
        return nullptr;
    }

    Actor->AddInstanceComponent(BoxCollider);
    BoxCollider->SetupAttachment(Actor->GetRootComponent());
    BoxCollider->SetWorldTransform(Transform);
    BoxCollider->InitBoxExtent(Size);
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
    if (!IsValid(Child) || !IsValid(Parent))
    {
        return;
    }

    Child->DetachFromComponent(DetachRules);
    Child->AttachToComponent(Parent, AttachRules, NAME_None);
}

void FActorHelper::DetachParent(USceneComponent *Child, const FDetachmentTransformRules &DetachRules)
{
    if (!IsValid(Child))
    {
        return;
    }

    Child->DetachFromComponent(DetachRules);
}

void FActorHelper::AttachParent(USceneComponent *Child,
                                             USceneComponent *Parent,
                                             const FAttachmentTransformRules &AttachRules)
{
    if (!IsValid(Child) || !IsValid(Parent))
    {
        return;
    }

    Child->AttachToComponent(Parent, AttachRules, NAME_None);
}