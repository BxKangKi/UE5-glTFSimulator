/**
 * @file SystemInfoFunctionLibrary.cpp
 * 역할: 플랫폼·시스템 정보를 Blueprint에 제공합니다.
 * 핵심 기능: 실행 환경 및 시스템 정보 조회.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "System/SystemInfoFunctionLibrary.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformMemory.h"
#include "HardwareInfo.h"
#include "Misc/App.h"
#include "RHI.h"

FSystemHardwareInfo USystemInfoFunctionLibrary::GetSystemHardwareInfo()
{
    FSystemHardwareInfo Info;

    // 1. Read CPU information such as brand name and physical core count.
    Info.CPUBrand = FPlatformMisc::GetCPUBrand();
    Info.CoreCount = FPlatformMisc::NumberOfCores();

    // 2. Read GPU information from the global adapter name when RHI is initialized.
    if (GDynamicRHI && !GRHIAdapterName.IsEmpty())
    {
        Info.GPUBrand = GRHIAdapterName;
    }
    else
    {
        Info.GPUBrand = TEXT("Unknown");
    }
    return Info;
}


float USystemInfoFunctionLibrary::GetFramerate()
{
    // FApp::GetDeltaTime() is in seconds, so multiply by 1000 to convert to milliseconds.
    // This is the frame time synchronized with the render thread.
    return FMath::RoundToInt(1.0f / FMath::Max(FApp::GetDeltaTime(), 0.000001f));
}

int32 USystemInfoFunctionLibrary::GetUsedMemory()
{
    // Read current hardware memory stats through FPlatformMemory::GetStats().
    FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
    // Convert UsedPhysical from bytes to megabytes.
    return static_cast<int32>(MemoryStats.UsedPhysical / (1024 * 1024));
}

int32 USystemInfoFunctionLibrary::GetTotalMemory()
{
    FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
    // Convert TotalPhysical from bytes to megabytes.
    return static_cast<int32>(MemoryStats.TotalPhysical / (1024 * 1024));
}