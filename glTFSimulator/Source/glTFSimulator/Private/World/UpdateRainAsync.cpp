// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "World/UpdateRainAsync.h"
#include "NiagaraComponent.h"
#include "Components/SceneCaptureComponent2D.h"

UUpdateRainAsync *UUpdateRainAsync::UpdateRainAsync(
    UObject *WorldContextObject,
    USceneCaptureComponent2D *ViewComp,
    UNiagaraComponent *NiagaraComp,
    const float InMaxViewDist,
    const FName &InParam)
{
    UUpdateRainAsync* Action = NewObject<UUpdateRainAsync>();
    Action->WorldContextObject = WorldContextObject;
    Action->ViewCompPtr = ViewComp;
    Action->NiagaraCompPtr = NiagaraComp;
    Action->MaxViewDist = FMath::Max(0.0f, InMaxViewDist);
    Action->Param = InParam;

    // Capture all component values while still on the game thread. Invalid Blueprint inputs are
    // completed safely by Activate instead of being dereferenced in the factory.
    if (IsValid(ViewComp))
    {
        ViewComp->MaxViewDistanceOverride = Action->MaxViewDist;
    }
    if (IsValid(WorldContextObject))
    {
        Action->RegisterWithGameInstance(WorldContextObject);
    }
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

    ApplyRainParameters(ViewCompPtr.Get(), NiagaraCompPtr.Get(), MaxViewDist, Param);

    Completed.Broadcast();
    SetReadyToDestroy();
}

bool UUpdateRainAsync::ApplyRainParameters(
    USceneCaptureComponent2D* ViewComp,
    UNiagaraComponent* NiagaraComp,
    const float InMaxViewDist,
    const FName& InParam)
{
    if (!IsValid(ViewComp) || !IsValid(NiagaraComp))
    {
        return false;
    }

    // The calculation is cheaper than a task-graph dispatch and only touches game-thread UObjects.
    const float MaxView = FMath::Max(0.0f, InMaxViewDist);
    ViewComp->MaxViewDistanceOverride = MaxView;
    const FTransform CaptureTransform = ViewComp->GetComponentTransform();
    const FVector Location = CaptureTransform.GetLocation();
    const FQuat Rotation = CaptureTransform.GetRotation();
    const FVector Forward = Rotation.GetForwardVector();
    const FVector Right = Rotation.GetRightVector();
    const FVector Up = Rotation.GetUpVector();
    const FPlane XPlane(Location.X, Location.Y, Location.Z, ViewComp->OrthoWidth);
    const FPlane YPlane(Right.X, Right.Y, Right.Z, 1.0f);
    const FPlane ZPlane(Up.X, Up.Y, Up.Z, ViewComp->CustomNearClippingPlane);
    const FPlane WPlane(Forward.X, Forward.Y, Forward.Z, MaxView);
    const FMatrix ViewMatrix(XPlane, YPlane, ZPlane, WPlane);

    NiagaraComp->SetFloatParameter(TEXT("MaxViewDistanceOverride"), MaxView);
    NiagaraComp->SetVariableMatrix(InParam, ViewMatrix);
    return true;
}
