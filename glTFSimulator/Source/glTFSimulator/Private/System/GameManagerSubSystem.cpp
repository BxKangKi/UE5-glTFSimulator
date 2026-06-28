// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "System/GameManagerSubSystem.h"
#include "System/MacroLibrary.h"
#include "Setting/GameSettings.h"
#include "World/WorldData.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/GameUserSettings.h"
#include "System/SystemInfoFunctionLibrary.h"
#include "Components/PostProcessComponent.h"

UGameManagerSubSystem::UGameManagerSubSystem()
{
    bIsGamePaused = false;
    bIsWorldLoading = false;
    LoadingStatus = 0;
    TotalSumFPS = 0;
    TotalCountFPS = 0;
}

bool UGameManagerSubSystem::ShouldCreateSubsystem(UObject *Outer) const
{
    // 서브시스템 생성 여부 결정 (기본적으로 true 반환)
    return true;
}

void UGameManagerSubSystem::Initialize(FSubsystemCollectionBase &Collection)
{
    Super::Initialize(Collection);
    GameSettings = UGameSettings::CreateSettingsData(this);
    // 게임 인스턴스가 생성될 때 실행됨 (초기화 로직 작성)
}

void UGameManagerSubSystem::Deinitialize()
{
    // 게임 인스턴스가 종료될 때 실행됨 (자원 해제 등 해제 로직 작성)
    Super::Deinitialize();
}

void UGameManagerSubSystem::SaveSettings()
{
    if (IsValid(GameSettings))
    {
        GameSettings->SaveSettingsData();
    }
}

void UGameManagerSubSystem::TogglePause()
{
    if (bIsWorldLoading)
    {
        return;
    }
    SetGamePaused(!bIsGamePaused);
}

void UGameManagerSubSystem::SetGamePaused(bool bPaused)
{
    if (bIsWorldLoading && bPaused)
    {
        return;
    }
    bIsGamePaused = bPaused;
    if (UWorld* World = GetWorld())
    {
        UGameplayStatics::SetGamePaused(World, bIsGamePaused);
    }
}

void UGameManagerSubSystem::SetWorldLoading(bool bLoading)
{
    bIsWorldLoading = bLoading;
    if (bIsWorldLoading)
    {
        SetGamePaused(false);
    }
}

void UGameManagerSubSystem::UpdateSettings()
{
    if (IsValid(GameSettings) && IsValid(PostProcess))
    {
        GameSettings->UpdateSettings(PostProcess);
    }
}

UGameManagerSubSystem *UGameManagerSubSystem::GetSubSystem(AActor *InActor)
{
    return IsValid(InActor) ? GetSubSystem(InActor->GetWorld()) : nullptr;
}

UGameManagerSubSystem *UGameManagerSubSystem::GetSubSystem(UWorld *InWorld)
{
    if (!InWorld)
    {
        return nullptr;
    }

    UGameInstance *Instance = InWorld->GetGameInstance();
    if (IsValid(Instance))
    {
        return Instance->GetSubsystem<UGameManagerSubSystem>();
    }
    else
    {
        return nullptr;
    }
}


void UGameManagerSubSystem::ToggleFullscreen()
{
    UGameUserSettings *UserSettings = GEngine->GetGameUserSettings();
    if (IsValid(UserSettings))
    {
        // 현재 창 모드 확인
        EWindowMode::Type CurrentMode = UserSettings->GetFullscreenMode();

        // 전체화면(또는 창모드 전체화면)이면 창모드로, 창모드면 전체화면으로 전환
        EWindowMode::Type NewMode = (CurrentMode == EWindowMode::Windowed)
                                        ? EWindowMode::WindowedFullscreen // 또는 EWindowMode::Fullscreen (독점 전체화면)
                                        : EWindowMode::Windowed;

        UserSettings->SetFullscreenMode(NewMode);
        UserSettings->ApplySettings(false); // 해상도 변경 적용
    }
}

FString UGameManagerSubSystem::GetDebugText()
{
    FString Result = TEXT("[Debug]");
    Result.Append(LINE_TERMINATOR);
    Result = GetHardwareInfoText(Result);
    Result = GetFramerateInfoText(Result);
    return Result;
}

FString UGameManagerSubSystem::GetHardwareInfoText(FString InString)
{
    FSystemHardwareInfo Info = USystemInfoFunctionLibrary::GetSystemHardwareInfo();
    InString.Append(TEXT("CPU Name: ")).Append(Info.CPUBrand).Append(LINE_TERMINATOR);
    InString.Append(TEXT("CPU Core Count: ")).Append(FString::FromInt(Info.CoreCount));
    InString.Append(LINE_TERMINATOR);
    InString.Append(TEXT("GPU Name: ")).Append(Info.GPUBrand).Append(LINE_TERMINATOR);
    FString Used = FString::FromInt(USystemInfoFunctionLibrary::GetUsedMemory());
    FString Total = FString::FromInt(USystemInfoFunctionLibrary::GetTotalMemory());
    InString.Append(TEXT("Memory: ")).Append(Used).Append(TEXT(" / "));
    InString.Append(Total).Append(TEXT(" (MB)")).Append(LINE_TERMINATOR);
    return InString;
}

FString UGameManagerSubSystem::GetFramerateInfoText(FString InString)
{
    int32 FPS = FMath::RoundToInt(USystemInfoFunctionLibrary::GetFramerate());
    // Prevent overflow
    if (TotalSumFPS >= MAX_INT32 - FPS)
    {
        TotalSumFPS = 0;
        TotalCountFPS = 0;
    }
    TotalSumFPS += FPS;
    TotalCountFPS += 1;
    int32 AvgFPS = FMath::RoundToInt(TotalSumFPS / (float)TotalCountFPS);
    InString.Append(TEXT("FPS: ")).Append(FString::FromInt(FPS)).Append(LINE_TERMINATOR);
    InString.Append(TEXT("FPS(Avg): ")).Append(FString::FromInt(AvgFPS)).Append(LINE_TERMINATOR);
    return InString;
}