#include "System/SystemInfoFunctionLibrary.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformMemory.h"
#include "HardwareInfo.h"
#include "Misc/App.h"
#include "RHI.h"

FSystemHardwareInfo USystemInfoFunctionLibrary::GetSystemHardwareInfo()
{
    FSystemHardwareInfo Info;

    // 1. CPU 정보 추출 (브랜드명 및 물리 코어 수)
    Info.CPUBrand = FPlatformMisc::GetCPUBrand();
    Info.CoreCount = FPlatformMisc::NumberOfCores();

    // 2. GPU 정보 추출 (RHI가 초기화된 상태라면 전역 어댑터 네임 참조)
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
    // FApp::GetDeltaTime()은 초(Second) 단위이므로 1000을 곱해 ms로 변환합니다.
    // 렌더링 스레드와 동기화된 정확한 프레임 간의 시간입니다.
    return FMath::RoundToInt(1.0f / FMath::Max(FApp::GetDeltaTime(), 0.000001f));
}

int32 USystemInfoFunctionLibrary::GetUsedMemory()
{
    // FPlatformMemory::GetStats()를 통해 현재 하드웨어의 메모리 상태를 가져옵니다.
    FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
    // UsedPhysical(바이트 단위)을 MB 단위로 변환합니다. (1024 * 1024)
    return static_cast<int32>(MemoryStats.UsedPhysical / (1024 * 1024));
}

int32 USystemInfoFunctionLibrary::GetTotalMemory()
{
    FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
    // TotalPhysical(바이트 단위)을 MB 단위로 변환합니다.
    return static_cast<int32>(MemoryStats.TotalPhysical / (1024 * 1024));
}