// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Model/DynamicLightSubsystem.h"
#include "Model/DynamicPointLightComponent.h"
#include "Async/ParallelFor.h"
#include "Engine/World.h"
#include "System/ActorHelper.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

void UDynamicLightSubsystem::Initialize(FSubsystemCollectionBase &Collection)
{
    Super::Initialize(Collection);
}

void UDynamicLightSubsystem::Deinitialize()
{
    ManagedLights.Empty();
    Super::Deinitialize();
}

void UDynamicLightSubsystem::RegisterLight(UDynamicPointLightComponent *InLight)
{
    if (!IsValid(InLight))
        return;

    FLightOptimizationData NewData;
    NewData.Position = InLight->GetComponentLocation();
    NewData.CullingDistanceSq = FMath::Square(InLight->GetCullingDistance());
    const bool bCanUseDecalFallback = InLight->IsLightDecalFallbackEnabled()
        && IsValid(InLight->GetLightDecal())
        && InLight->GetDecalTransitionDistance() > InLight->GetCullingDistance();
    NewData.DecalTransitionDistanceSq = bCanUseDecalFallback ? FMath::Square(InLight->GetDecalTransitionDistance()) : 0.0f;
    NewData.LightComponent = InLight;
    NewData.TargetDecalMaterial = bCanUseDecalFallback ? InLight->GetLightDecal() : nullptr;

    // 초기 상태 반영
    NewData.bCurrentLightVisibility = InLight->IsVisible();

    ManagedLights.Add(NewData);
}

void UDynamicLightSubsystem::UnregisterLight(UDynamicPointLightComponent *InLight)
{
    for (int32 i = ManagedLights.Num() - 1; i >= 0; --i)
    {
        if (ManagedLights[i].LightComponent.Get() == InLight)
        {
            // 동적 스폰된 디칼 컴포넌트가 있다면 소멸 처리
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

void UDynamicLightSubsystem::Tick(float DeltaTime)
{
    if (ManagedLights.Num() == 0)
        return;

    // 1. 카메라 위치 획득 (메인 스레드에서 1번만 안전하게 수행)
    APlayerController *PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
    if (!IsValid(PC))
        return;

    FVector CameraLocation;
    FRotator CameraRotation;
    PC->GetPlayerViewPoint(CameraLocation, CameraRotation);

    // 2. 가시성 플래그 연산 병렬 처리 (ParallelFor)
    // 연산 데이터가 연속적인 TArray 구조이므로 CPU 캐시 효율이 극대화됩니다.
    ParallelFor(ManagedLights.Num(), [this, &CameraLocation](int32 Index)
                {
        FLightOptimizationData& Data = ManagedLights[Index];
        
        // 유효하지 않은 약참조 컴포넌트 스킵
        if (!Data.LightComponent.IsValid()) return;

        // 제곱근 연산이 없는 DistSquared로 계산 비용 최소화
        float DistSq = FVector::DistSquared(CameraLocation, Data.Position);

        if (DistSq < Data.CullingDistanceSq)
        {
            // 카메라와 가까움: 라이트 ON, 디칼 OFF
            Data.bTargetLightVisibility = true;
            Data.bTargetDecalVisibility = false;
        }
        else if (Data.TargetDecalMaterial.IsValid() && Data.DecalTransitionDistanceSq > Data.CullingDistanceSq && DistSq < Data.DecalTransitionDistanceSq)
        {
            // 중간 거리: 라이트 OFF, optional decal fallback ON.
            Data.bTargetLightVisibility = false;
            Data.bTargetDecalVisibility = true;
        }
        else
        {
            // 너무 멀리 있음: 둘 다 컬링 (OFF)
            Data.bTargetLightVisibility = false;
            Data.bTargetDecalVisibility = false;
        } });

    // 3. 메인 스레드 순차 처리 (상태 렌더링 동기화)
    // UObject의 상태 조작 및 컴포넌트 생성은 스레드 안전하지 않으므로 여기서 몰아서 처리합니다.
    for (FLightOptimizationData &Data : ManagedLights)
    {
        UDynamicPointLightComponent *LightComp = Data.LightComponent.Get();
        if (!IsValid(LightComp))
            continue;

        // 동적으로 변화된 컴포넌트의 최신 갱신 위치 반영
        Data.Position = LightComp->GetComponentLocation();

        // [라이트 상태 적용]
        if (Data.bTargetLightVisibility != Data.bCurrentLightVisibility)
        {
            Data.bCurrentLightVisibility = Data.bTargetLightVisibility;
            LightComp->SetVisibility(Data.bCurrentLightVisibility);
        }

        // [디칼 상태 적용]
        if (Data.bTargetDecalVisibility != Data.bCurrentDecalVisibility)
        {
            Data.bCurrentDecalVisibility = Data.bTargetDecalVisibility;

            if (Data.bCurrentDecalVisibility)
            {
                // 디칼이 켜져야 하는데 아직 할당이 안 되었다면 런타임에 지연 생성(Lazy Initialization)
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
    // 디칼을 라이트의 오너 액터 루트에 부착하고 위치 일치화
    NewDecal->AttachToComponent(Owner->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
    NewDecal->SetWorldLocationAndRotation(LightComp->GetComponentLocation(), FRotator(-90.0f, 0.0f, 0.0f)); // 아래 방향 투영 기본값
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