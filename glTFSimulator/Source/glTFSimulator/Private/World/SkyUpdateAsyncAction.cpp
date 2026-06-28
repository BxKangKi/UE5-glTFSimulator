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
        float totalSeconds = FMath::Fmod(World->WorldTime, World->OneDayTime); // 하루 중 경과 시간 (초)
        float dayOfYear = (totalSeconds / World->OneDayTime) * World->OneYearDays;   // 현재 일수 (1~365)
        float axialTiltRad = Deg2Rad * World->AxialTilt;
        float latitudeRad = Deg2Rad * World->Latitude;

        // 태양 적위 (라디안)
        float sunDeclination = axialTiltRad * FMath::Sin(2.0f * __PI__ * (dayOfYear / World->OneYearDays));

        // 시간 (시)
        float timeInHours = (totalSeconds / World->OneDayTime) * 24.0f;

        // 태양 시간각 (도)
        float hourAngle = 15.0f * (timeInHours - 12.0f) + World->Longitude * 15.0f; // 경도 보정

        // 고도 (라디안)
        float altitude = FMath::Asin(
            FMath::Sin(latitudeRad) * FMath::Sin(sunDeclination) +
            FMath::Cos(latitudeRad) * FMath::Cos(sunDeclination) * FMath::Cos(Deg2Rad * hourAngle));

        // 방위각 (라디안)
        float azimuth = FMath::Atan2(
            -FMath::Cos(sunDeclination) * FMath::Sin(Deg2Rad * hourAngle),
            FMath::Cos(latitudeRad) * FMath::Sin(sunDeclination) -
                FMath::Sin(latitudeRad) * FMath::Cos(sunDeclination) * FMath::Cos(Deg2Rad * hourAngle));

        // Cartesian 좌표 변환
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
        // 데이터가 없으면 바로 완료 호출(빈 벡터)
        OnCompleted.Broadcast(FLightRotation::Default());
        SetReadyToDestroy();
        return;
    }
    // 백그라운드 스레드에서 계산 실행
    Async(EAsyncExecution::ThreadPool, [this]()
          {
        // 실제 계산
        FLightRotation Result;
        FRotator SunRotator = CalculateSunRotation(Data);
        Result.Sun = SunRotator;
        FRotator MoonRotator = FRotator(-SunRotator.Pitch, SunRotator.Yaw + 180.0f, -SunRotator.Roll);
        MoonRotator.Normalize(); // 값 정규화(각도 0~360 범위 맞춤)
        Result.Moon = MoonRotator;
        // 게임스레드로 다시 넘어와서 Delegate 실행
        AsyncTask(ENamedThreads::GameThread, [this, Result]()
                  {
            OnCompleted.Broadcast(Result);
            SetReadyToDestroy();}); });
}