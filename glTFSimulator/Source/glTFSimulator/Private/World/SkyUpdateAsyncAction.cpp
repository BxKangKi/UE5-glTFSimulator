// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "World/SkyUpdateAsyncAction.h"
#include "World/WorldData.h"
#include "System/MathHelper.h"

USkyUpdateAsyncAction *USkyUpdateAsyncAction::SkyUpdateAsync(UObject *WorldContextObject, UWorldData *Data)
{
    USkyUpdateAsyncAction *Action = NewObject<USkyUpdateAsyncAction>();
    Action->WorldContextObject = WorldContextObject;
    Action->Data = Data;
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}

FRotator USkyUpdateAsyncAction::CalculateSunRotation(UWorldData *World)
{
    FRotator Result = FRotator::ZeroRotator;
    if (IsValid(World))
    {
        float totalSeconds = FMath::Fmod(World->WorldTime, World->OneDayTime); // Seconds elapsed within the current day.
        float dayOfYear = (totalSeconds / World->OneDayTime) * World->OneYearDays;   // Current day of year, 1-365.
        float axialTiltRad = Deg2Rad * World->AxialTilt;
        float latitudeRad = Deg2Rad * World->Latitude;

        // Solar declination in radians.
        float sunDeclination = axialTiltRad * FMath::Sin(2.0f * __PI__ * (dayOfYear / World->OneYearDays));

        // Time in hours.
        float timeInHours = (totalSeconds / World->OneDayTime) * 24.0f;

        // Solar hour angle in degrees.
        float hourAngle = 15.0f * (timeInHours - 12.0f) + World->Longitude * 15.0f; // Longitude correction.

        // Altitude in radians.
        float altitude = FMath::Asin(
            FMath::Sin(latitudeRad) * FMath::Sin(sunDeclination) +
            FMath::Cos(latitudeRad) * FMath::Cos(sunDeclination) * FMath::Cos(Deg2Rad * hourAngle));

        // Azimuth in radians.
        float azimuth = FMath::Atan2(
            -FMath::Cos(sunDeclination) * FMath::Sin(Deg2Rad * hourAngle),
            FMath::Cos(latitudeRad) * FMath::Sin(sunDeclination) -
                FMath::Sin(latitudeRad) * FMath::Cos(sunDeclination) * FMath::Cos(Deg2Rad * hourAngle));

        // Convert to Cartesian coordinates.
        float x = FMath::Cos(altitude) * FMath::Cos(azimuth);
        float y = FMath::Cos(altitude) * FMath::Sin(azimuth);
        float z = FMath::Sin(altitude);

        FVector direction = FVector(x, y, z).GetSafeNormal();
        Result = FRotationMatrix::MakeFromX(direction).Rotator();
        Result.Normalize();
    }
    return Result;
}

void USkyUpdateAsyncAction::Activate()
{
    if (!IsValid(Data))
    {
        // Complete immediately with an empty vector when there is no data.
        OnCompleted.Broadcast(FLightRotation::Default());
        SetReadyToDestroy();
        return;
    }
    // Run the calculation on a background thread.
    Async(EAsyncExecution::ThreadPool, [this]()
          {
        // Perform the actual calculation.
        FLightRotation Result;
        FRotator SunRotator = CalculateSunRotation(Data);
        Result.Sun = SunRotator;
        FRotator MoonRotator = FRotator(-SunRotator.Pitch, SunRotator.Yaw + 180.0f, -SunRotator.Roll);
        MoonRotator.Normalize(); // Normalize the value to the 0-360 degree range.
        Result.Moon = MoonRotator;
        // Return to the game thread before broadcasting the delegate.
        AsyncTask(ENamedThreads::GameThread, [this, Result]()
                  {
            OnCompleted.Broadcast(Result);
            SetReadyToDestroy();}); });
}