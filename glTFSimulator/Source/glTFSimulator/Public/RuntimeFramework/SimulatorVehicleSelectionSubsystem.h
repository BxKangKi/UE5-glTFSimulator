#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SimulatorVehicleSelectionSubsystem.generated.h"

UCLASS()
class GLTFSIMULATOR_API USimulatorVehicleSelectionSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    UFUNCTION(BlueprintCallable, Category="Vehicle|Selection") void SetAllowedAssetRoots(const TArray<FString>& Roots);
    UFUNCTION(BlueprintCallable, Category="Vehicle|Selection") void SetVehicleReferences(const TArray<FString>& References, int32 PreserveIndex = -1);
    UFUNCTION(BlueprintCallable, Category="Vehicle|Selection") bool SelectVehicle(int32 Index, FString& OutError);
    UFUNCTION(BlueprintCallable, Category="Vehicle|Selection") void SetExplicitVehicleReference(const FString& Reference);
    UFUNCTION(BlueprintCallable, Category="Vehicle|Selection") bool ResolveSelectedVehiclePath(FString& OutCanonicalPath, FString& OutError) const;
    UFUNCTION(BlueprintCallable, Category="Vehicle|Selection", meta=(WorldContext="WorldContextObject"))
    AActor* SpawnSelectedVehicle(UObject* WorldContextObject, TSubclassOf<AActor> VehicleClass, const FTransform& Transform, FString& OutError);

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Vehicle|Selection") int32 SelectedIndex = INDEX_NONE;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Vehicle|Selection") FString ExplicitReference;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Vehicle|Selection") TArray<FString> VehicleReferences;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Vehicle|Selection") TArray<FString> AllowedAssetRoots;
};
