// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file CharacterLoadAsyncAction.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
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
class UWorldBakedModelAsset;
class USkeleton;
class USkeletalMesh;
class UPhysicsAsset;
class UMaterialInterface;

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
    TObjectPtr<UWorldBakedModelAsset> CurrentLoadedAsset = nullptr;
    UPROPERTY(Transient)
    TObjectPtr<USkeleton> CurrentRuntimeSkeleton = nullptr;

    /** Source assets referenced by the copied native async config; retained until its callback drains. */
    UPROPERTY(Transient)
    TObjectPtr<USkeleton> SourceSkeletonReferenceGuard = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInterface> SourceMaterialReferenceGuard = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<USkeletalMesh> PendingSkeletalMesh = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UPhysicsAsset> PendingRuntimePhysicsAsset = nullptr;

    TMap<FString, FString> PendingBoneMap;
    FTimerHandle GameThreadStageTimer;
    bool bCancelled = false;
    bool bFinished = false;
    /** A requested .dat bundle read / mesh finalizer is active. Game-thread owned. */
    bool bMeshLoadInFlight = false;
    int32 ModelDatabaseRetryCount = 0;

    /** Mesh/skin pair selected from the baked node table. */
    int32 DetectedMeshIndex = INDEX_NONE;
    int32 DetectedSkinIndex = INDEX_NONE;

    void ResolveAndLoadModel();
    void ScheduleModelDatabaseRetry();

    UFUNCTION()
    void OnBakedAssetLoaded(UWorldBakedModelAsset *Asset);
    void ContinueWithEmbeddedBoneMap();

    /** Game-thread stage: creates only the UObject configuration needed to start glTFRuntime's worker-thread mesh build. */
    void BeginSkeletalMeshLoad_GameThread();

    UFUNCTION()
    void OnMeshLoaded(USkeletalMesh *SkeletalMesh);

    /** Game-thread stage: builds the transient physics asset on a separate frame from mesh finalization. */
    void BuildRuntimePhysics_GameThread();

    /** Game-thread stage: generates bodies/constraints only below the exact hairRoot bone. */
    void BuildHairPhysics_GameThread();

    /** Game-thread stage: generates bodies/constraints only below the exact dynRoot bone. */
    void BuildDynamicPhysics_GameThread();

    /** Game-thread stage: finalizes the already-mutated private Chaos PhysicsAsset exactly once. */
    void FinalizeRuntimePhysics_GameThread();

    /** Game-thread stage: performs the final atomic component swap. */
    void CommitRuntimeMesh_GameThread();

    void ScheduleGameThreadStage(void (UCharacterLoadAsyncAction::*StageFunction)());
    void ClearGameThreadStageTimer();
    bool CheckRootBoneName(UWorldBakedModelAsset *Asset);
    bool ResolveCharacterSkin(UWorldBakedModelAsset *Asset);
    void FailLoad(const FString& Reason);
    void ReleaseCurrentAsset();
    bool HasAsyncWorkInFlight() const;
    void TryFinishCancelledRequest();
    void FinishAndRelease();
};
