// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

/**
 * @file WaterActor.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "World/WaterActor.h"
#include "World/WaterExclusionBoxComponent.h"
#include "World/BuoyancyComponent.h"
#include "System/GlTFSimulatorGameInstance.h"
#include "System/GlTFSimulatorAssetRegistry.h"
#include "Character/CharacterController.h"
#include "Interface/WaterInteract.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Components/PostProcessComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/DecalComponent.h"
#include "Components/ActorComponent.h"
#include "Engine/World.h"

namespace
{
    constexpr int32 MaxWaterRenderExclusionBoxes = 8;

    FName MakeIndexedWaterParameterName(const TCHAR* Prefix, const int32 Index)
    {
        return FName(*FString::Printf(TEXT("%s_%d"), Prefix, Index));
    }

    // Water probes can execute several times per frame. A weak per-world registry avoids a full
    // actor iterator for each probe while still allowing actors to disappear safely during travel.
    TMap<const UWorld*, TArray<TWeakObjectPtr<AWaterActor>>> GWaterActorsByWorld;
    TMap<const UWorld*, TWeakObjectPtr<AWaterActor>> GGlobalOceanByWorld;

    void RegisterWaterActor(AWaterActor* WaterActor)
    {
        if (IsValid(WaterActor) && WaterActor->GetWorld())
        {
            GWaterActorsByWorld.FindOrAdd(WaterActor->GetWorld()).AddUnique(WaterActor);
        }
    }

    void UnregisterWaterActor(AWaterActor* WaterActor)
    {
        if (!WaterActor)
        {
            return;
        }

        const UWorld* World = WaterActor->GetWorld();
        TArray<TWeakObjectPtr<AWaterActor>>* WaterActors = GWaterActorsByWorld.Find(World);
        if (!WaterActors)
        {
            return;
        }

        WaterActors->RemoveAll([WaterActor](const TWeakObjectPtr<AWaterActor>& Entry)
        {
            return !Entry.IsValid() || Entry.Get() == WaterActor;
        });
        if (WaterActors->IsEmpty())
        {
            GWaterActorsByWorld.Remove(World);
        }

        if (TWeakObjectPtr<AWaterActor>* GlobalOcean = GGlobalOceanByWorld.Find(World);
            GlobalOcean && (!GlobalOcean->IsValid() || GlobalOcean->Get() == WaterActor))
        {
            GGlobalOceanByWorld.Remove(World);
        }
    }
}

AWaterActor::AWaterActor()
{
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    Decal = CreateDefaultSubobject<UDecalComponent>(TEXT("StaticMesh"));
    Decal->SetupAttachment(RootComponent);
    Decal->SetRelativeLocation(FVector(0.0f, 0.0f, -1.0f));
    Decal->DecalSize = FVector(1.0f, 1.0f, 1.0f);
    Collision = CreateDefaultSubobject<UBoxComponent>(TEXT("Collision"));
    Collision->SetupAttachment(RootComponent);
    Collision->SetRelativeLocation(FVector(0.0f, 0.0f, -1.0f));
    Collision->SetBoxExtent(FVector(1.0f, 1.0f, 1.0f));
    Collision->SetEnableGravity(false);
    Collision->SetVisibility(false);
    // Water is a trigger/query volume, never a solid physics wall. The old default primitive
    // response could block capsules and ragdoll bodies depending on the project collision preset.
    Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    Collision->SetCollisionResponseToAllChannels(ECR_Overlap);
    Collision->SetGenerateOverlapEvents(true);
    Collision->SetCanEverAffectNavigation(false);
    PostProcess = CreateDefaultSubobject<UPostProcessComponent>(TEXT("PostProces"));
    PostProcess->SetupAttachment(Collision);
    PostProcess->bUnbound = false;
    PostProcess->bEnabled = true;
    PostProcess->BlendRadius = 5.0f;
    StaticMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WaterStaticMesh"));
    StaticMesh->SetupAttachment(RootComponent);
    StaticMesh->SetEnableGravity(false);
    StaticMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    StaticMesh->SetGenerateOverlapEvents(false);
    StaticMesh->SetCastShadow(false);
    StaticMesh->SetVisibility(true, true);
    StaticMesh->SetHiddenInGame(false, true);
}

void AWaterActor::BeginPlay()
{
    Super::BeginPlay();

    // Re-assert native water collision at runtime as Blueprint subclasses may carry older serialized
    // component presets that override constructor defaults. Water must overlap, never block.
    if (IsValid(Collision.Get()))
    {
        Collision->SetCollisionEnabled(bGlobalOcean ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryOnly);
        Collision->SetCollisionResponseToAllChannels(ECR_Overlap);
        Collision->SetGenerateOverlapEvents(!bGlobalOcean);
        Collision->SetCanEverAffectNavigation(false);
    }
    if (IsValid(StaticMesh.Get()))
    {
        StaticMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        StaticMesh->SetGenerateOverlapEvents(false);
        StaticMesh->SetVisibility(true, true);
        StaticMesh->SetHiddenInGame(false, true);
    }

    if (UGlTFSimulatorAssetRegistry* Registry = UGlTFSimulatorGameInstance::GetAssetRegistryFromContext(this))
    {
        // Empty registry entries must not erase values authored on BP_Water or another derived class.
        if (!Registry->WaterDecalMaterial.IsNull())
        {
            if (UMaterialInterface* RegistryDecal = Registry->WaterDecalMaterial.LoadSynchronous())
            {
                DecalMaterial = RegistryDecal;
            }
        }
        if (!bGlobalOcean && !Registry->UnderWaterMaterial.IsNull())
        {
            if (UMaterialInterface* RegistryUnderWater = Registry->UnderWaterMaterial.LoadSynchronous())
            {
                UnderWaterMaterial = RegistryUnderWater;
            }
        }
        if (IsValid(StaticMesh) && !Registry->WaterMesh.IsNull())
        {
            if (UStaticMesh* RegistryWaterMesh = Registry->WaterMesh.LoadSynchronous())
            {
                StaticMesh->SetStaticMesh(RegistryWaterMesh);
            }
        }
    }

    // Compatibility with the original project assets. This also makes the native AWaterActor
    // fallback visibly render when the new AssetRegistry has not been populated yet.
    bool bLoadedLegacyPlane = false;
    if (IsValid(StaticMesh) && !IsValid(StaticMesh->GetStaticMesh()))
    {
        if (UStaticMesh* LegacyPlane = LoadObject<UStaticMesh>(
                nullptr, TEXT("/Game/Resources/Meshes/SM_Plane.SM_Plane")))
        {
            StaticMesh->SetStaticMesh(LegacyPlane);
            bLoadedLegacyPlane = true;
        }
    }
    if (IsValid(StaticMesh) && bLoadedLegacyPlane)
    {
        if (UMaterialInterface* LegacyWaterMaterial = LoadObject<UMaterialInterface>(
                nullptr, TEXT("/Game/Resources/Materials/MI_Water.MI_Water")))
        {
            StaticMesh->SetMaterial(0, LegacyWaterMaterial);
        }
    }
    if (!IsValid(DecalMaterial))
    {
        DecalMaterial = LoadObject<UMaterialInterface>(
            nullptr, TEXT("/Game/Resources/Materials/MI_Caustics.MI_Caustics"));
    }
    if (!bGlobalOcean && !IsValid(UnderWaterMaterial))
    {
        UnderWaterMaterial = LoadObject<UMaterialInterface>(
            nullptr, TEXT("/Game/Resources/Materials/MPPI_UnderWater.MPPI_UnderWater"));
    }

    if (!IsValid(StaticMesh.Get()) || !IsValid(StaticMesh->GetStaticMesh()))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Water actor has no render mesh. Actor=%s Class=%s; configure AssetRegistry.WaterMesh or keep the legacy /Game/Resources/Meshes/SM_Plane asset."),
            *GetName(), *GetNameSafe(GetClass()));
    }

    RegisterWaterActor(this);
    SetCurrentLevel();
    if (!bGlobalOcean)
    {
        bAuthoredPostProcessEnabled = IsValid(PostProcess.Get()) ? PostProcess->bEnabled : true;
    }
    EnsureWaterMaterialInstances();
    ApplyWaterExclusionRenderParameters();
    UMaterialInstanceDynamic *DecalMID = IsValid(DecalMaterial) ? UMaterialInstanceDynamic::Create(DecalMaterial, this) : nullptr;
    if (DecalMID)
    {
        DecalMID->SetScalarParameterValue(TEXT("WaterLevel"), Level);
        Decal->SetDecalMaterial(DecalMID);
    }
    // The global ocean no longer owns an underwater PP material. WorldEnvManager's single
    // unbound GlobalPostProcessMaterial reads OceanHeight/OceanEnabled from ShaderLibraryMPC.
    UMaterialInstanceDynamic *PostProcessMID = !bGlobalOcean && IsValid(UnderWaterMaterial)
        ? UMaterialInstanceDynamic::Create(UnderWaterMaterial, this)
        : nullptr;
    if (PostProcessMID)
    {
        PostProcess->AddOrUpdateBlendable(PostProcessMID, 1.0f);
        PostProcessMID->SetScalarParameterValue(FName("WaterLevel"), Level);
    }
}

void AWaterActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UnregisterWaterActor(this);
    Super::EndPlay(EndPlayReason);
}

void AWaterActor::SetGlobalOcean(const bool bInGlobalOcean)
{
    bGlobalOcean = bInGlobalOcean;
    SetCurrentLevel();

    if (UWorld* World = GetWorld())
    {
        if (bGlobalOcean)
        {
            GGlobalOceanByWorld.Add(World, this);
        }
        else if (TWeakObjectPtr<AWaterActor>* Existing = GGlobalOceanByWorld.Find(World);
            Existing && Existing->Get() == this)
        {
            GGlobalOceanByWorld.Remove(World);
        }
    }

    // The global ocean is an infinite XY half-space below Level. Its collision box is rendering
    // scaffolding only and must never determine water presence. Local/streamed water actors retain
    // their normal overlap volume behaviour.
    if (IsValid(Collision.Get()))
    {
        Collision->SetCollisionEnabled(bGlobalOcean ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryOnly);
        Collision->SetGenerateOverlapEvents(!bGlobalOcean);
        if (!bGlobalOcean)
        {
            Collision->SetCollisionResponseToAllChannels(ECR_Overlap);
        }
    }

    if (IsValid(PostProcess.Get()))
    {
        // Global-ocean underwater rendering is owned exclusively by WorldEnvManager's unbound
        // post process. Keep this actor's finite PP only for local/streamed water volumes.
        PostProcess->bUnbound = false;
        PostProcess->bEnabled = bGlobalOcean ? false : bAuthoredPostProcessEnabled;
    }
}

void AWaterActor::SetGlobalOceanRenderRadius(const float RadiusCm)
{
    if (!bGlobalOcean || !FMath::IsFinite(RadiusCm) || RadiusCm <= KINDA_SMALL_NUMBER)
    {
        return;
    }

    // This radius is visual only. Global-ocean water presence remains an infinite XY half-space
    // resolved from OceanHeightCm, so reducing the mesh footprint never changes buoyancy/swimming.
    const float SafeRadiusCm = FMath::Max(100.0f, RadiusCm);
    if (FMath::IsNearlyEqual(GlobalOceanRenderRadiusCm, SafeRadiusCm, 1.0f))
    {
        return;
    }

    if (!IsValid(StaticMesh.Get()) || !IsValid(StaticMesh->GetStaticMesh()))
    {
        return;
    }

    const FBoxSphereBounds LocalBounds = StaticMesh->GetStaticMesh()->GetBounds();
    const FVector RelativeScale = StaticMesh->GetRelativeScale3D().GetAbs();
    const double BaseHalfExtentX = static_cast<double>(LocalBounds.BoxExtent.X)
        * static_cast<double>(FMath::Max(RelativeScale.X, UE_SMALL_NUMBER));
    const double BaseHalfExtentY = static_cast<double>(LocalBounds.BoxExtent.Y)
        * static_cast<double>(FMath::Max(RelativeScale.Y, UE_SMALL_NUMBER));
    const double BaseHalfExtent = FMath::Min(BaseHalfExtentX, BaseHalfExtentY);
    if (!FMath::IsFinite(BaseHalfExtent) || BaseHalfExtent <= UE_SMALL_NUMBER)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Global ocean mesh has invalid XY bounds; render-radius sizing skipped. Mesh=%s Bounds=%s"),
            *GetNameSafe(StaticMesh->GetStaticMesh()),
            *LocalBounds.BoxExtent.ToCompactString());
        return;
    }

    // Use one uniform XY actor scale so a square/rectangular plane fully covers the requested
    // circular view radius without stretching X/Y at different rates. Z stays unscaled because
    // collision and underwater detection are independent from this visual surface.
    const double RequiredScale = static_cast<double>(SafeRadiusCm) / BaseHalfExtent;
    if (!FMath::IsFinite(RequiredScale) || RequiredScale <= UE_SMALL_NUMBER)
    {
        return;
    }

    FVector NewActorScale = GetActorScale3D();
    NewActorScale.X = RequiredScale;
    NewActorScale.Y = RequiredScale;
    NewActorScale.Z = 1.0;

    if (!NewActorScale.Equals(GetActorScale3D(), 0.001))
    {
        SetActorScale3D(NewActorScale);
    }
    GlobalOceanRenderRadiusCm = SafeRadiusCm;

    UE_LOG(LogTemp, Display,
        TEXT("Global ocean visual radius configured. Radius=%.1f m Mesh=%s ActorScaleXY=%.3f"),
        SafeRadiusCm / 100.0f,
        *GetNameSafe(StaticMesh->GetStaticMesh()),
        NewActorScale.X);
}

void AWaterActor::WaterTrigger(AActor *OtherActor, bool InWater)
{
    if (!OtherActor)
    {
        return;
    }

    // WaterInteraction and Buoyancy are intentionally independent switches. Generic swimming /
    // splash state sees an exclusion as dry, while UBuoyancyComponent keeps the underlying water
    // overlap latched and suppresses force per sample. Keeping that overlap latched is important:
    // an object can leave an exclusion box without ever leaving the large ocean overlap and must
    // resume buoyancy immediately without waiting for a new BeginOverlap event.
    const bool bInteractionExcluded = InWater && UWaterExclusionBoxComponent::IsLocationExcluded(
        this, OtherActor->GetActorLocation(), EWaterExclusionFeature::WaterInteraction);
    const bool bInteractionInWater = InWater && !bInteractionExcluded;

    if (IWaterInteract* ActorWaterInteract = Cast<IWaterInteract>(OtherActor))
    {
        if (bInteractionInWater)
        {
            ActorWaterInteract->EnterWater(Level);
        }
        else
        {
            ActorWaterInteract->ExitWater(Level);
        }
    }

    TInlineComponentArray<UActorComponent*, 16> Components;
    OtherActor->GetComponents(Components);
    for (UActorComponent* Component : Components)
    {
        if (IWaterInteract* ComponentWaterInteract = Cast<IWaterInteract>(Component))
        {
            // Global-ocean buoyancy is height-driven inside UBuoyancyComponent and must never be
            // latched by a synthetic overlap. Local water keeps overlap state, while exclusions are
            // still applied per buoyancy sample.
            if (bGlobalOcean && Cast<UBuoyancyComponent>(Component))
            {
                continue;
            }

            const bool bComponentInWater = Cast<UBuoyancyComponent>(Component)
                ? InWater
                : bInteractionInWater;
            if (bComponentInWater)
            {
                ComponentWaterInteract->EnterWater(Level);
            }
            else
            {
                ComponentWaterInteract->ExitWater(Level);
            }
        }
    }
}

void AWaterActor::NotifyActorBeginOverlap(AActor* OtherActor)
{
    if (!bGlobalOcean)
    {
        WaterTrigger(OtherActor, true);
    }
}

void AWaterActor::NotifyActorEndOverlap(AActor* OtherActor)
{
    if (!bGlobalOcean)
    {
        WaterTrigger(OtherActor, false);
    }
}

void AWaterActor::SetCurrentLevel()
{
    FVector Location = GetActorLocation();
    Level = Location.Z;
}

void AWaterActor::CheckOverlappingWater(AActor *Target)
{
    if (!IsValid(Target))
    {
        return;
    }

    // Local water volumes still use overlap events. The global ocean is intentionally excluded
    // from overlap detection and is resolved from height below.
    TArray<AActor*> OverlappingActors;
    Target->GetOverlappingActors(OverlappingActors, AWaterActor::StaticClass());
    for (AActor* OverlappedActor : OverlappingActors)
    {
        if (AWaterActor* WaterActor = Cast<AWaterActor>(OverlappedActor);
            IsValid(WaterActor) && !WaterActor->IsGlobalOcean())
        {
            WaterActor->WaterTrigger(Target, true);
        }
    }

    float GlobalOceanLevel = 0.0f;
    if (FindGlobalOceanLevel(Target, GlobalOceanLevel)
        && Target->GetActorLocation().Z <= GlobalOceanLevel + 5.0f
        && !UWaterExclusionBoxComponent::IsLocationExcluded(
            Target, Target->GetActorLocation(), EWaterExclusionFeature::WaterInteraction))
    {
        const UWorld* World = Target->GetWorld();
        if (TWeakObjectPtr<AWaterActor>* Entry = GGlobalOceanByWorld.Find(World))
        {
            if (AWaterActor* WaterActor = Entry->Get(); IsValid(WaterActor))
            {
                WaterActor->WaterTrigger(Target, true);
            }
        }
    }
}

bool AWaterActor::FindGlobalOceanLevel(const UObject* WorldContextObject, float& OutLevel)
{
    const UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    if (!World)
    {
        return false;
    }

    TWeakObjectPtr<AWaterActor>* Entry = GGlobalOceanByWorld.Find(World);
    AWaterActor* WaterActor = Entry ? Entry->Get() : nullptr;
    if (!IsValid(WaterActor) || !WaterActor->IsGlobalOcean())
    {
        if (Entry)
        {
            GGlobalOceanByWorld.Remove(World);
        }
        return false;
    }

    OutLevel = WaterActor->Level;
    return true;
}

bool AWaterActor::FindWaterLevelAtLocationStrict(const UObject *WorldContextObject, const FVector &WorldLocation, float &OutLevel)
{
    const UWorld *ConstWorld = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    UWorld *World = const_cast<UWorld *>(ConstWorld);
    if (!World)
    {
        return false;
    }

    if (UWaterExclusionBoxComponent::IsLocationExcluded(
            WorldContextObject, WorldLocation, EWaterExclusionFeature::WaterInteraction))
    {
        return false;
    }

    bool bFound = false;
    float BestLevel = OutLevel;
    constexpr float SurfaceTolerance = 5.0f;
    constexpr float LowerBoundsTolerance = 2.0f;

    TArray<TWeakObjectPtr<AWaterActor>>* WaterActors = GWaterActorsByWorld.Find(World);
    if (!WaterActors)
    {
        return false;
    }

    for (int32 Index = WaterActors->Num() - 1; Index >= 0; --Index)
    {
        AWaterActor* WaterActor = (*WaterActors)[Index].Get();
        if (!IsValid(WaterActor))
        {
            WaterActors->RemoveAtSwap(Index, 1, EAllowShrinking::No);
            continue;
        }

        const float WaterLevel = WaterActor->Level;
        if (WaterActor->IsGlobalOcean())
        {
            // Infinite horizontal ocean: only the authored sea level matters. No collision bounds,
            // no lower-Z cutoff and no camera-following hit box participate in the query.
            if (WorldLocation.Z <= WaterLevel + SurfaceTolerance)
            {
                BestLevel = bFound ? FMath::Max(BestLevel, WaterLevel) : WaterLevel;
                bFound = true;
            }
            continue;
        }

        const UBoxComponent *WaterCollision = WaterActor->Collision.Get();
        bool bInsideBoxColumn = false;
        bool bInsideVerticalRange = false;

        if (WaterCollision)
        {
            const FVector LocalPoint = WaterCollision->GetComponentTransform().InverseTransformPosition(WorldLocation);
            const FVector LocalExtent = WaterCollision->GetUnscaledBoxExtent();

            bInsideBoxColumn = FMath::Abs(LocalPoint.X) <= LocalExtent.X
                && FMath::Abs(LocalPoint.Y) <= LocalExtent.Y;
            bInsideVerticalRange = LocalPoint.Z >= -LocalExtent.Z - LowerBoundsTolerance
                && WorldLocation.Z <= WaterLevel + SurfaceTolerance;
        }
        else
        {
            const FBox Bounds = WaterActor->GetComponentsBoundingBox(true);
            if (!Bounds.IsValid)
            {
                continue;
            }

            bInsideBoxColumn = WorldLocation.X >= Bounds.Min.X
                && WorldLocation.X <= Bounds.Max.X
                && WorldLocation.Y >= Bounds.Min.Y
                && WorldLocation.Y <= Bounds.Max.Y;
            bInsideVerticalRange = WorldLocation.Z >= Bounds.Min.Z - LowerBoundsTolerance
                && WorldLocation.Z <= WaterLevel + SurfaceTolerance;
        }

        if (!bInsideBoxColumn || !bInsideVerticalRange)
        {
            continue;
        }

        BestLevel = bFound ? FMath::Max(BestLevel, WaterLevel) : WaterLevel;
        bFound = true;
    }

    if (bFound)
    {
        OutLevel = BestLevel;
    }
    return bFound;
}

bool AWaterActor::FindWaterLevelAtLocation(const UObject *WorldContextObject, const FVector &WorldLocation, float &OutLevel)
{
    const UWorld *ConstWorld = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    UWorld *World = const_cast<UWorld *>(ConstWorld);
    if (!World)
    {
        return false;
    }

    if (UWaterExclusionBoxComponent::IsLocationExcluded(
            WorldContextObject, WorldLocation, EWaterExclusionFeature::WaterInteraction))
    {
        return false;
    }

    bool bFound = false;
    float BestLevel = OutLevel;
    constexpr float HorizontalTolerance = 35.0f;
    constexpr float SurfaceTolerance = 20.0f;
    constexpr float LowerBoundsTolerance = 400.0f;

    TArray<TWeakObjectPtr<AWaterActor>>* WaterActors = GWaterActorsByWorld.Find(World);
    if (!WaterActors)
    {
        return false;
    }

    for (int32 Index = WaterActors->Num() - 1; Index >= 0; --Index)
    {
        AWaterActor* WaterActor = (*WaterActors)[Index].Get();
        if (!IsValid(WaterActor))
        {
            WaterActors->RemoveAtSwap(Index, 1, EAllowShrinking::No);
            continue;
        }

        const float WaterLevel = WaterActor->Level;
        if (WaterActor->IsGlobalOcean())
        {
            if (WorldLocation.Z <= WaterLevel + SurfaceTolerance)
            {
                BestLevel = bFound ? FMath::Max(BestLevel, WaterLevel) : WaterLevel;
                bFound = true;
            }
            continue;
        }

        const UBoxComponent *WaterCollision = WaterActor->Collision.Get();
        const FBox Bounds = WaterCollision ? WaterCollision->Bounds.GetBox() : WaterActor->GetComponentsBoundingBox(true);
        if (!Bounds.IsValid)
        {
            continue;
        }

        const bool bInsideXY = WorldLocation.X >= Bounds.Min.X - HorizontalTolerance
            && WorldLocation.X <= Bounds.Max.X + HorizontalTolerance
            && WorldLocation.Y >= Bounds.Min.Y - HorizontalTolerance
            && WorldLocation.Y <= Bounds.Max.Y + HorizontalTolerance;
        if (!bInsideXY)
        {
            continue;
        }

        const bool bInsideWaterColumn = WorldLocation.Z <= WaterLevel + SurfaceTolerance
            && WorldLocation.Z >= Bounds.Min.Z - LowerBoundsTolerance;
        if (!bInsideWaterColumn)
        {
            continue;
        }

        BestLevel = bFound ? FMath::Max(BestLevel, WaterLevel) : WaterLevel;
        bFound = true;
    }

    if (bFound)
    {
        OutLevel = BestLevel;
    }
    return bFound;
}

void AWaterActor::EnsureWaterMaterialInstances()
{
    if (!IsValid(StaticMesh.Get()))
    {
        return;
    }

    const int32 MaterialCount = StaticMesh->GetNumMaterials();
    if (MaterialCount <= 0)
    {
        WaterMaterialInstances.Reset();
        return;
    }

    WaterMaterialInstances.SetNum(MaterialCount);
    for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
    {
        UMaterialInstanceDynamic* ExistingMID = Cast<UMaterialInstanceDynamic>(StaticMesh->GetMaterial(MaterialIndex));
        UMaterialInstanceDynamic* MID = ExistingMID
            ? ExistingMID
            : StaticMesh->CreateDynamicMaterialInstance(MaterialIndex, StaticMesh->GetMaterial(MaterialIndex));
        WaterMaterialInstances[MaterialIndex] = MID;
    }
}

void AWaterActor::ApplyWaterExclusionRenderParameters()
{
    EnsureWaterMaterialInstances();

    TArray<FWaterExclusionNativeBox> Boxes;
    UWaterExclusionBoxComponent::GatherNativeBoxes(
        this,
        EWaterExclusionFeature::Rendering,
        Boxes,
        MaxWaterRenderExclusionBoxes);

    for (UMaterialInstanceDynamic* MID : WaterMaterialInstances)
    {
        if (!IsValid(MID))
        {
            continue;
        }

        MID->SetScalarParameterValue(TEXT("WaterExclusionCount"), static_cast<float>(Boxes.Num()));
        for (int32 Index = 0; Index < Boxes.Num(); ++Index)
        {
            const FWaterExclusionNativeBox& Box = Boxes[Index];
            const FTransform& Transform = Box.WorldTransform;
            const FVector Scale = Transform.GetScale3D().GetAbs();
            const FVector WorldExtent = Box.LocalExtent * Scale;
            const FQuat Rotation = Transform.GetRotation().GetNormalized();

            MID->SetVectorParameterValue(
                MakeIndexedWaterParameterName(TEXT("WaterExclusionCenter"), Index),
                FLinearColor(Transform.GetLocation().X, Transform.GetLocation().Y, Transform.GetLocation().Z, 1.0f));
            MID->SetVectorParameterValue(
                MakeIndexedWaterParameterName(TEXT("WaterExclusionExtent"), Index),
                FLinearColor(WorldExtent.X, WorldExtent.Y, WorldExtent.Z, 0.0f));
            MID->SetVectorParameterValue(
                MakeIndexedWaterParameterName(TEXT("WaterExclusionRotation"), Index),
                FLinearColor(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W));
        }
    }
}

void AWaterActor::UpdateUnderwaterPostProcessForView(const FVector& ViewLocation, const bool bExcluded)
{
    if (!IsValid(PostProcess.Get()))
    {
        return;
    }

    if (bGlobalOcean)
    {
        // Global-ocean PP is evaluated by the unified WorldEnvManager material through MPC values.
        PostProcess->bEnabled = false;
        return;
    }

    // Local water keeps its authored finite PostProcessComponent bounds. Exclusions can still turn
    // that effect off while the camera is inside an exclusion box.
    PostProcess->bEnabled = bAuthoredPostProcessEnabled && !bExcluded;
}

void AWaterActor::RefreshWaterExclusionRendering(const UObject* WorldContextObject)
{
    const UWorld* ConstWorld = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    UWorld* World = const_cast<UWorld*>(ConstWorld);
    if (!World)
    {
        return;
    }

    TArray<TWeakObjectPtr<AWaterActor>>* WaterActors = GWaterActorsByWorld.Find(World);
    if (!WaterActors)
    {
        return;
    }

    for (int32 Index = WaterActors->Num() - 1; Index >= 0; --Index)
    {
        AWaterActor* WaterActor = (*WaterActors)[Index].Get();
        if (!IsValid(WaterActor))
        {
            WaterActors->RemoveAtSwap(Index, 1, EAllowShrinking::No);
            continue;
        }
        WaterActor->ApplyWaterExclusionRenderParameters();
    }
}

void AWaterActor::UpdateLocalViewWaterEffects(const UObject* WorldContextObject, const FVector& ViewLocation)
{
    const UWorld* ConstWorld = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    UWorld* World = const_cast<UWorld*>(ConstWorld);
    if (!World)
    {
        return;
    }

    const bool bSuppressPostProcess = UWaterExclusionBoxComponent::IsLocationExcluded(
        WorldContextObject,
        ViewLocation,
        EWaterExclusionFeature::UnderwaterPostProcess);

    TArray<TWeakObjectPtr<AWaterActor>>* WaterActors = GWaterActorsByWorld.Find(World);
    if (!WaterActors)
    {
        return;
    }

    for (int32 Index = WaterActors->Num() - 1; Index >= 0; --Index)
    {
        AWaterActor* WaterActor = (*WaterActors)[Index].Get();
        if (!IsValid(WaterActor))
        {
            WaterActors->RemoveAtSwap(Index, 1, EAllowShrinking::No);
            continue;
        }
        if (WaterActor->IsGlobalOcean())
        {
            continue;
        }
        WaterActor->UpdateUnderwaterPostProcessForView(ViewLocation, bSuppressPostProcess);
    }
}

