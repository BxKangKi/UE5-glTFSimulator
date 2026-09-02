#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "glTFRuntimeFunctionLibrary.h"
#include "RuntimeFramework/SimulatorSharedResourceSubsystem.h"
#include "SimulatorGlTFRuntimeCacheLibrary.generated.h"

/** Exact-version adapter around the bundled UglTFRuntimeFunctionLibrary filename API. */
UCLASS()
class GLTFSIMULATOR_API USimulatorGlTFRuntimeCacheLibrary final : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    static UglTFRuntimeAsset* LoadSharedAssetFromFilename(UObject* WorldContextObject,
        const FString& Filename,
        const bool bPathRelativeToContent,
        const FglTFRuntimeConfig& LoaderConfig);

    UFUNCTION(BlueprintCallable, Category="glTF|Shared Resources", meta=(WorldContext="WorldContextObject"))
    static UglTFRuntimeAsset* LoadSharedAssetFromFilenameWithLease(UObject* WorldContextObject,
        const FString& Filename,
        const bool bPathRelativeToContent,
        const FglTFRuntimeConfig& LoaderConfig,
        FSimulatorResourceLease& OutLease, FString& OutError);
};
