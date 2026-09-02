#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "HAL/ThreadSafeCounter.h"
#include "Templates/Function.h"
#include "SimulatorSharedResourceSubsystem.generated.h"

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FSimulatorResourceLease
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FString Fingerprint;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 Generation = INDEX_NONE;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) bool bSourceAsset = true;

    bool IsValid() const { return !Fingerprint.IsEmpty() && Generation != INDEX_NONE; }
};

USTRUCT()
struct FSimulatorSharedResourceEntry
{
    GENERATED_BODY()

    UPROPERTY() TObjectPtr<UObject> Object = nullptr;
    UPROPERTY() int32 ReferenceCount = 0;
    UPROPERTY() int32 Generation = 1;
    UPROPERTY() double LastReleaseSeconds = 0.0;
};

using FSimulatorSharedAssetFactory = TFunction<UObject*()>;
using FSimulatorSharedAssetCompletion = TFunction<void(const FSimulatorResourceLease&, UObject*, const FString&)>;

/**
 * Project-level cache layered above glTFRuntime. The plugin's parser-local caches
 * remain enabled. Identical complete source files share one asset via a content
 * fingerprint; generated meshes/materials/textures may also be registered using
 * fingerprints derived from their actual source bytes, never a display name.
 */
UCLASS()
class GLTFSIMULATOR_API USimulatorSharedResourceSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Deinitialize() override;

    /** Hashing happens on a worker. Factory and completion always execute on the game thread. */
    int32 AcquireSourceAssetAsync(const FString& CanonicalPath, UObject* RequestOwner, FSimulatorSharedAssetFactory&& Factory, FSimulatorSharedAssetCompletion&& Completion);

    /**
     * Synchronous counterpart used by APIs that must return a glTFRuntime asset immediately.
     * LoaderVariant is folded into the content key, so identical source bytes loaded with
     * materially different glTFRuntime configs are never aliased to the same UObject.
     */
    FSimulatorResourceLease AcquireSourceAssetSync(const FString& CanonicalPath, FSimulatorSharedAssetFactory&& Factory, UObject*& OutAsset, FString& OutError, const FString& LoaderVariant = FString());

    /**
     * Convenience form for UObject owners. The returned asset is still held by the cache and
     * callers are expected to store it in a UPROPERTY/TObjectPtr. The temporary cache lease is
     * released before returning to avoid monotonically increasing reference counts.
     */
    UObject* AcquireSourceAssetSyncForOwner(const FString& CanonicalPath, UObject* RequestOwner, FSimulatorSharedAssetFactory&& Factory, FString& OutError, const FString& LoaderVariant = FString());

    FSimulatorResourceLease RegisterGeneratedResource(const FString& StableContentFingerprint, UObject* Resource);

    UFUNCTION(BlueprintCallable, Category="glTF|Shared Resources")
    void Release(const FSimulatorResourceLease& Lease);

    UFUNCTION(BlueprintPure, Category="glTF|Shared Resources")
    UObject* FindSourceAsset(const FString& Fingerprint) const;

    UFUNCTION(BlueprintPure, Category="glTF|Shared Resources")
    UObject* FindGeneratedResource(const FString& Fingerprint) const;

    UFUNCTION(BlueprintCallable, Category="glTF|Shared Resources")
    void CollectUnused(float MinimumUnusedSeconds = 30.0f);

    UFUNCTION(BlueprintPure, Category="glTF|Shared Resources")
    static FString FingerprintBytes(const TArray<uint8>& Bytes);

private:
    UPROPERTY(Transient) TMap<FString, FSimulatorSharedResourceEntry> SourceAssets;
    UPROPERTY(Transient) TMap<FString, FSimulatorSharedResourceEntry> GeneratedResources;
    uint64 CacheEpoch = 1;
    FThreadSafeCounter RequestCounter;

    static bool FingerprintFile(const FString& CanonicalPath, FString& OutFingerprint, FString& OutError);
    FSimulatorResourceLease AcquireOrCreateOnGameThread(const FString& Fingerprint, FSimulatorSharedAssetFactory& Factory, FString& OutError);
};
