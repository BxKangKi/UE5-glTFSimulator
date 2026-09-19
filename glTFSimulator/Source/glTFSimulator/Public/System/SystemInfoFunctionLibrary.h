/**
 * @file SystemInfoFunctionLibrary.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
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