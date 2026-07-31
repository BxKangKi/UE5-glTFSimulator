// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Model/DynamicLightSubsystem.h"
#include "Model/DynamicPointLightComponent.h"
#include "Engine/World.h"
#include "Subsystems/SubsystemCollection.h"
#include "System/GameUpdateSubSystem.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

void UDynamicLightSubsystem::Initialize(FSubsystemCollectionBase &Collection)
{
    Super::Initialize(Collection);
}

void UDynamicLightSubsystem::Deinitialize()
{
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateHandle);
    }
    GameUpdateHandle = INDEX_NONE;
    ManagedLights.Empty();
    Super::Deinitialize();
}

void UDynamicLightSubsystem::RegisterLight(UDynamicPointLightComponent *InLight)
{
    if (!IsValid(InLight))
    {
        return;
    }

    // Component re-registration must not create duplicate work or duplicate fallback decals.
    if (ManagedLights.ContainsByPredicate([InLight](const FLightOptimizationData& Data)
        {
            return Data.LightComponent.Get() == InLight;
        }))
    {
        return;
    }

    // Pay the per-frame update cost only while at least one managed light exists.
    if (GameUpdateHandle == INDEX_NONE)
    {
        if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
        {
            GameUpdateHandle = GameUpdate->RegisterUpdate(
                this,
                [this](const float DeltaSeconds)
                {
                    UpdateLightsFromGameUpdate(DeltaSeconds);
                },
                40);
        }
    }

    FLightOptimizationData NewData;
    NewData.Position = InLight->GetComponentLocation();
    NewData.CullingDistanceSq = FMath::Square(InLight->GetCullingDistance());
    const bool bCanUseDecalFallback = InLight->IsLightDecalFallbackEnabled()
        && IsValid(InLight->GetLightDecal())
        && InLight->GetDecalTransitionDistance() > InLight->GetCullingDistance();
    NewData.DecalTransitionDistanceSq = bCanUseDecalFallback ? FMath::Square(InLight->GetDecalTransitionDistance()) : 0.0f;
    NewData.LightComponent = InLight;
    NewData.TargetDecalMaterial = bCanUseDecalFallback ? InLight->GetLightDecal() : nullptr;

    // Apply the initial state.
    NewData.bCurrentLightVisibility = InLight->IsVisible();

    ManagedLights.Add(NewData);
}

void UDynamicLightSubsystem::UnregisterLight(UDynamicPointLightComponent *InLight)
{
    for (int32 i = ManagedLights.Num() - 1; i >= 0; --i)
    {
        if (ManagedLights[i].LightComponent.Get() == InLight)
        {
            // Destroy dynamically spawned decal components, if any.
            if (UDecalComponent *Decal = ManagedLights[i].DecalComponent.Get())
            {
                Decal->UnregisterComponent();
                Decal->DestroyComponent();
            }
            ManagedLights.RemoveAtSwap(i);
            break;
        }
    }

    if (ManagedLights.IsEmpty() && GameUpdateHandle != INDEX_NONE)
    {
        if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
        {
            GameUpdate->UnregisterUpdate(GameUpdateHandle);
        }
        GameUpdateHandle = INDEX_NONE;
    }
}

void UDynamicLightSubsystem::UpdateLightsFromGameUpdate(float /*DeltaTime*/)
{
    if (ManagedLights.Num() == 0)
    {
        return;
    }

    // Read the camera location once for the whole batch.
    APlayerController *PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
    if (!IsValid(PC))
    {
        return;
    }

    FVector CameraLocation;
    FRotator CameraRotation;
    PC->GetPlayerViewPoint(CameraLocation, CameraRotation);

    // Each light needs only one squared-distance comparison. A sequential contiguous pass is
    // cheaper than dispatching worker tasks and keeps every weak UObject access thread-safe.
    for (FLightOptimizationData &Data : ManagedLights)
    {
        UDynamicPointLightComponent *LightComp = Data.LightComponent.Get();
        if (!IsValid(LightComp))
        {
            continue;
        }

        // Dynamic components may move, so refresh their position before calculating visibility.
        Data.Position = LightComp->GetComponentLocation();
        const float DistanceSquared = FVector::DistSquared(CameraLocation, Data.Position);
        const bool bUseLight = DistanceSquared < Data.CullingDistanceSq;
        const bool bUseDecal = !bUseLight &&
            Data.TargetDecalMaterial.IsValid() &&
            Data.DecalTransitionDistanceSq > Data.CullingDistanceSq &&
            DistanceSquared < Data.DecalTransitionDistanceSq;
        Data.bTargetLightVisibility = bUseLight;
        Data.bTargetDecalVisibility = bUseDecal;

        // Apply light state.
        if (Data.bTargetLightVisibility != Data.bCurrentLightVisibility)
        {
            Data.bCurrentLightVisibility = Data.bTargetLightVisibility;
            LightComp->SetVisibility(Data.bCurrentLightVisibility);
        }

        // Apply decal state.
        if (Data.bTargetDecalVisibility != Data.bCurrentDecalVisibility)
        {
            Data.bCurrentDecalVisibility = Data.bTargetDecalVisibility;

            if (Data.bCurrentDecalVisibility)
            {
                // Lazily create the decal when it is needed but not yet allocated.
                UDecalComponent *DecalComp = Data.DecalComponent.Get();
                if (!IsValid(DecalComp) && Data.TargetDecalMaterial.IsValid())
                {
                    DecalComp = CreateDecalComponent(LightComp, Data.TargetDecalMaterial.Get());
                    Data.DecalComponent = DecalComp;
                }

                if (IsValid(DecalComp))
                {
                    DecalComp->SetVisibility(true);
                }
            }
            else
            {
                if (UDecalComponent *DecalComp = Data.DecalComponent.Get())
                {
                    DecalComp->SetVisibility(false);
                }
            }
        }
    }
}

UDecalComponent *UDynamicLightSubsystem::CreateDecalComponent(UDynamicPointLightComponent *LightComp, UMaterialInterface *Material)
{
    if (!IsValid(LightComp) || !IsValid(Material))
    {
        return nullptr;
    }

    AActor *Owner = LightComp->GetOwner();
    USceneComponent* RootComponent = IsValid(Owner) ? Owner->GetRootComponent() : nullptr;
    if (!IsValid(Owner) || !IsValid(RootComponent))
    {
        return nullptr;
    }

    UDecalComponent *NewDecal = NewObject<UDecalComponent>(Owner);
    if (!IsValid(NewDecal))
    {
        return nullptr;
    }

    Owner->AddInstanceComponent(NewDecal);
    // Attach the decal to the light owner root and match its location.
    NewDecal->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
    NewDecal->SetWorldLocationAndRotation(LightComp->GetComponentLocation(), FRotator(-90.0f, 0.0f, 0.0f)); // Default downward projection.
    // Clamp fallback decals so they cannot cover the whole scene with a white wash.
    const float LightRadius = FMath::Clamp(LightComp->AttenuationRadius, 1.0f, LightComp->GetMaxLightDecalSize());
    NewDecal->DecalSize = FVector(LightRadius, LightRadius, LightRadius);
    FLinearColor Color = LightComp->GetColorTemperature();
    Color.A = FMath::Clamp(LightComp->Intensity * 0.00001f, 0.0f, LightComp->GetMaxLightDecalOpacity());
    NewDecal->SetDecalColor(Color);
    NewDecal->SetDecalMaterial(Material);
    NewDecal->bDestroyOwnerAfterFade = false;
    NewDecal->RegisterComponent();
    return NewDecal;
}
