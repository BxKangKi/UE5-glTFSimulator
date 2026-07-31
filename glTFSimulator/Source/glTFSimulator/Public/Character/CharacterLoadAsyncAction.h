// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeCounter.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "TimerManager.h"
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
class USkeleton;
class USkeletalMesh;
class UPhysicsAsset;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCharacterLoadCallback, bool, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCharacterLoadProgress, float, Progress);

UCLASS()
class GLTFSIMULATOR_API UCharacterLoadAsyncAction : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"), Category = "glTFSimulator|Async")
    static UCharacterLoadAsyncAction *LoadCharacterAsync(UObject *WorldContextObject, ACharacterController *InOwner, FString InPath);

    virtual void Activate() override;
    void CancelAndRelease();

    UPROPERTY(BlueprintAssignable)
    FCharacterLoadCallback OnCompleted;

    UPROPERTY(BlueprintAssignable)
    FCharacterLoadProgress OnProgress;

private:
    TWeakObjectPtr<ACharacterController> OwnerCharacter;
    /** Kept only as a weak cancellation observer so rapid character changes remain serialized. */
    TWeakObjectPtr<ACharacterController> ReleaseObserver;
    FString FilePath;
    UPROPERTY()
    TObjectPtr<UglTFRuntimeAsset> CurrentLoadedAsset = nullptr;
    UPROPERTY(Transient)
    TObjectPtr<USkeleton> CurrentRuntimeSkeleton = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<USkeletalMesh> PendingSkeletalMesh = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UPhysicsAsset> PendingRuntimePhysicsAsset = nullptr;

    TMap<FString, FString> PendingBoneMap;
    FTimerHandle GameThreadStageTimer;
    TSharedPtr<FThreadSafeCounter, ESPMode::ThreadSafe> AssetLoadCancelToken;
    int32 AssetLoadRequestSerial = 0;
    bool bCancelled = false;
    bool bFinished = false;
    /** Background GLB validation/parser construction is active. Written on the game thread only. */
    bool bAssetLoadInFlight = false;
    /** Background bone-map file/JSON parsing is active. Written on the game thread only. */
    bool bBoneMapLoadInFlight = false;
    /** glTFRuntime worker/finalizer is active. Written on the game thread only. */
    bool bMeshLoadInFlight = false;

    /** Ticket held while this request owns or waits for the global glTFRuntime mesh slot. */
    uint64 GlTFRuntimeOperationTicket = 0;

    /** Mesh/skin pair selected from a validated skinned node in the external GLB. */
    int32 DetectedMeshIndex = INDEX_NONE;
    int32 DetectedSkinIndex = INDEX_NONE;

    UFUNCTION()
    void LoadAssetAsync();
    UFUNCTION()
    void OnglTFAssetLoaded(UglTFRuntimeAsset *Asset);
    void LoadBoneMapAsync();

    /** Game-thread stage: creates only the UObject configuration needed to start glTFRuntime's worker-thread mesh build. */
    void BeginSkeletalMeshLoad_GameThread();

    UFUNCTION()
    void OnMeshLoaded(USkeletalMesh *SkeletalMesh);

    /** Game-thread stage: builds the transient physics asset on a separate frame from mesh finalization. */
    void BuildRuntimePhysics_GameThread();

    /** Game-thread stage: adds optional hair-chain bodies/constraints in its own frame. */
    void BuildHairPhysics_GameThread();

    /** Game-thread stage: adds optional dynamic-chain bodies/constraints in its own frame. */
    void BuildDynamicPhysics_GameThread();

    /** Game-thread stage: reapplies directly assigned template bodies and finalizes lookup tables. */
    void FinalizeRuntimePhysics_GameThread();

    /** Game-thread stage: performs the final atomic component swap. */
    void CommitRuntimeMesh_GameThread();

    void ScheduleGameThreadStage(void (UCharacterLoadAsyncAction::*StageFunction)());
    void ClearGameThreadStageTimer();
    bool CheckRootBoneName(UglTFRuntimeAsset *Asset);
    bool ResolveCharacterSkin(UglTFRuntimeAsset *Asset);
    void FailLoad(const FString& Reason);
    void ReleaseCurrentAsset();
    void CancelActiveAssetLoad();
    bool HasAsyncWorkInFlight() const;
    void TryFinishCancelledRequest();
    void FinishAndRelease();
};
