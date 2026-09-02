#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SimulatorBoundsCacheSubsystem.generated.h"

UENUM(BlueprintType)
enum class ESimulatorBoundsSource : uint8
{
    None,
    SCZ,
    RuntimeMeasured
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FSimulatorCachedBounds
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FBox LocalBox = FBox(ForceInit);
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) ESimulatorBoundsSource Source = ESimulatorBoundsSource::None;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FString SourceFingerprint;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 SchemaRevision = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) bool bRuntimeValidated = false;
};

/**
 * Bounds policy: use SCZ metadata before loading, measure registered primitive
 * components once after loading, then cache by source fingerprint/revision.
 * No caller needs to recalculate bounds every frame.
 */
UCLASS()
class GLTFSIMULATOR_API USimulatorBoundsCacheSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category="glTF|Bounds")
    bool TryGetBounds(const FString& ResourceKey, const FString& SourceFingerprint, int32 SchemaRevision, FSimulatorCachedBounds& OutBounds) const;

    UFUNCTION(BlueprintCallable, Category="glTF|Bounds")
    void StoreSCZBounds(const FString& ResourceKey, const FBox& LocalBox, const FString& SourceFingerprint, int32 SchemaRevision);

    UFUNCTION(BlueprintCallable, Category="glTF|Bounds")
    bool MeasureActorBoundsOnce(const FString& ResourceKey, AActor* LoadedActor, const FString& SourceFingerprint, int32 SchemaRevision, FSimulatorCachedBounds& OutBounds);

    UFUNCTION(BlueprintCallable, Category="glTF|Bounds")
    void InvalidateResource(const FString& ResourceKey);

    UFUNCTION(BlueprintCallable, Category="glTF|Bounds")
    void InvalidateFingerprint(const FString& SourceFingerprint);

private:
    UPROPERTY(Transient) TMap<FString, FSimulatorCachedBounds> Entries;
    static FBox CalculateActorLocalBounds(AActor* Actor);
};
