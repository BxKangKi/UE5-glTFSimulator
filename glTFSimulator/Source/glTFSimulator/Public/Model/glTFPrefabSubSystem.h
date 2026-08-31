// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeCounter.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "glTFPrefabSubSystem.generated.h"

class UglTFRuntimeAsset;

DECLARE_DELEGATE_FourParams(
    FOnPrefabRuntimeAssetReady,
    UglTFRuntimeAsset*,
    int32,
    bool,
    const FString&);

/**
 * Owns and shares runtime prefab glTFRuntime assets across all streamed model nodes.
 *
 * A reference is owned by a concrete model-node token. Assets are unloaded only after the last
 * node reference and every active native mesh build using that asset have drained.
 */
UCLASS()
class GLTFSIMULATOR_API UglTFPrefabSubSystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    static UglTFPrefabSubSystem* Get(UObject* WorldContextObject);

    virtual void Deinitialize() override;

    /**
     * Acquires one node reference to a prefab named by the model JSON. The model path determines
     * the model root, and the prefab is resolved only from <model root>/prefab/<name>.glb.
     * The paired prefab JSON must declare AssetType="prefab".
     */
    void AcquirePrefabReference(
        const FString& ModelFilePath,
        const FString& PrefabName,
        const FString& ReferenceToken,
        FOnPrefabRuntimeAssetReady Callback);

    /** Releases one previously acquired node reference. Duplicate releases are ignored. */
    void ReleasePrefabReference(const FString& PrefabFilePath, const FString& ReferenceToken);

    /** Pins a loaded prefab asset while glTFRuntime is constructing a native mesh from it. */
    bool BeginPrefabAssetUse(const FString& PrefabFilePath, const FString& ReferenceToken);

    /** Ends one native mesh use; a zero-reference asset can then be unloaded. */
    void EndPrefabAssetUse(const FString& PrefabFilePath, const FString& ReferenceToken);

    /** Resolves the canonical prefab path without loading it. Returns empty for invalid names. */
    FString ResolvePrefabPath(const FString& ModelFilePath, const FString& PrefabName) const;

    int32 GetReferenceCount(const FString& PrefabFilePath) const;

private:
    struct FPrefabLoadResult;

    struct FPrefabRuntimeState
    {
        int32 ReferenceCount = 0;
        int32 ActiveUseCount = 0;
        bool bLoading = false;
        FString FailureReason;
        TSet<FString> ReferenceTokens;
        TSet<FString> ActiveUseTokens;
        TArray<FOnPrefabRuntimeAssetReady> PendingCallbacks;
        TSharedPtr<FThreadSafeCounter, ESPMode::ThreadSafe> LoadCancelToken;
    };

    /** Strong UObject ownership for loaded prefab assets. */
    UPROPERTY(Transient)
    TMap<FString, TObjectPtr<UglTFRuntimeAsset>> LoadedAssets;

    /** Non-reflected bookkeeping; LoadedAssets is the GC root for the actual UObject. */
    TMap<FString, FPrefabRuntimeState> RuntimeStates;

    bool EnsurePrefabSubsystemGameThread(const TCHAR* FunctionName) const;
    void StartPrefabLoad(const FString& PrefabFilePath, FPrefabRuntimeState& State);
    void FinishPrefabLoad(
        const FString& PrefabFilePath,
        TSharedPtr<FPrefabLoadResult, ESPMode::ThreadSafe> Result);
    void TryUnloadPrefab(const FString& PrefabFilePath);
    static FString NormalizePrefabName(const FString& PrefabName);
};
