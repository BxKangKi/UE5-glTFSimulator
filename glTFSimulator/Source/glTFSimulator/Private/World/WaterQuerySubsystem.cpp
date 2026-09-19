// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "World/WaterQuerySubsystem.h"
#include "World/WaterActor.h"
#include "World/WaterExclusionBoxComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace
{
    struct FWaterQueryTolerance
    {
        float Horizontal = 0.0f;
        float Surface = 5.0f;
        float LowerBounds = 2.0f;
    };

    FWaterQueryTolerance GetTolerance(const EWaterQueryMode Mode)
    {
        if (Mode == EWaterQueryMode::Relaxed)
        {
            return {35.0f, 20.0f, 400.0f};
        }
        return {0.0f, 5.0f, 2.0f};
    }

    EWaterExclusionFeature GetExclusionFeature(const EWaterQueryPurpose Purpose)
    {
        return Purpose == EWaterQueryPurpose::Buoyancy
            ? EWaterExclusionFeature::Buoyancy
            : EWaterExclusionFeature::WaterInteraction;
    }
}

UWaterQuerySubsystem* UWaterQuerySubsystem::Get(const UObject* WorldContextObject)
{
    UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    return World ? World->GetSubsystem<UWaterQuerySubsystem>() : nullptr;
}

void UWaterQuerySubsystem::RegisterWaterActor(AWaterActor* WaterActor)
{
    if (!IsValid(WaterActor) || WaterActor->GetWorld() != GetWorld())
    {
        return;
    }

    WaterActors.AddUnique(WaterActor);
    if (WaterActor->IsGlobalOcean())
    {
        GlobalOcean = WaterActor;
    }
}

void UWaterQuerySubsystem::UnregisterWaterActor(AWaterActor* WaterActor)
{
    WaterActors.RemoveAll([WaterActor](const TWeakObjectPtr<AWaterActor>& Entry)
    {
        return !Entry.IsValid() || Entry.Get() == WaterActor;
    });

    if (!GlobalOcean.IsValid() || GlobalOcean.Get() == WaterActor)
    {
        GlobalOcean.Reset();
    }
}

void UWaterQuerySubsystem::SetGlobalOceanActor(AWaterActor* WaterActor, const bool bIsGlobalOcean)
{
    if (!IsValid(WaterActor) || WaterActor->GetWorld() != GetWorld())
    {
        return;
    }

    WaterActors.AddUnique(WaterActor);
    if (bIsGlobalOcean)
    {
        GlobalOcean = WaterActor;
    }
    else if (GlobalOcean.Get() == WaterActor)
    {
        GlobalOcean.Reset();
    }
}

void UWaterQuerySubsystem::GetRegisteredWaterActors(TArray<AWaterActor*>& OutWaterActors)
{
    CompactInvalidWaterActors();
    OutWaterActors.Reset();
    OutWaterActors.Reserve(WaterActors.Num());
    for (const TWeakObjectPtr<AWaterActor>& Entry : WaterActors)
    {
        if (AWaterActor* WaterActor = Entry.Get(); IsValid(WaterActor))
        {
            OutWaterActors.Add(WaterActor);
        }
    }
}

void UWaterQuerySubsystem::CompactInvalidWaterActors()
{
    WaterActors.RemoveAll([](const TWeakObjectPtr<AWaterActor>& Entry)
    {
        return !Entry.IsValid();
    });

    if (!GlobalOcean.IsValid())
    {
        GlobalOcean.Reset();
    }
}

bool UWaterQuerySubsystem::GetGlobalOceanLevel(float& OutLevel) const
{
    const AWaterActor* WaterActor = GlobalOcean.Get();
    if (!IsValid(WaterActor) || !WaterActor->IsGlobalOcean())
    {
        return false;
    }

    OutLevel = WaterActor->Level;
    return true;
}

bool UWaterQuerySubsystem::QueryActorLocation(
    const AActor* Actor,
    const EWaterQueryMode Mode,
    const EWaterQueryPurpose Purpose,
    FWaterQueryResult& OutResult)
{
    if (!IsValid(Actor))
    {
        OutResult = FWaterQueryResult();
        return false;
    }
    return QueryWaterAtLocation(Actor->GetActorLocation(), Mode, Purpose, OutResult);
}

bool UWaterQuerySubsystem::QueryWaterAtLocation(
    const FVector& WorldLocation,
    const EWaterQueryMode Mode,
    const EWaterQueryPurpose Purpose,
    FWaterQueryResult& OutResult)
{
    OutResult = FWaterQueryResult();
    if (!GetWorld() || WorldLocation.ContainsNaN())
    {
        return false;
    }

    const EWaterExclusionFeature ExclusionFeature = GetExclusionFeature(Purpose);
    if (UWaterExclusionBoxComponent::IsLocationExcluded(this, WorldLocation, ExclusionFeature))
    {
        return false;
    }

    CompactInvalidWaterActors();
    const FWaterQueryTolerance Tolerance = GetTolerance(Mode);

    bool bFound = false;
    float BestSurfaceZ = -TNumericLimits<float>::Max();
    AWaterActor* BestWaterActor = nullptr;

    for (const TWeakObjectPtr<AWaterActor>& Entry : WaterActors)
    {
        AWaterActor* WaterActor = Entry.Get();
        if (!IsValid(WaterActor))
        {
            continue;
        }

        const float SurfaceZ = WaterActor->Level;
        bool bContainsPoint = false;

        if (WaterActor->IsGlobalOcean())
        {
            // Infinite XY ocean: visual mesh/decal bounds never participate in gameplay queries.
            bContainsPoint = WorldLocation.Z <= SurfaceZ + Tolerance.Surface;
        }
        else if (const UBoxComponent* WaterCollision = WaterActor->Collision.Get())
        {
            if (Mode == EWaterQueryMode::Strict)
            {
                const FVector LocalPoint = WaterCollision->GetComponentTransform().InverseTransformPosition(WorldLocation);
                const FVector LocalExtent = WaterCollision->GetUnscaledBoxExtent();
                bContainsPoint = FMath::Abs(LocalPoint.X) <= LocalExtent.X
                    && FMath::Abs(LocalPoint.Y) <= LocalExtent.Y
                    && LocalPoint.Z >= -LocalExtent.Z - Tolerance.LowerBounds
                    && WorldLocation.Z <= SurfaceZ + Tolerance.Surface;
            }
            else
            {
                const FBox Bounds = WaterCollision->Bounds.GetBox();
                bContainsPoint = Bounds.IsValid
                    && WorldLocation.X >= Bounds.Min.X - Tolerance.Horizontal
                    && WorldLocation.X <= Bounds.Max.X + Tolerance.Horizontal
                    && WorldLocation.Y >= Bounds.Min.Y - Tolerance.Horizontal
                    && WorldLocation.Y <= Bounds.Max.Y + Tolerance.Horizontal
                    && WorldLocation.Z >= Bounds.Min.Z - Tolerance.LowerBounds
                    && WorldLocation.Z <= SurfaceZ + Tolerance.Surface;
            }
        }
        else
        {
            const FBox Bounds = WaterActor->GetComponentsBoundingBox(true);
            bContainsPoint = Bounds.IsValid
                && WorldLocation.X >= Bounds.Min.X - Tolerance.Horizontal
                && WorldLocation.X <= Bounds.Max.X + Tolerance.Horizontal
                && WorldLocation.Y >= Bounds.Min.Y - Tolerance.Horizontal
                && WorldLocation.Y <= Bounds.Max.Y + Tolerance.Horizontal
                && WorldLocation.Z >= Bounds.Min.Z - Tolerance.LowerBounds
                && WorldLocation.Z <= SurfaceZ + Tolerance.Surface;
        }

        if (!bContainsPoint)
        {
            continue;
        }

        if (!bFound || SurfaceZ > BestSurfaceZ)
        {
            bFound = true;
            BestSurfaceZ = SurfaceZ;
            BestWaterActor = WaterActor;
        }
    }

    if (!bFound)
    {
        return false;
    }

    OutResult.bInWater = true;
    OutResult.SurfaceZ = BestSurfaceZ;
    OutResult.ImmersionDepth = BestSurfaceZ - WorldLocation.Z;
    OutResult.WaterActor = BestWaterActor;
    OutResult.bGlobalOcean = IsValid(BestWaterActor) && BestWaterActor->IsGlobalOcean();
    return true;
}

void UWaterQuerySubsystem::Deinitialize()
{
    WaterActors.Reset();
    GlobalOcean.Reset();
    Super::Deinitialize();
}
