// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "World/WaterActor.h"
#include "Character/CharacterController.h"
#include "Interface/WaterInteract.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PostProcessComponent.h"
#include "GameFramework/PhysicsVolume.h"
#include "Materials/MaterialInterface.h"
#include "Components/DecalComponent.h"
#include "Components/ActorComponent.h"
#include "EngineUtils.h"

AWaterActor::AWaterActor()
{
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    Decal = CreateDefaultSubobject<UDecalComponent>(TEXT("StaticMesh"));
    Decal->SetupAttachment(RootComponent);
    Decal->SetWorldLocation(FVector(0.0f, 0.0f, -1.0f));
    Decal->DecalSize = FVector(1.0f, 1.0f, 1.0f);
    Collision = CreateDefaultSubobject<UBoxComponent>(TEXT("Collision"));
    Collision->SetupAttachment(RootComponent);
    Collision->SetWorldLocation(FVector(0.0f, 0.0f, -1.0f));
    Collision->SetBoxExtent(FVector(1.0f, 1.0f, 1.0f));
    Collision->SetEnableGravity(false);
    Collision->SetVisibility(false);
    PostProcess = CreateDefaultSubobject<UPostProcessComponent>(TEXT("PostProces"));
    PostProcess->SetupAttachment(Collision);
    PostProcess->bUnbound = false;
    PostProcess->bEnabled = true;
    PostProcess->BlendRadius = 5.0f;
    StaticMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WaterStaticMesh"));
    StaticMesh->SetupAttachment(RootComponent);
    StaticMesh->SetEnableGravity(false);
    StaticMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    StaticMesh->SetCastShadow(false);
}

void AWaterActor::BeginPlay()
{
    Super::BeginPlay();
    SetCurrentLevel();
    APhysicsVolume *Volume = Collision->GetPhysicsVolume();
    if (Volume)
    {
        Volume->bWaterVolume = true;
    }
    UMaterialInstanceDynamic *DecalMID = UMaterialInstanceDynamic::Create(DecalMaterial, this);
    if (DecalMID)
    {
        DecalMID->SetScalarParameterValue(TEXT("WaterLevel"), Level);
        Decal->SetDecalMaterial(DecalMID);
    }
    UMaterialInstanceDynamic *PostProcessMID = UMaterialInstanceDynamic::Create(UnderWaterMaterial, this);
    if (PostProcessMID)
    {
        // 2. Add this to the post-process component Blendables array with weight 1.0.
        PostProcess->AddOrUpdateBlendable(PostProcessMID, 1.0f);
        PostProcessMID->SetScalarParameterValue(FName("WaterLevel"), Level);
    }
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

    TArray<UActorComponent*> Components;
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

    for (TActorIterator<AWaterActor> It(World); It; ++It)
    {
        AWaterActor *WaterActor = *It;
        if (!IsValid(WaterActor))
        {
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

    for (TActorIterator<AWaterActor> It(World); It; ++It)
    {
        AWaterActor *WaterActor = *It;
        if (!IsValid(WaterActor))
        {
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