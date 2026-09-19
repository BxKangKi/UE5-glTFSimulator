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
#include "World/WaterQuerySubsystem.h"
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

    if (!IsValid(StaticMesh.Get()) || !IsValid(StaticMesh->GetStaticMesh()))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Water actor has no render mesh. Configure AssetRegistry.WaterMesh or a derived WaterActor class."));
    }

    if (UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(this))
    {
        WaterQuery->RegisterWaterActor(this);
    }
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
    if (UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(this))
    {
        WaterQuery->UnregisterWaterActor(this);
    }
    Super::EndPlay(EndPlayReason);
}

void AWaterActor::SetGlobalOcean(const bool bInGlobalOcean)
{
    bGlobalOcean = bInGlobalOcean;
    SetCurrentLevel();

    if (UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(this))
    {
        WaterQuery->SetGlobalOceanActor(this, bGlobalOcean);
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

    // Use one uniform actor scale so the camera-following water surface covers the requested
    // circular view radius and the attached underwater decal also receives a sufficiently deep
    // projection volume. Global-ocean collision/underwater detection are height-based and do not
    // depend on this actor scale, so enlarging Z is visual-only for the global ocean.
    const double RequiredScale = static_cast<double>(SafeRadiusCm) / BaseHalfExtent;
    if (!FMath::IsFinite(RequiredScale) || RequiredScale <= UE_SMALL_NUMBER)
    {
        return;
    }

    FVector NewActorScale = GetActorScale3D();
    NewActorScale.X = RequiredScale;
    NewActorScale.Y = RequiredScale;
    NewActorScale.Z = RequiredScale;

    if (!NewActorScale.Equals(GetActorScale3D(), 0.001))
    {
        SetActorScale3D(NewActorScale);
    }
    GlobalOceanRenderRadiusCm = SafeRadiusCm;

    UE_LOG(LogTemp, Display,
        TEXT("Global ocean visual radius configured. Radius=%.1f m Mesh=%s ActorScaleXYZ=%.3f"),
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

    // Preserve local-water overlap compatibility, then resolve the height-driven global ocean
    // through the common query service. New gameplay code should query UWaterQuerySubsystem directly.
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

    if (UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(Target))
    {
        FWaterQueryResult Result;
        if (WaterQuery->QueryActorLocation(
                Target,
                EWaterQueryMode::Strict,
                EWaterQueryPurpose::Interaction,
                Result)
            && Result.bGlobalOcean
            && IsValid(Result.WaterActor))
        {
            Result.WaterActor->WaterTrigger(Target, true);
        }
    }
}

bool AWaterActor::FindGlobalOceanLevel(const UObject* WorldContextObject, float& OutLevel)
{
    if (UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(WorldContextObject))
    {
        return WaterQuery->GetGlobalOceanLevel(OutLevel);
    }
    return false;
}

bool AWaterActor::FindWaterLevelAtLocationStrict(const UObject *WorldContextObject, const FVector &WorldLocation, float &OutLevel)
{
    if (UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(WorldContextObject))
    {
        FWaterQueryResult Result;
        if (WaterQuery->QueryWaterAtLocation(
                WorldLocation,
                EWaterQueryMode::Strict,
                EWaterQueryPurpose::Interaction,
                Result))
        {
            OutLevel = Result.SurfaceZ;
            return true;
        }
    }
    return false;
}

bool AWaterActor::FindWaterLevelAtLocation(const UObject *WorldContextObject, const FVector &WorldLocation, float &OutLevel)
{
    if (UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(WorldContextObject))
    {
        FWaterQueryResult Result;
        if (WaterQuery->QueryWaterAtLocation(
                WorldLocation,
                EWaterQueryMode::Relaxed,
                EWaterQueryPurpose::Interaction,
                Result))
        {
            OutLevel = Result.SurfaceZ;
            return true;
        }
    }
    return false;
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
    UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(WorldContextObject);
    if (!WaterQuery)
    {
        return;
    }

    TArray<AWaterActor*> WaterActors;
    WaterQuery->GetRegisteredWaterActors(WaterActors);
    for (AWaterActor* WaterActor : WaterActors)
    {
        if (IsValid(WaterActor))
        {
            WaterActor->ApplyWaterExclusionRenderParameters();
        }
    }
}

void AWaterActor::UpdateLocalViewWaterEffects(const UObject* WorldContextObject, const FVector& ViewLocation)
{
    UWaterQuerySubsystem* WaterQuery = UWaterQuerySubsystem::Get(WorldContextObject);
    if (!WaterQuery)
    {
        return;
    }

    const bool bSuppressPostProcess = UWaterExclusionBoxComponent::IsLocationExcluded(
        WorldContextObject,
        ViewLocation,
        EWaterExclusionFeature::UnderwaterPostProcess);

    TArray<AWaterActor*> WaterActors;
    WaterQuery->GetRegisteredWaterActors(WaterActors);
    for (AWaterActor* WaterActor : WaterActors)
    {
        if (IsValid(WaterActor) && !WaterActor->IsGlobalOcean())
        {
            WaterActor->UpdateUnderwaterPostProcessForView(ViewLocation, bSuppressPostProcess);
        }
    }
}

