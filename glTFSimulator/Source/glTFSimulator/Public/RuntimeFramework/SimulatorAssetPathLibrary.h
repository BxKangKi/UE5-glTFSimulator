#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SimulatorAssetPathLibrary.generated.h"

UCLASS()
class GLTFSIMULATOR_API USimulatorAssetPathLibrary final : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="glTF|Path")
    static bool ResolveAssetReference(const FString& Reference, const TArray<FString>& AllowedRoots, FString& OutCanonicalPath, FString& OutError);

    UFUNCTION(BlueprintCallable, Category="glTF|Path")
    static bool ResolveSelectedReference(const FString& ExplicitReference, const TArray<FString>& References, int32 SelectedIndex,
        const TArray<FString>& AllowedRoots, FString& OutCanonicalPath, FString& OutError);
};
