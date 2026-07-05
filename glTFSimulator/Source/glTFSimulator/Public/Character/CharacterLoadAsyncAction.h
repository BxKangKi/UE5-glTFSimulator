// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeCounter.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "CharacterLoadAsyncAction.generated.h"

USTRUCT(BlueprintType)
struct FBoneMapWrapper
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Data")
    TMap<FString, FString> BoneMap;
};

class ACharacterController;
class UglTFRuntimeAsset;

// Multicast delegate used for the Blueprint output pin.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCharacterLoadCallback, bool, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCharacterLoadProgress, float, Progress);

UCLASS()
class GLTFSIMULATOR_API UCharacterLoadAsyncAction : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    // Static factory function callable from Blueprint.
    UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"), Category = "glTFSimulator|Async")
    static UCharacterLoadAsyncAction *LoadCharacterAsync(UObject *WorldContextObject, ACharacterController *InOwner, FString InPath);

    virtual void Activate() override;
    void CancelAndRelease();

    UPROPERTY(BlueprintAssignable)
    FCharacterLoadCallback OnCompleted;

    UPROPERTY(BlueprintAssignable)
    FCharacterLoadProgress OnProgress;

private:
    // Variables that keep async load state between steps.
    TWeakObjectPtr<ACharacterController> OwnerCharacter;
    FString FilePath;
    UPROPERTY()
    UglTFRuntimeAsset *CurrentLoadedAsset;
    TSharedPtr<FThreadSafeCounter, ESPMode::ThreadSafe> AssetLoadCancelToken;
    int32 AssetLoadRequestSerial = 0;
    bool bCancelled = false;
    bool bFinished = false;
    bool bMeshLoadInFlight = false;
    // Internal callbacks for each async step.
    UFUNCTION()
    void LoadAssetAsync();
    UFUNCTION()
    void OnglTFAssetLoaded(UglTFRuntimeAsset *Asset);
    void LoadBoneMapAsync();
    UFUNCTION()
    void OnMeshLoaded(USkeletalMesh *SkeletalMesh);
    // Helper functions ported from the previous flow.
    bool CheckRootBoneName(UglTFRuntimeAsset *Asset);
    void FinalizePhysics(USkeletalMesh *SkeletalMesh);
    void ReleaseCurrentAsset();
    void CancelActiveAssetLoad();
    void FinishAndRelease();
};