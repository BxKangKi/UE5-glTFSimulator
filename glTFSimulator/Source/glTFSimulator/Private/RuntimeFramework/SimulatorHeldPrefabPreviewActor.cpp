#include "RuntimeFramework/SimulatorHeldPrefabPreviewActor.h"
#include "RuntimeFramework/SimulatorRuntimeAssetSource.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "TimerManager.h"

ASimulatorHeldPrefabPreviewActor::ASimulatorHeldPrefabPreviewActor()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = false;
    SetReplicateMovement(false);
    PreviewRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PreviewRoot"));
    SetRootComponent(PreviewRoot);
}

ASimulatorHeldPrefabPreviewActor* ASimulatorHeldPrefabPreviewActor::SpawnHeldPreview(UObject* WorldContextObject, ACharacter* Holder,
    TSubclassOf<AActor> PrefabVisualClass, const FString& CanonicalSourcePath,
    const FSimulatorCharacterInteractionConfig& CharacterConfig, const FSimulatorEquipmentInteractionConfig& EquipmentConfig)
{
    if (!IsValid(WorldContextObject) || !IsValid(Holder) || !PrefabVisualClass) return nullptr;
    UWorld* World = WorldContextObject->GetWorld(); if (!IsValid(World)) return nullptr;
    FActorSpawnParameters WrapperParams; WrapperParams.Owner = Holder; WrapperParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn; WrapperParams.ObjectFlags |= RF_Transient;
    ASimulatorHeldPrefabPreviewActor* Wrapper = World->SpawnActor<ASimulatorHeldPrefabPreviewActor>(StaticClass(), Holder->GetActorTransform(), WrapperParams);
    if (!IsValid(Wrapper)) return nullptr;
    FActorSpawnParameters VisualParams; VisualParams.Owner = Wrapper; VisualParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn; VisualParams.ObjectFlags |= RF_Transient;
    AActor* Visual = World->SpawnActor<AActor>(PrefabVisualClass, FTransform::Identity, VisualParams);
    if (!Wrapper->InitializePreview(Holder, Visual, CanonicalSourcePath, CharacterConfig, EquipmentConfig))
    {
        Wrapper->Destroy(); return nullptr;
    }
    return Wrapper;
}

bool ASimulatorHeldPrefabPreviewActor::InitializePreview(ACharacter* Holder, AActor* SpawnedVisualActor, const FString& CanonicalSourcePath,
    const FSimulatorCharacterInteractionConfig& CharacterConfig, const FSimulatorEquipmentInteractionConfig& EquipmentConfig)
{
    if (!IsValid(Holder) || !IsValid(SpawnedVisualActor) || SpawnedVisualActor == this) return false;
    HolderWeak = Holder; VisualActor = SpawnedVisualActor; PrefabClass = SpawnedVisualActor->GetClass(); SourcePath = CanonicalSourcePath;
    StoredCharacterConfig = CharacterConfig; StoredEquipmentConfig = EquipmentConfig; StoredEquipmentConfig.Sanitize();
    VisualActor->SetReplicates(false); VisualActor->SetReplicateMovement(false); VisualActor->SetActorEnableCollision(false);
    VisualActor->AttachToComponent(PreviewRoot, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
    if (VisualActor->GetClass()->ImplementsInterface(USimulatorRuntimeAssetSource::StaticClass()))
    {
        ISimulatorRuntimeAssetSource::Execute_SetRuntimeAssetSource(VisualActor, SourcePath);
    }
    ConfigureVisualForPreview(); AttachToResolvedHand(); RemainingBoundsAttempts = 120;
    GetWorldTimerManager().SetTimer(BoundsRetryTimer, this, &ASimulatorHeldPrefabPreviewActor::TryFinalizeBounds, 0.05f, true, 0.0f);
    TryFinalizeBounds(); return true;
}

void ASimulatorHeldPrefabPreviewActor::ConfigureVisualForPreview()
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

void ASimulatorHeldPrefabPreviewActor::AttachToResolvedHand()
{
    ACharacter* Holder = HolderWeak.Get(); if (!IsValid(Holder) || !IsValid(Holder->GetMesh())) return;
    const ESimulatorHand Hand = StoredEquipmentConfig.ResolvePrimaryHand(StoredCharacterConfig.DominantHand);
    const FSimulatorGripPoint* Grip = StoredEquipmentConfig.GetGrip(Hand);
    FName Socket = Grip && !Grip->CharacterSocket.IsNone() ? Grip->CharacterSocket : (Hand == ESimulatorHand::Right ? StoredCharacterConfig.RightHandSocket : StoredCharacterConfig.LeftHandSocket);
    AttachToComponent(Holder->GetMesh(), FAttachmentTransformRules::SnapToTargetNotIncludingScale, Socket);
    SetActorRelativeTransform(Grip ? Grip->AttachmentOffset : FTransform::Identity);
}

FBox ASimulatorHeldPrefabPreviewActor::CalculateVisualBounds() const
{
    FBox Bounds(ForceInit); if (!IsValid(VisualActor)) return Bounds;
    TInlineComponentArray<UPrimitiveComponent*> Primitives; VisualActor->GetComponents(Primitives);
    for (const UPrimitiveComponent* Primitive : Primitives)
    {
        if (IsValid(Primitive) && Primitive->IsRegistered()) Bounds += Primitive->Bounds.GetBox();
    }
    return Bounds;
}

void ASimulatorHeldPrefabPreviewActor::TryFinalizeBounds()
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

void ASimulatorHeldPrefabPreviewActor::NotifyVisualContentReady()
{
    RemainingBoundsAttempts = FMath::Max(RemainingBoundsAttempts, 1); TryFinalizeBounds();
}

AActor* ASimulatorHeldPrefabPreviewActor::PlacePrefab(const FTransform& WorldTransform, const ESpawnActorCollisionHandlingMethod CollisionHandling)
{
    UWorld* World = GetWorld(); if (!IsValid(World) || !PrefabClass) return nullptr;
    FActorSpawnParameters Params; Params.Owner = HolderWeak.Get(); Params.SpawnCollisionHandlingOverride = CollisionHandling;
    AActor* Placed = World->SpawnActor<AActor>(PrefabClass, WorldTransform, Params);
    if (IsValid(Placed) && Placed->GetClass()->ImplementsInterface(USimulatorRuntimeAssetSource::StaticClass()))
    {
        ISimulatorRuntimeAssetSource::Execute_SetRuntimeAssetSource(Placed, SourcePath);
    }
    if (IsValid(Placed)) Destroy();
    return Placed;
}

void ASimulatorHeldPrefabPreviewActor::StopBoundsRetry()
{
    if (UWorld* World = GetWorld()) World->GetTimerManager().ClearTimer(BoundsRetryTimer);
}

void ASimulatorHeldPrefabPreviewActor::DestroyVisualActor()
{
    if (IsValid(VisualActor)) VisualActor->Destroy(); VisualActor = nullptr;
}

void ASimulatorHeldPrefabPreviewActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopBoundsRetry(); DestroyVisualActor(); HolderWeak.Reset(); Super::EndPlay(EndPlayReason);
}
