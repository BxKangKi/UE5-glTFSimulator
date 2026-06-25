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
    int32 CoreCount;
};

UCLASS()
class GLTFSIMULATOR_API USystemInfoFunctionLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    // 블루프린트에서 순수 함수(Pure)로 호출할 수 있도록 노출
    UFUNCTION(BlueprintPure)
    static FSystemHardwareInfo GetSystemHardwareInfo();

    // 현재 프레임 타임(Delta Time)을 밀리초(ms) 단위로 반환합니다.
    UFUNCTION(BlueprintPure)
    static float GetFramerate();

    // 현재 사용 중인 물리 메모리(RAM) 용량을 메가바이트(MB) 단위로 반환합니다.
    UFUNCTION(BlueprintPure)
    static int32 GetUsedMemory();

    // 시스템의 전체 물리 메모리(RAM) 용량을 반환합니다. (퍼센트 계산용)
    UFUNCTION(BlueprintPure)
    static int32 GetTotalMemory();
};