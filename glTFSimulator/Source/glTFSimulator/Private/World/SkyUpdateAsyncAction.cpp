// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "World/SkyUpdateAsyncAction.h"
#include "World/WorldData.h"
#include "System/MathHelper.h"

USkyUpdateAsyncAction *USkyUpdateAsyncAction::SkyUpdateAsync(UObject *WorldContextObject, UWorldData *Data)
{
    if (!ensureMsgf(IsInGameThread(),
        TEXT("USkyUpdateAsyncAction::SkyUpdateAsync must create its UObject on the game thread")))
    {
        return nullptr;
    }

    USkyUpdateAsyncAction *Action = NewObject<USkyUpdateAsyncAction>();
    Action->WorldContextObject = WorldContextObject;
    Action->Data = Data;
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}

FRotator USkyUpdateAsyncAction::CalculateSunRotation(const UWorldData* World)
{
    if (!ensureMsgf(IsInGameThread(),
        TEXT("USkyUpdateAsyncAction::CalculateSunRotation reads UObject state and must run on the game thread")))
    {
        return FRotator::ZeroRotator;
    }

    FRotator Result = FRotator::ZeroRotator;
    if (IsValid(World) &&
        FMath::IsFinite(World->WorldTime) &&
        FMath::IsFinite(World->OneDayTime) &&
        FMath::IsFinite(World->OneYearDays) &&
        FMath::IsFinite(World->AxialTilt) &&
        FMath::IsFinite(World->Latitude) &&
        FMath::IsFinite(World->Longitude) &&
        World->OneDayTime > UE_SMALL_NUMBER &&
        World->OneYearDays > UE_SMALL_NUMBER)
    {
        // Wrap negative and positive times into a stable [0, OneDayTime) interval.
        float TotalSeconds = FMath::Fmod(World->WorldTime, World->OneDayTime);
        if (TotalSeconds < 0.0f)
        {
            TotalSeconds += World->OneDayTime;
        }

        const float DayOfYear = (TotalSeconds / World->OneDayTime) * World->OneYearDays;
        const float AxialTiltRadians = Deg2Rad * World->AxialTilt;
        const float LatitudeRadians = Deg2Rad * World->Latitude;

        // Solar declination follows the configured axial tilt over one simulated year.
        const float SunDeclination =
            AxialTiltRadians * FMath::Sin(2.0f * __PI__ * (DayOfYear / World->OneYearDays));

        const float TimeInHours = (TotalSeconds / World->OneDayTime) * 24.0f;
        const float HourAngle = 15.0f * (TimeInHours - 12.0f) + World->Longitude * 15.0f;
        const float HourAngleRadians = Deg2Rad * HourAngle;

        // Clamp numerical drift before asin so extreme settings cannot produce NaNs.
        const float AltitudeSine = FMath::Clamp(
            FMath::Sin(LatitudeRadians) * FMath::Sin(SunDeclination) +
                FMath::Cos(LatitudeRadians) * FMath::Cos(SunDeclination) * FMath::Cos(HourAngleRadians),
            -1.0f,
            1.0f);
        const float Altitude = FMath::Asin(AltitudeSine);

        const float Azimuth = FMath::Atan2(
            -FMath::Cos(SunDeclination) * FMath::Sin(HourAngleRadians),
            FMath::Cos(LatitudeRadians) * FMath::Sin(SunDeclination) -
                FMath::Sin(LatitudeRadians) * FMath::Cos(SunDeclination) * FMath::Cos(HourAngleRadians));

        const FVector Direction(
            FMath::Cos(Altitude) * FMath::Cos(Azimuth),
            FMath::Cos(Altitude) * FMath::Sin(Azimuth),
            FMath::Sin(Altitude));
        if (!Direction.IsNearlyZero())
        {
            Result = FRotationMatrix::MakeFromX(Direction.GetSafeNormal()).Rotator();
            Result.Normalize();
        }
    }
    return Result;
}

FLightRotation USkyUpdateAsyncAction::CalculateLightRotation(const UWorldData* World)
{
    FLightRotation Result = FLightRotation::Default();
    Result.Sun = CalculateSunRotation(World);
    Result.Moon = FRotator(-Result.Sun.Pitch, Result.Sun.Yaw + 180.0f, -Result.Sun.Roll);
    Result.Moon.Normalize();
    return Result;
}

void USkyUpdateAsyncAction::Activate()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("USkyUpdateAsyncAction::Activate must run on the game thread")))
    {
        return;
    }

    if (!IsValid(Data))
    {
        // Complete immediately with an empty vector when there is no data.
        OnCompleted.Broadcast(FLightRotation::Default());
        SetReadyToDestroy();
        return;
    }
    // This is only a handful of trigonometric operations. Keeping it on the game thread avoids
    // thread-pool scheduling overhead and, critically, avoids reading UObject properties off-thread.
    OnCompleted.Broadcast(CalculateLightRotation(Data));
    SetReadyToDestroy();
}
