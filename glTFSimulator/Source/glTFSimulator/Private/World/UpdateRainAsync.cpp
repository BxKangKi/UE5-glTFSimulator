// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "World/UpdateRainAsync.h"
#include "Async/Async.h"
#include "NiagaraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "NiagaraFunctionLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Math/RotationMatrix.h"

UUpdateRainAsync *UUpdateRainAsync::UpdateRainAsync(
    UObject *WorldContextObject,
    USceneCaptureComponent2D *ViewComp,
    UNiagaraComponent *NiagaraComp,
    const float InMaxViewDist,
    const FName &InParam)
{
    ViewComp->MaxViewDistanceOverride = InMaxViewDist;
    auto *Action = NewObject<UUpdateRainAsync>();
    Action->WorldContextObject = WorldContextObject->GetWorld();
    Action->ViewCompPtr = ViewComp;
    Action->NiagaraCompPtr = NiagaraComp;
    Action->OrthoWidth = ViewComp->OrthoWidth;
    Action->NearPlane = ViewComp->CustomNearClippingPlane;
    Action->MaxViewDist = InMaxViewDist;
    Action->Param = InParam;
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}

void UUpdateRainAsync::Activate()
{
    if (!ViewCompPtr.IsValid() || !NiagaraCompPtr.IsValid())
    {
        Completed.Broadcast();
        SetReadyToDestroy();
        return;
    }

    // On game thread, before async
    const FTransform CaptureTransform = ViewCompPtr->GetComponentTransform();
    const float Ortho = OrthoWidth;
    const float Near = NearPlane;
    const float MaxView = MaxViewDist;
    const FName LocalParam = Param;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
              [CaptureTransform, Ortho, Near, MaxView, LocalParam, this]()
              {
                  const FVector Location = CaptureTransform.GetLocation();
                  const FRotator Rot = CaptureTransform.GetRotation().Rotator();
                  const FVector Forward = UKismetMathLibrary::GetForwardVector(Rot);
                  const FVector Right = UKismetMathLibrary::GetRightVector(Rot);
                  const FVector Up = UKismetMathLibrary::GetUpVector(Rot);
                  const FPlane XPlane(Location.X, Location.Y, Location.Z, Ortho);
                  const FPlane YPlane(Right.X, Right.Y, Right.Z, 1.0f);
                  const FPlane ZPlane(Up.X, Up.Y, Up.Z, Near);
                  const FPlane WPlane(Forward.X, Forward.Y, Forward.Z, MaxView);
                  const FMatrix ViewMatrix(XPlane, YPlane, ZPlane, WPlane);

                  AsyncTask(ENamedThreads::GameThread,
                            [this, ViewMatrix, MaxView, LocalParam]()
                            {
                                if (!NiagaraCompPtr.IsValid())
                                {
                                    Completed.Broadcast();
                                    SetReadyToDestroy();
                                    return;
                                }

                                UNiagaraComponent *NiagaraComp = NiagaraCompPtr.Get();
                                NiagaraComp->SetFloatParameter(TEXT("MaxViewDistanceOverride"), MaxView);
                                NiagaraComp->SetVariableMatrix(LocalParam, ViewMatrix);

                                Completed.Broadcast();
                                SetReadyToDestroy();
                            });
              });
}