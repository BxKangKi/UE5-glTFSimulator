/**
 * @file SystemInfoFunctionLibrary.h
 * 역할: 플랫폼·시스템 정보를 Blueprint에 제공합니다.
 * 핵심 기능: 실행 환경 및 시스템 정보 조회.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SystemInfoFunctionLibrary.generated.h"

USTRUCT(BlueprintType)
struct FSystemHardwareInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "System Info")
    FString CPUBrand;

    UPROPERTY(BlueprintReadOnly, Category = "System Info")
    FString GPUBrand;

    UPROPERTY(BlueprintReadOnly, Category = "System Info")
    int32 CoreCount = 0;
};

UCLASS()
class GLTFSIMULATOR_API USystemInfoFunctionLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    // Exposed as a BlueprintPure function.
    UFUNCTION(BlueprintPure)
    static FSystemHardwareInfo GetSystemHardwareInfo();

    // Returns the current frame delta time in milliseconds.
    UFUNCTION(BlueprintPure)
    static float GetFramerate();

    // Returns currently used physical memory in megabytes.
    UFUNCTION(BlueprintPure)
    static int32 GetUsedMemory();

    // Returns total physical memory for percentage calculations.
    UFUNCTION(BlueprintPure)
    static int32 GetTotalMemory();
};