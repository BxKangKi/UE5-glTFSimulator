#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "SimulatorRuntimeAssetSource.generated.h"

UINTERFACE(BlueprintType)
class GLTFSIMULATOR_API USimulatorRuntimeAssetSource : public UInterface
{
    GENERATED_BODY()
};

/** Implement on prefab/vehicle actors that can be configured from a model JSON/glTF path. */
class GLTFSIMULATOR_API ISimulatorRuntimeAssetSource
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="glTF|Runtime Asset")
    void SetRuntimeAssetSource(const FString& CanonicalSourcePath);
};
