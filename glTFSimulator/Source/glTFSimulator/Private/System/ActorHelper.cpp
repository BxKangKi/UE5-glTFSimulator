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
    // Attach the box collider to the root component.
    BoxCollider->SetupAttachment(Actor->GetRootComponent());
    BoxCollider->SetWorldTransform(Transform);
    // Set the collider size, for example 100x100x100.
    BoxCollider->InitBoxExtent(Size);
    // Enable the collider.
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