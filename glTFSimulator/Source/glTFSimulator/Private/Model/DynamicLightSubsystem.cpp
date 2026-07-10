// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Model/DynamicLightSubsystem.h"
#include "Model/DynamicPointLightComponent.h"
#include "Async/ParallelFor.h"
#include "Engine/World.h"
#include "Subsystems/SubsystemCollection.h"
#include "System/ActorHelper.h"
#include "System/GameUpdateSubSystem.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

void UDynamicLightSubsystem::Initialize(FSubsystemCollectionBase &Collection)
{
    Super::Initialize(Collection);
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
        return;

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
}

void UDynamicLightSubsystem::UpdateLightsFromGameUpdate(float DeltaTime)
{
    if (ManagedLights.Num() == 0)
        return;

    // 1. Read the camera location once on the game thread.
    APlayerController *PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
    if (!IsValid(PC))
        return;

    FVector CameraLocation;
    FRotator CameraRotation;
    PC->GetPlayerViewPoint(CameraLocation, CameraRotation);

    // 2. Calculate visibility flags in parallel.
    // Contiguous TArray storage maximizes CPU cache efficiency.
    ParallelFor(ManagedLights.Num(), [this, &CameraLocation](int32 Index)
                {
        FLightOptimizationData& Data = ManagedLights[Index];
        
        // Skip invalid weak component references.
        if (!Data.LightComponent.IsValid()) return;

        // Use squared distance to avoid square-root cost.
        float DistSq = FVector::DistSquared(CameraLocation, Data.Position);

        if (DistSq < Data.CullingDistanceSq)
        {
            // Near the camera: light on, decal off.
            Data.bTargetLightVisibility = true;
            Data.bTargetDecalVisibility = false;
        }
        else if (Data.TargetDecalMaterial.IsValid() && Data.DecalTransitionDistanceSq > Data.CullingDistanceSq && DistSq < Data.DecalTransitionDistanceSq)
        {
            // Middle distance: light off, optional decal fallback on.
            Data.bTargetLightVisibility = false;
            Data.bTargetDecalVisibility = true;
        }
        else
        {
            // Too far away: cull both light and decal.
            Data.bTargetLightVisibility = false;
            Data.bTargetDecalVisibility = false;
        } });

    // 3. Apply render-facing state sequentially on the game thread.
    // UObject state changes and component creation are not thread-safe, so they are batched here.
    for (FLightOptimizationData &Data : ManagedLights)
    {
        UDynamicPointLightComponent *LightComp = Data.LightComponent.Get();
        if (!IsValid(LightComp))
            continue;

        // Apply the latest updated position for dynamic components.
        Data.Position = LightComp->GetComponentLocation();

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
                    //AActor *Owner = LightComp->GetOwner();
                    //if (!Owner)
                    //    return;
                    //FActorHelper::DestroyComponent(Owner, DecalComp);
                    DecalComp->SetVisibility(false);
                }
            }
        }
    }
}

UDecalComponent *UDynamicLightSubsystem::CreateDecalComponent(UDynamicPointLightComponent *LightComp, UMaterialInterface *Material)
{
    AActor *Owner = LightComp->GetOwner();
    if (!Owner)
        return nullptr;

    UDecalComponent *NewDecal = NewObject<UDecalComponent>(Owner);
    if (!NewDecal)
        return nullptr;

    Owner->AddInstanceComponent(NewDecal);
    // Attach the decal to the light owner root and match its location.
    NewDecal->AttachToComponent(Owner->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
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