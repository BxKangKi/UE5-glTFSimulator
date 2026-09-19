// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "World/WaterExclusionBoxComponent.h"

#include "Engine/World.h"
#include "World/WaterActor.h"

namespace
{
    TMap<const UWorld*, TArray<TWeakObjectPtr<UWaterExclusionBoxComponent>>> GWaterExclusionsByWorld;

    TArray<TWeakObjectPtr<UWaterExclusionBoxComponent>>* FindRegistry(const UObject* WorldContextObject)
    {
        const UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
        return World ? GWaterExclusionsByWorld.Find(World) : nullptr;
    }
}

UWaterExclusionBoxComponent::UWaterExclusionBoxComponent()
{
    // Constructor-time component setters such as SetBoxExtent/SetCollisionEnabled can force
    // UShapeComponent to refresh its BodySetup. In UE 5.8 that path may allocate an unnamed
    // UObject while the component CDO is still being constructed, which is forbidden and
    // produces the UObjectGlobals.cpp "NewObject with empty name" fatal error. Initialize
    // the stored values directly and let normal component registration build any runtime state.
    // Generate-overlap is intentionally not touched here: UPrimitiveComponent exposes the backing
    // bit as private in UE 5.8, and the public setter is safely applied from OnRegister() below.
    InitBoxExtent(FVector(200.0f, 200.0f, 200.0f));
    BodyInstance.SetCollisionEnabled(ECollisionEnabled::NoCollision, false);
    BodyInstance.SetResponseToAllChannels(ECR_Ignore);
    bHiddenInGame = true;

    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
    PrimaryComponentTick.TickInterval = 0.10f;
}

void UWaterExclusionBoxComponent::OnRegister()
{
    Super::OnRegister();

    // Registration is past UObject construction, so the normal component setters are safe here.
    // Re-assert the non-physical contract in case an older Blueprint/component instance serialized
    // collision or overlap values that differ from the native defaults.
    SetCollisionEnabled(ECollisionEnabled::NoCollision);
    SetCollisionResponseToAllChannels(ECR_Ignore);
    SetGenerateOverlapEvents(false);
    SetCanEverAffectNavigation(false);
    SetHiddenInGame(true);
}

void UWaterExclusionBoxComponent::BeginPlay()
{
    Super::BeginPlay();
    RegisterExclusion();
    LastPublishedTransform = GetComponentTransform();
    LastPublishedExtent = GetUnscaledBoxExtent();
    LastPublishedFeatureMask = GetFeatureMask();
    AWaterActor::RefreshWaterExclusionRendering(this);
}

void UWaterExclusionBoxComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UWorld* World = GetWorld();
    UnregisterExclusion();
    if (World)
    {
        AWaterActor::RefreshWaterExclusionRendering(World);
    }
    Super::EndPlay(EndPlayReason);
}

void UWaterExclusionBoxComponent::TickComponent(
    const float DeltaTime,
    const ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    const FTransform CurrentTransform = GetComponentTransform();
    const FVector CurrentExtent = GetUnscaledBoxExtent();
    const uint8 CurrentFeatureMask = GetFeatureMask();
    if (!CurrentTransform.Equals(LastPublishedTransform, 0.01f)
        || !CurrentExtent.Equals(LastPublishedExtent, 0.01f)
        || CurrentFeatureMask != LastPublishedFeatureMask)
    {
        LastPublishedTransform = CurrentTransform;
        LastPublishedExtent = CurrentExtent;
        LastPublishedFeatureMask = CurrentFeatureMask;
        AWaterActor::RefreshWaterExclusionRendering(this);
    }
}

bool UWaterExclusionBoxComponent::ContainsWorldLocation(const FVector& WorldLocation) const
{
    FWaterExclusionNativeBox Box;
    Box.WorldTransform = GetComponentTransform();
    Box.LocalExtent = GetUnscaledBoxExtent();
    return Box.Contains(WorldLocation);
}

void UWaterExclusionBoxComponent::RefreshWaterExclusion()
{
    LastPublishedTransform = GetComponentTransform();
    LastPublishedExtent = GetUnscaledBoxExtent();
    LastPublishedFeatureMask = GetFeatureMask();
    AWaterActor::RefreshWaterExclusionRendering(this);
}

uint8 UWaterExclusionBoxComponent::GetFeatureMask() const
{
    return (bExcludeWaterRendering ? 1u : 0u)
        | (bExcludeUnderwaterPostProcess ? 2u : 0u)
        | (bExcludeBuoyancy ? 4u : 0u)
        | (bExcludeWaterInteraction ? 8u : 0u);
}

bool UWaterExclusionBoxComponent::IsFeatureEnabled(const EWaterExclusionFeature Feature) const
{
    switch (Feature)
    {
    case EWaterExclusionFeature::Rendering:
        return bExcludeWaterRendering;
    case EWaterExclusionFeature::UnderwaterPostProcess:
        return bExcludeUnderwaterPostProcess;
    case EWaterExclusionFeature::Buoyancy:
        return bExcludeBuoyancy;
    case EWaterExclusionFeature::WaterInteraction:
        return bExcludeWaterInteraction;
    default:
        return false;
    }
}

void UWaterExclusionBoxComponent::RegisterExclusion()
{
    if (UWorld* World = GetWorld())
    {
        GWaterExclusionsByWorld.FindOrAdd(World).AddUnique(this);
    }
}

void UWaterExclusionBoxComponent::UnregisterExclusion()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }
    if (TArray<TWeakObjectPtr<UWaterExclusionBoxComponent>>* Entries = GWaterExclusionsByWorld.Find(World))
    {
        Entries->RemoveAll([this](const TWeakObjectPtr<UWaterExclusionBoxComponent>& Entry)
        {
            return !Entry.IsValid() || Entry.Get() == this;
        });
        if (Entries->IsEmpty())
        {
            GWaterExclusionsByWorld.Remove(World);
        }
    }
}

bool UWaterExclusionBoxComponent::IsLocationExcluded(
    const UObject* WorldContextObject,
    const FVector& WorldLocation,
    const EWaterExclusionFeature Feature,
    const float Tolerance)
{
    TArray<TWeakObjectPtr<UWaterExclusionBoxComponent>>* Entries = FindRegistry(WorldContextObject);
    if (!Entries)
    {
        return false;
    }

    for (int32 Index = Entries->Num() - 1; Index >= 0; --Index)
    {
        UWaterExclusionBoxComponent* Component = (*Entries)[Index].Get();
        if (!IsValid(Component))
        {
            Entries->RemoveAtSwap(Index, 1, EAllowShrinking::No);
            continue;
        }
        if (!Component->IsFeatureEnabled(Feature))
        {
            continue;
        }

        FWaterExclusionNativeBox Box;
        Box.WorldTransform = Component->GetComponentTransform();
        Box.LocalExtent = Component->GetUnscaledBoxExtent();
        if (Box.Contains(WorldLocation, Tolerance))
        {
            return true;
        }
    }
    return false;
}

void UWaterExclusionBoxComponent::GatherNativeBoxes(
    const UObject* WorldContextObject,
    const EWaterExclusionFeature Feature,
    TArray<FWaterExclusionNativeBox>& OutBoxes,
    const int32 MaxBoxes)
{
    OutBoxes.Reset();
    if (MaxBoxes <= 0)
    {
        return;
    }

    TArray<TWeakObjectPtr<UWaterExclusionBoxComponent>>* Entries = FindRegistry(WorldContextObject);
    if (!Entries)
    {
        return;
    }

    OutBoxes.Reserve(FMath::Min(MaxBoxes, Entries->Num()));
    for (int32 Index = Entries->Num() - 1; Index >= 0 && OutBoxes.Num() < MaxBoxes; --Index)
    {
        UWaterExclusionBoxComponent* Component = (*Entries)[Index].Get();
        if (!IsValid(Component))
        {
            Entries->RemoveAtSwap(Index, 1, EAllowShrinking::No);
            continue;
        }
        if (!Component->IsFeatureEnabled(Feature))
        {
            continue;
        }

        FWaterExclusionNativeBox& Box = OutBoxes.AddDefaulted_GetRef();
        Box.WorldTransform = Component->GetComponentTransform();
        Box.LocalExtent = Component->GetUnscaledBoxExtent();
    }
}
