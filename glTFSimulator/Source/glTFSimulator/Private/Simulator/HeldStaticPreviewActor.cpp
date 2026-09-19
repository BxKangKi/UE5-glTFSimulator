/**
 * @file HeldStaticPreviewActor.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Simulator/HeldStaticPreviewActor.h"
#include "Model/StaticActor.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "TimerManager.h"

ASimulatorHeldStaticPreviewActor::ASimulatorHeldStaticPreviewActor()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = false;
    SetReplicateMovement(false);
    PreviewRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PreviewRoot"));
    SetRootComponent(PreviewRoot);
}

ASimulatorHeldStaticPreviewActor* ASimulatorHeldStaticPreviewActor::SpawnHeldPreview(UObject* WorldContextObject, ACharacter* Holder,
    TSubclassOf<AStaticActor> StaticVisualClass, const FString& CanonicalModelReference,
    const FSimulatorCharacterInteractionConfig& CharacterConfig, const FSimulatorEquipmentInteractionConfig& EquipmentConfig)
{
    if (!IsValid(WorldContextObject) || !IsValid(Holder)) return nullptr;
    UClass* VisualClass = StaticVisualClass.Get();
    if (!IsValid(VisualClass) || !VisualClass->IsChildOf(AStaticActor::StaticClass())
        || VisualClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
    {
        VisualClass = AStaticActor::StaticClass();
    }
    UWorld* World = WorldContextObject->GetWorld(); if (!IsValid(World)) return nullptr;
    FActorSpawnParameters WrapperParams; WrapperParams.Owner = Holder; WrapperParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn; WrapperParams.ObjectFlags |= RF_Transient;
    ASimulatorHeldStaticPreviewActor* Wrapper = World->SpawnActor<ASimulatorHeldStaticPreviewActor>(StaticClass(), Holder->GetActorTransform(), WrapperParams);
    if (!IsValid(Wrapper)) return nullptr;
    FActorSpawnParameters VisualParams; VisualParams.Owner = Wrapper; VisualParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn; VisualParams.ObjectFlags |= RF_Transient;
    AStaticActor* Visual = World->SpawnActor<AStaticActor>(VisualClass, FTransform::Identity, VisualParams);
    if (!Wrapper->InitializePreview(Holder, Visual, CanonicalModelReference, CharacterConfig, EquipmentConfig))
    {
        Wrapper->Destroy(); return nullptr;
    }
    return Wrapper;
}

bool ASimulatorHeldStaticPreviewActor::InitializePreview(ACharacter* Holder, AStaticActor* SpawnedVisualActor, const FString& CanonicalModelReference,
    const FSimulatorCharacterInteractionConfig& CharacterConfig, const FSimulatorEquipmentInteractionConfig& EquipmentConfig)
{
    if (!IsValid(Holder) || !IsValid(SpawnedVisualActor)) return false;
    HolderWeak = Holder; VisualActor = SpawnedVisualActor; StaticActorClass = SpawnedVisualActor->GetClass(); ModelReference = CanonicalModelReference;
    StoredCharacterConfig = CharacterConfig; StoredEquipmentConfig = EquipmentConfig; StoredEquipmentConfig.Sanitize();
    VisualActor->SetReplicates(false); VisualActor->SetReplicateMovement(false); VisualActor->SetActorEnableCollision(false);
    VisualActor->AttachToComponent(PreviewRoot, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
    VisualActor->SetRenderOnlyStreaming(true);
    if (!VisualActor->LoadStatic(ModelReference, TEXT("HeldStaticPreview")))
    {
        return false;
    }
    ConfigureVisualForPreview(); AttachToResolvedHand(); RemainingBoundsAttempts = 120;
    GetWorldTimerManager().SetTimer(BoundsRetryTimer, this, &ASimulatorHeldStaticPreviewActor::TryFinalizeBounds, 0.05f, true, 0.0f);
    TryFinalizeBounds(); return true;
}

void ASimulatorHeldStaticPreviewActor::ConfigureVisualForPreview()
{
    if (!IsValid(VisualActor)) return;
    TInlineComponentArray<UPrimitiveComponent*> Primitives; VisualActor->GetComponents(Primitives);
    for (UPrimitiveComponent* Primitive : Primitives)
    {
        if (!IsValid(Primitive)) continue;
        Primitive->SetCollisionEnabled(ECollisionEnabled::NoCollision); Primitive->SetGenerateOverlapEvents(false);
        Primitive->SetSimulatePhysics(false); Primitive->SetEnableGravity(false); Primitive->SetCanEverAffectNavigation(false);
    }
}

void ASimulatorHeldStaticPreviewActor::AttachToResolvedHand()
{
    ACharacter* Holder = HolderWeak.Get(); if (!IsValid(Holder) || !IsValid(Holder->GetMesh())) return;
    const ESimulatorHand Hand = StoredEquipmentConfig.ResolvePrimaryHand(StoredCharacterConfig.DominantHand);
    const FSimulatorGripPoint* Grip = StoredEquipmentConfig.GetGrip(Hand);
    FName Socket = Grip && !Grip->CharacterSocket.IsNone() ? Grip->CharacterSocket : (Hand == ESimulatorHand::Right ? StoredCharacterConfig.RightHandSocket : StoredCharacterConfig.LeftHandSocket);
    AttachToComponent(Holder->GetMesh(), FAttachmentTransformRules::SnapToTargetNotIncludingScale, Socket);
    SetActorRelativeTransform(Grip ? Grip->AttachmentOffset : FTransform::Identity);
}

FBox ASimulatorHeldStaticPreviewActor::CalculateVisualBounds() const
{
    FBox Bounds(ForceInit); if (!IsValid(VisualActor)) return Bounds;
    TInlineComponentArray<UPrimitiveComponent*> Primitives; VisualActor->GetComponents(Primitives);
    for (const UPrimitiveComponent* Primitive : Primitives)
    {
        if (IsValid(Primitive) && Primitive->IsRegistered()) Bounds += Primitive->Bounds.GetBox();
    }
    return Bounds;
}

void ASimulatorHeldStaticPreviewActor::TryFinalizeBounds()
{
    if (!IsValid(VisualActor)) { StopBoundsRetry(); return; }
    const FBox Bounds = CalculateVisualBounds();
    if (Bounds.IsValid)
    {
        const float Longest = Bounds.GetSize().GetMax();
        if (FMath::IsFinite(Longest) && Longest > KINDA_SMALL_NUMBER)
        {
            const float UniformScale = FMath::Clamp(StoredEquipmentConfig.HeldPreviewLongestDimensionCm / Longest, 0.0001f, 1000.0f);
            VisualActor->SetActorScale3D(VisualActor->GetActorScale3D() * UniformScale);
            bBoundsFinalized = true; VisualActor->SetActorTickEnabled(false); StopBoundsRetry(); return;
        }
    }
    if (--RemainingBoundsAttempts <= 0) StopBoundsRetry();
}

void ASimulatorHeldStaticPreviewActor::NotifyVisualContentReady()
{
    RemainingBoundsAttempts = FMath::Max(RemainingBoundsAttempts, 1); TryFinalizeBounds();
}

AActor* ASimulatorHeldStaticPreviewActor::PlaceStatic(const FTransform& WorldTransform, const ESpawnActorCollisionHandlingMethod CollisionHandling)
{
    UWorld* World = GetWorld(); if (!IsValid(World) || !StaticActorClass) return nullptr;
    FActorSpawnParameters Params; Params.Owner = HolderWeak.Get(); Params.SpawnCollisionHandlingOverride = CollisionHandling;
    UClass* SpawnClass = StaticActorClass.Get();
    if (!IsValid(SpawnClass) || !SpawnClass->IsChildOf(AStaticActor::StaticClass())
        || SpawnClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
    {
        SpawnClass = AStaticActor::StaticClass();
    }
    AStaticActor* Placed = World->SpawnActor<AStaticActor>(SpawnClass, WorldTransform, Params);
    if (!IsValid(Placed) && SpawnClass != AStaticActor::StaticClass())
    {
        Placed = World->SpawnActor<AStaticActor>(AStaticActor::StaticClass(), WorldTransform, Params);
    }
    if (IsValid(Placed))
    {
        Placed->SetRenderOnlyStreaming(false);
        if (!Placed->LoadStatic(ModelReference, TEXT("PlacedStatic")))
        {
            Placed->Destroy();
            return nullptr;
        }
        Destroy();
    }
    return Placed;
}

void ASimulatorHeldStaticPreviewActor::StopBoundsRetry()
{
    if (UWorld* World = GetWorld()) World->GetTimerManager().ClearTimer(BoundsRetryTimer);
}

void ASimulatorHeldStaticPreviewActor::DestroyVisualActor()
{
    if (IsValid(VisualActor)) VisualActor->Destroy(); VisualActor = nullptr;
}

void ASimulatorHeldStaticPreviewActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopBoundsRetry(); DestroyVisualActor(); HolderWeak.Reset(); Super::EndPlay(EndPlayReason);
}
