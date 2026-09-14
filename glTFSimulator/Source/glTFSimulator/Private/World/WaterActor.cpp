// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

/**
 * @file WaterActor.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "World/WaterActor.h"
#include "System/GlTFSimulatorGameInstance.h"
#include "System/GlTFSimulatorAssetRegistry.h"
#include "Character/CharacterController.h"
#include "Interface/WaterInteract.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Components/PostProcessComponent.h"
#include "GameFramework/PhysicsVolume.h"
#include "Materials/MaterialInterface.h"
#include "Components/DecalComponent.h"
#include "Components/ActorComponent.h"
#include "Engine/World.h"

namespace
{
    // Water probes can execute several times per frame. A weak per-world registry avoids a full
    // actor iterator for each probe while still allowing actors to disappear safely during travel.
    TMap<const UWorld*, TArray<TWeakObjectPtr<AWaterActor>>> GWaterActorsByWorld;

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
        Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        Collision->SetCollisionResponseToAllChannels(ECR_Overlap);
        Collision->SetGenerateOverlapEvents(true);
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
        if (!Registry->UnderWaterMaterial.IsNull())
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
    if (!IsValid(UnderWaterMaterial))
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
    APhysicsVolume *Volume = Collision->GetPhysicsVolume();
    if (Volume)
    {
        Volume->bWaterVolume = true;
    }
    UMaterialInstanceDynamic *DecalMID = IsValid(DecalMaterial) ? UMaterialInstanceDynamic::Create(DecalMaterial, this) : nullptr;
    if (DecalMID)
    {
        DecalMID->SetScalarParameterValue(TEXT("WaterLevel"), Level);
        Decal->SetDecalMaterial(DecalMID);
    }
    UMaterialInstanceDynamic *PostProcessMID = IsValid(UnderWaterMaterial) ? UMaterialInstanceDynamic::Create(UnderWaterMaterial, this) : nullptr;
    if (PostProcessMID)
    {
        // 2. Add this to the post-process component Blendables array with weight 1.0.
        PostProcess->AddOrUpdateBlendable(PostProcessMID, 1.0f);
        PostProcessMID->SetScalarParameterValue(FName("WaterLevel"), Level);
    }
}

void AWaterActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UnregisterWaterActor(this);
    Super::EndPlay(EndPlayReason);
}

void AWaterActor::WaterTrigger(AActor *OtherActor, bool InWater)
{
    if (!OtherActor)
    {
        return;
    }

    if (IWaterInteract* ActorWaterInteract = Cast<IWaterInteract>(OtherActor))
    {
        if (InWater)
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
            if (InWater)
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
    WaterTrigger(OtherActor, true);
}

void AWaterActor::NotifyActorEndOverlap(AActor* OtherActor)
{
    WaterTrigger(OtherActor, false);
}

void AWaterActor::SetCurrentLevel()
{
    FVector Location = GetActorLocation();
    Level = Location.Z;
}

void AWaterActor::CheckOverlappingWater(AActor *Target)
{
    // 1. Create an array that stores AWaterActor pointers.
    TArray<AActor *> OverlappingActors;
    // 2. Set the class filter to AWaterActor, matching the Blueprint ClassFilter pin.
    TSubclassOf<AActor> ClassFilter = AWaterActor::StaticClass();
    // 3. Collect only the filtered actors.
    Target->GetOverlappingActors(OverlappingActors, ClassFilter);
    // 4. ForEachLoop
    for (AActor *OverlappedActor : OverlappingActors)
    {
        if (OverlappedActor)
        {
            // 1. Cast the AActor to AWaterActor.
            AWaterActor *WaterActor = Cast<AWaterActor>(OverlappedActor);

            // 2. If the cast succeeds, call the water update function.
            if (WaterActor)
            {
                WaterActor->WaterTrigger(Target, true);
            }
        }
    }
}

bool AWaterActor::FindWaterLevelAtLocationStrict(const UObject *WorldContextObject, const FVector &WorldLocation, float &OutLevel)
{
    const UWorld *ConstWorld = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    UWorld *World = const_cast<UWorld *>(ConstWorld);
    if (!World)
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

        const UBoxComponent *WaterCollision = WaterActor->Collision.Get();
        const float WaterLevel = WaterActor->Level;
        bool bInsideBoxColumn = false;
        bool bInsideVerticalRange = false;

        if (WaterCollision)
        {
            // Strict means the tested point itself must be inside the actual BoxComponent
            // column.  Do not use the world AABB here: with a rotated/scaled box or a
            // capsule merely touching the side, the AABB can report water even after
            // the character has visually left the water volume.
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

        const float WaterLevel = WaterActor->Level;
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
