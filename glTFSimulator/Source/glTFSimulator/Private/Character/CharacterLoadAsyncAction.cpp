// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Character/CharacterLoadAsyncAction.h"

#include "Animation/Skeleton.h"
#include "Character/CharacterController.h"
#include "Character/CharacterFunctionLibrary.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeParser.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopeExit.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Setting/GameSettings.h"
#include "System/FileFunctionLibrary.h"
#include "System/GlbValidation.h"
#include "System/SafeFileIO.h"
#include "System/glTFRuntimeSafety.h"
#include "System/MacroLibrary.h"
#include "TimerManager.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    constexpr int64 MaxBoneMapJsonBytes = 16ll * 1024ll * 1024ll;
    constexpr int32 MaxSkeletonBonesForGeneratedSecondaryPhysics = 512;

    // Unity builds concatenate multiple .cpp files into one translation unit. The file-specific
    // prefix prevents this helper from colliding with similarly named model-loading validators.
    bool IsFiniteCharacterLoadVector(const FVector& Vector)
    {
        return FMath::IsFinite(Vector.X) &&
            FMath::IsFinite(Vector.Y) &&
            FMath::IsFinite(Vector.Z);
    }

    bool IsFiniteCharacterLoadQuat(const FQuat& Rotation)
    {
        return FMath::IsFinite(Rotation.X) &&
            FMath::IsFinite(Rotation.Y) &&
            FMath::IsFinite(Rotation.Z) &&
            FMath::IsFinite(Rotation.W);
    }

    bool IsFiniteCharacterLoadTransform(const FTransform& Transform)
    {
        const FQuat Rotation = Transform.GetRotation();
        return !Transform.ContainsNaN() &&
            IsFiniteCharacterLoadVector(Transform.GetLocation()) &&
            IsFiniteCharacterLoadVector(Transform.GetScale3D()) &&
            IsFiniteCharacterLoadQuat(Rotation) &&
            Rotation.IsNormalized();
    }

    void ReleaseTransientRuntimeObject(UObject* Object)
    {
        if (IsValid(Object) && !Object->IsAsset())
        {
            Object->ClearFlags(RF_Public | RF_Standalone);
            Object->SetFlags(RF_Transient);
        }
    }
}

UCharacterLoadAsyncAction* UCharacterLoadAsyncAction::LoadCharacterAsync(
    UObject* WorldContextObject,
    ACharacterController* InOwner,
    FString InPath)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("LoadCharacterAsync must create its UObject on the game thread")))
    {
        return nullptr;
    }

    UCharacterLoadAsyncAction* Action = NewObject<UCharacterLoadAsyncAction>();
    Action->OwnerCharacter = InOwner;
    Action->ReleaseObserver = InOwner;
    Action->FilePath = MoveTemp(InPath);
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}

void UCharacterLoadAsyncAction::Activate()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("UCharacterLoadAsyncAction::Activate must run on the game thread")))
    {
        TWeakObjectPtr<UCharacterLoadAsyncAction> WeakThis(this);
        if (!FSafeFileIO::DispatchTrackedGameThread([WeakThis]()
        {
            if (UCharacterLoadAsyncAction* StrongThis = WeakThis.Get())
            {
                StrongThis->Activate();
            }
        }))
        {
            // Module shutdown suppresses late UObject callbacks; no off-thread cleanup is safe here.
            return;
        }
        return;
    }

    bCancelled = false;
    bFinished = false;
    bAssetLoadInFlight = false;
    bBoneMapLoadInFlight = false;
    bMeshLoadInFlight = false;
    GlTFRuntimeOperationTicket = 0;
    DetectedMeshIndex = INDEX_NONE;
    DetectedSkinIndex = INDEX_NONE;
    CancelActiveAssetLoad();
    ClearGameThreadStageTimer();
    CurrentLoadedAsset = nullptr;
    CurrentRuntimeSkeleton = nullptr;
    PendingSkeletalMesh = nullptr;
    PendingRuntimePhysicsAsset = nullptr;
    PendingBoneMap.Empty();
    OnProgress.Broadcast(0.0f);

    FilePath = GlbValidation::NormalizePath(FilePath);
    if (!OwnerCharacter.IsValid())
    {
        FailLoad(TEXT("Character GLB preflight failed because the owner pawn is invalid"));
        return;
    }

    // File I/O, GLB structure validation, and parser construction are intentionally kept off
    // the game thread. UObject creation and component mutation happen only in later GT stages.
    LoadAssetAsync();
}

void UCharacterLoadAsyncAction::LoadAssetAsync()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("LoadAssetAsync dispatch must originate on the game thread")))
    {
        return;
    }

    if (bCancelled)
    {
        TryFinishCancelledRequest();
        return;
    }

    CancelActiveAssetLoad();
    const int32 RequestId = AssetLoadRequestSerial;
    const FString RequestedFilePath = FilePath;

    FglTFRuntimeConfig Config;
    Config.TransformBaseType = EglTFRuntimeTransformBaseType::YForward;
    Config.bAllowExternalFiles = true;

    TWeakObjectPtr<UCharacterLoadAsyncAction> WeakThis(this);
    TSharedPtr<FThreadSafeCounter, ESPMode::ThreadSafe> CancelToken =
        MakeShared<FThreadSafeCounter, ESPMode::ThreadSafe>(0);
    AssetLoadCancelToken = CancelToken;
    bAssetLoadInFlight = true;

    const bool bWorkerQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, RequestedFilePath, RequestId, Config, CancelToken]()
        {
            // BACKGROUND THREAD ONLY: filesystem access, GLB validation, and parser construction.
            // Project code performs no direct UObject creation/access or component mutation here;
            // glTFRuntime internally marshals any engine-object setup it requires to the game thread.
            FString ValidationReason;
            bool bCharacterFileValid = false;
            TSharedPtr<FglTFRuntimeParser> Parser;

            if (CancelToken.IsValid() && CancelToken->GetValue() == 0)
            {
                bCharacterFileValid =
                    GlbValidation::ValidateCharacterFile(RequestedFilePath, ValidationReason);

                if (bCharacterFileValid && CancelToken->GetValue() == 0)
                {
                    // Parser creation is serialized across the process. This project does not
                    // directly dereference a UObject in this worker stage.
                    Parser = FglTFRuntimeSafety::CreateParserSafely(RequestedFilePath, Config);
                    if (!Parser.IsValid() && ValidationReason.IsEmpty())
                    {
                        ValidationReason = TEXT("glTFRuntime parser creation was rejected or failed");
                    }
                }
            }

            // Always acknowledge completion on the game thread, including cancellation. The action
            // stays registered until this callback drains, which prevents a replacement character
            // request from starting a second parser job at the same time.
            if (!FSafeFileIO::DispatchTrackedGameThread(
                [WeakThis, RequestedFilePath, RequestId, Config, Parser, CancelToken,
                    bCharacterFileValid, ValidationReason]()
                {
                    UCharacterLoadAsyncAction* StrongThis = WeakThis.Get();
                    if (!IsValid(StrongThis))
                    {
                        return;
                    }

                    StrongThis->bAssetLoadInFlight = false;
                    if (StrongThis->AssetLoadCancelToken == CancelToken)
                    {
                        StrongThis->AssetLoadCancelToken.Reset();
                    }

                    const bool bRequestStillCurrent =
                        CancelToken.IsValid() && CancelToken->GetValue() == 0 &&
                        !StrongThis->bCancelled && !StrongThis->bFinished &&
                        StrongThis->AssetLoadRequestSerial == RequestId &&
                        StrongThis->OwnerCharacter.IsValid() &&
                        StrongThis->FilePath == RequestedFilePath &&
                        !IsGarbageCollecting();

                    if (!bRequestStillCurrent)
                    {
                        StrongThis->TryFinishCancelledRequest();
                        return;
                    }

                    if (!bCharacterFileValid)
                    {
                        StrongThis->FailLoad(FString::Printf(
                            TEXT("Character GLB structural preflight failed. Path=%s Reason=%s"),
                            *RequestedFilePath,
                            *ValidationReason));
                        return;
                    }

                    // GAME THREAD ONLY: UObjects, parser attachment, delegates, and component state.
                    UglTFRuntimeAsset* LoadedAsset = nullptr;
                    if (Parser.IsValid())
                    {
                        LoadedAsset = NewObject<UglTFRuntimeAsset>(StrongThis);
                        if (LoadedAsset)
                        {
                            LoadedAsset->RuntimeContextObject = Config.RuntimeContextObject;
                            LoadedAsset->RuntimeContextString = Config.RuntimeContextString;
                            if (!LoadedAsset->SetParser(Parser.ToSharedRef()))
                            {
                                // Central release owns cache teardown and keeps the asset alive until
                                // no native operation can still reference its parser.
                                FglTFRuntimeSafety::RequestAssetRelease(LoadedAsset);
                                LoadedAsset = nullptr;
                            }
                        }
                    }

                    StrongThis = WeakThis.Get();
                    if (!IsValid(StrongThis) || StrongThis->bCancelled || StrongThis->bFinished ||
                        StrongThis->AssetLoadRequestSerial != RequestId ||
                        !CancelToken.IsValid() || CancelToken->GetValue() != 0 ||
                        !StrongThis->OwnerCharacter.IsValid() ||
                        StrongThis->FilePath != RequestedFilePath)
                    {
                        if (IsValid(LoadedAsset))
                        {
                            FglTFRuntimeSafety::RequestAssetRelease(LoadedAsset);
                        }
                        if (IsValid(StrongThis))
                        {
                            StrongThis->TryFinishCancelledRequest();
                        }
                        return;
                    }

                    StrongThis->OnglTFAssetLoaded(LoadedAsset);
                }))
            {
                // No UObject state may be touched from this worker after shutdown starts.
                CancelToken->Set(1);
            }
        });

    if (!bWorkerQueued)
    {
        // Queue rejection happens on the game thread, so unwind the request synchronously.
        CancelToken->Set(1);
        if (AssetLoadCancelToken == CancelToken)
        {
            AssetLoadCancelToken.Reset();
        }
        bAssetLoadInFlight = false;
        bCancelled = true;
        FinishAndRelease();
    }
}

void UCharacterLoadAsyncAction::OnglTFAssetLoaded(UglTFRuntimeAsset* Asset)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("OnglTFAssetLoaded must run on the game thread")))
    {
        return;
    }

    if (bCancelled)
    {
        if (IsValid(Asset))
        {
            FglTFRuntimeSafety::RequestAssetRelease(Asset);
        }
        FinishAndRelease();
        return;
    }

    if (!IsValid(Asset) || !OwnerCharacter.IsValid())
    {
        CurrentLoadedAsset = Asset;
        FailLoad(FString::Printf(
            TEXT("Character glTF parser failed or owner became invalid. Path=%s"),
            *FilePath));
        return;
    }

    CurrentLoadedAsset = Asset;
    if (!ResolveCharacterSkin(Asset))
    {
        FailLoad(FString::Printf(
            TEXT("Character GLB has no valid skinned mesh node: %s"),
            *FilePath));
        return;
    }

    OnProgress.Broadcast(0.25f);
    LoadBoneMapAsync();
}

void UCharacterLoadAsyncAction::LoadBoneMapAsync()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("LoadBoneMapAsync dispatch must originate on the game thread")))
    {
        return;
    }

    if (bCancelled || bFinished)
    {
        TryFinishCancelledRequest();
        return;
    }

    TWeakObjectPtr<UCharacterLoadAsyncAction> WeakThis(this);
    const FString JsonPath = FPaths::ChangeExtension(FilePath, TEXT("json"));
    bBoneMapLoadInFlight = true;

    const bool bWorkerQueued = FSafeFileIO::RunTrackedWorker([WeakThis, JsonPath]()
    {
        // BACKGROUND THREAD ONLY: bounded file I/O and plain JSON/string processing.
        TMap<FString, FString> LocalBoneMap;
        FSafeJsonLimits Limits;
        Limits.MaxFileBytes = MaxBoneMapJsonBytes;
        Limits.MaxContainerEntries = 65536;
        Limits.MaxValues = 131072;
        Limits.bAllowBackupRecovery = false;
        const FSafeJsonLoadResult JsonResult = FSafeFileIO::LoadJsonBlocking(JsonPath, Limits);
        const TSharedPtr<FJsonObject>& Json = JsonResult.JsonObject;

        if (Json.IsValid())
        {
            for (const auto& Pair : Json->Values)
            {
                FString BoneValue;
                const FString BoneKey = FString(Pair.Key).TrimStartAndEnd();
                if (Pair.Value.IsValid() && Pair.Value->TryGetString(BoneValue))
                {
                    BoneValue = BoneValue.TrimStartAndEnd();
                    if (!BoneKey.IsEmpty() && !BoneValue.IsEmpty())
                    {
                        // External glTF/bone names remain name-based by design.
                        LocalBoneMap.Add(BoneValue, BoneKey);
                    }
                }
            }
        }

        if (!FSafeFileIO::DispatchTrackedGameThread(
            [WeakThis, LocalBoneMap = MoveTemp(LocalBoneMap)]() mutable
        {
            UCharacterLoadAsyncAction* StrongThis = WeakThis.Get();
            if (!IsValid(StrongThis))
            {
                return;
            }

            StrongThis->bBoneMapLoadInFlight = false;
            if (StrongThis->bCancelled || StrongThis->bFinished)
            {
                StrongThis->TryFinishCancelledRequest();
                return;
            }
            if (!IsValid(StrongThis->CurrentLoadedAsset) || !StrongThis->OwnerCharacter.IsValid())
            {
                StrongThis->FailLoad(TEXT("Character asset or owner became invalid before mesh creation"));
                return;
            }

            // GAME THREAD ONLY from here: UObject-backed state, delegates, timers, and mesh stages.
            StrongThis->PendingBoneMap = MoveTemp(LocalBoneMap);
            StrongThis->OnProgress.Broadcast(0.40f);
            StrongThis->ScheduleGameThreadStage(
                &UCharacterLoadAsyncAction::BeginSkeletalMeshLoad_GameThread);
        }))
        {
            // The action is owned by shutdown; do not mutate it from this worker.
            return;
        }
    });

    if (!bWorkerQueued)
    {
        // The worker never started; clear the drain flag before releasing the registered action.
        bBoneMapLoadInFlight = false;
        bCancelled = true;
        FinishAndRelease();
    }
}

void UCharacterLoadAsyncAction::BeginSkeletalMeshLoad_GameThread()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("BeginSkeletalMeshLoad_GameThread must run on the game thread")))
    {
        return;
    }

    GameThreadStageTimer.Invalidate();
    if (bCancelled || bFinished)
    {
        FinishAndRelease();
        return;
    }

    ACharacterController* Owner = OwnerCharacter.Get();
    if (!IsValid(Owner) || !IsValid(CurrentLoadedAsset) ||
        DetectedMeshIndex == INDEX_NONE || DetectedSkinIndex == INDEX_NONE)
    {
        FailLoad(TEXT("Character asset, owner, or skin became invalid before asynchronous mesh creation"));
        return;
    }

    USkeleton* DefaultSkeleton = Owner->DefaultAsset.Skeleton.Get();
    UMaterialInterface* Material = Owner->DefaultAsset.Material.Get();
    if (!IsValid(DefaultSkeleton) || !IsValid(Material))
    {
        FailLoad(TEXT("Character default skeleton or material is not assigned"));
        return;
    }

    // FglTFRuntimeSkeletalMeshConfig stores raw UObject pointers. The owner is deliberately weak and
    // may be destroyed during travel, so keep the source skeleton/material GC-safe on this action
    // until the native terminal callback has acknowledged completion or cancellation.
    SourceSkeletonReferenceGuard = DefaultSkeleton;
    SourceMaterialReferenceGuard = Material;

    // Creating/duplicating UObjects must remain on the game thread. The expensive glTF primitive,
    // skin-weight, render-buffer, material, and texture work is performed by glTFRuntime's async API.
    CurrentRuntimeSkeleton = UCharacterFunctionLibrary::DuplicateSkeleton(DefaultSkeleton);
    if (!IsValid(CurrentRuntimeSkeleton))
    {
        FailLoad(TEXT("Failed to create the transient runtime skeleton"));
        return;
    }

    FglTFRuntimeSkeletalMeshConfig Config;
    // The parser-owned ReadWrite cache is the single authoritative cache. It is cleared only by
    // RequestAssetRelease after the final native callback has returned its safety ticket.
    Config.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    Config.Skeleton = CurrentRuntimeSkeleton;
    Config.bOverwriteRefSkeleton = false;
    Config.bMergeAllBonesToBoneTree = true;
    Config.bIgnoreSkin = false;
    Config.OverrideSkinIndex = DetectedSkinIndex;
    Config.SkeletonConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    Config.SkeletonConfig.bAddRootBone = CheckRootBoneName(CurrentLoadedAsset);
    Config.SkeletonConfig.RootBoneName = TEXT("Root");
    Config.SkeletonConfig.BonesNameMap = PendingBoneMap;
    Config.SkeletonConfig.RootNodeIndex = -1;
    Config.SkeletonConfig.bClearRotations = true;
    Config.SkeletonConfig.CopyRotationsFrom = DefaultSkeleton;
    Config.SkeletonConfig.MaxNodesTreeDepth = -1;
    Config.SkeletonConfig.bAddRootNodeIfMissing = true;
    Config.MaterialsConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;

    TMap<EglTFRuntimeMaterialType, UMaterialInterface*> MaterialMap;
    MaterialMap.Add(EglTFRuntimeMaterialType::Opaque, Material);
    MaterialMap.Add(EglTFRuntimeMaterialType::Translucent, Material);
    MaterialMap.Add(EglTFRuntimeMaterialType::TwoSided, Material);
    MaterialMap.Add(EglTFRuntimeMaterialType::TwoSidedTranslucent, Material);
    MaterialMap.Add(EglTFRuntimeMaterialType::Masked, Material);
    MaterialMap.Add(EglTFRuntimeMaterialType::TwoSidedMasked, Material);
    Config.MaterialsConfig.UberMaterialsOverrideMap = MaterialMap;
    Config.MaterialsConfig.UnlitOverrideMap = MaterialMap;
    Config.MaterialsConfig.bGeneratesMipMaps = false;
    Config.MaterialsConfig.SpecularFactor = 0.0f;

    const int32 TextureDimensionLimit = UGameSettings::ResolveMaxTextureResolution(Owner);
    Config.MaterialsConfig.ImagesConfig.MaxWidth = TextureDimensionLimit;
    Config.MaterialsConfig.ImagesConfig.MaxHeight = TextureDimensionLimit;
    Config.MaterialsConfig.ImagesConfig.bCompressMips = false;
    Config.MaterialsConfig.ImagesConfig.bStreaming = false;
    Config.MaterialsConfig.bLoadMipMaps = false;
    Config.bIgnoreMissingBones = true;
    Config.Outer = GetTransientPackage();
    Config.bIgnoreEmptyMorphTargets = true;

    // Physics bodies are built in a separate game-thread stage from the directly assigned
    // physics template. Avoid asking the glTF worker/finalizer to generate a second asset.
    Config.bAutoGeneratePhysicsAssetBodies = false;
    Config.bAllowCPUAccess = false;

    OnProgress.Broadcast(0.55f);
    bMeshLoadInFlight = true;

    // Skeletal generation shares the same process-wide native slot as static mesh generation.
    const FglTFRuntimeSkeletalMeshConfig RequestedConfig = Config;
    TWeakObjectPtr<UCharacterLoadAsyncAction> WeakThis(this);
    const uint64 SubmittedTicket = FglTFRuntimeSafety::EnqueueOperation(
        this,
        CurrentLoadedAsset,
        FString::Printf(TEXT("Character mesh %s"), *FPaths::GetCleanFilename(FilePath)),
        [WeakThis, RequestedConfig](const uint64 Ticket)
        {
            UCharacterLoadAsyncAction* StrongThis = WeakThis.Get();
            if (!IsValid(StrongThis) || StrongThis->bCancelled || StrongThis->bFinished ||
                !IsValid(StrongThis->CurrentLoadedAsset))
            {
                if (IsValid(StrongThis))
                {
                    StrongThis->GlTFRuntimeOperationTicket = 0;
                    StrongThis->bMeshLoadInFlight = false;
                    StrongThis->TryFinishCancelledRequest();
                }
                // Finalize action/asset ownership before releasing the parser slot. The active
                // ticket is the lifetime barrier that keeps ClearCache from racing this branch.
                FglTFRuntimeSafety::CompleteOperation(Ticket);
                return;
            }

            StrongThis->GlTFRuntimeOperationTicket = Ticket;
            FglTFRuntimeSkeletalMeshAsync MeshDelegate;
            MeshDelegate.BindDynamic(StrongThis, &UCharacterLoadAsyncAction::OnMeshLoaded);
            // Load the exact mesh/skin pair found during validation. The recursive API assumes
            // scene index 0 when given an empty node name and can accidentally merge unrelated
            // props, so it is not reliable for arbitrary external character files.
            StrongThis->CurrentLoadedAsset->LoadSkeletalMeshAsync(
                StrongThis->DetectedMeshIndex,
                StrongThis->DetectedSkinIndex,
                MeshDelegate,
                RequestedConfig);
        },
        [WeakThis](const FString& Reason)
        {
            UCharacterLoadAsyncAction* StrongThis = WeakThis.Get();
            if (!IsValid(StrongThis))
            {
                return;
            }

            StrongThis->GlTFRuntimeOperationTicket = 0;
            StrongThis->bMeshLoadInFlight = false;
            if (StrongThis->bCancelled || StrongThis->bFinished)
            {
                StrongThis->TryFinishCancelledRequest();
                return;
            }

            StrongThis->FailLoad(FString::Printf(
                TEXT("Character mesh request was rejected by the glTFRuntime safety queue: %s"),
                *Reason));
        });

    // A ReadWrite cache hit may complete synchronously from inside EnqueueOperation. Preserve the
    // callback's terminal state instead of restoring a stale ticket after the call returns.
    if (bMeshLoadInFlight && GlTFRuntimeOperationTicket == 0)
    {
        GlTFRuntimeOperationTicket = SubmittedTicket;
    }
}

void UCharacterLoadAsyncAction::OnMeshLoaded(USkeletalMesh* SkeletalMesh)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("glTFRuntime skeletal-mesh callback must run on the game thread")))
    {
        return;
    }

    const uint64 CompletedTicket = GlTFRuntimeOperationTicket;
    GlTFRuntimeOperationTicket = 0;
    bMeshLoadInFlight = false;

    // Keep this parser's slot active through every callback-side UObject/reference mutation.
    // A cancellation/failure below may request cache release; it becomes executable only after
    // this scope returns the ticket and therefore cannot race this callback.
    ON_SCOPE_EXIT
    {
        FglTFRuntimeSafety::CompleteOperationAfterCallback(CompletedTicket);
    };

    if (bCancelled || bFinished)
    {
        ReleaseTransientRuntimeObject(SkeletalMesh);
        FinishAndRelease();
        return;
    }

    if (!IsValid(SkeletalMesh) || !OwnerCharacter.IsValid() ||
        SkeletalMesh->GetRefSkeleton().GetNum() <= 0)
    {
        ReleaseTransientRuntimeObject(SkeletalMesh);
        FglTFRuntimeSafety::ReportRecoverableFailure(
            FilePath,
            TEXT("glTFRuntime returned a null or structurally invalid skeletal mesh"));
        FailLoad(FString::Printf(
            TEXT("glTFRuntime returned an invalid character mesh. Path=%s"),
            *FilePath));
        return;
    }

    PendingSkeletalMesh = SkeletalMesh;
    ReleaseTransientRuntimeObject(PendingSkeletalMesh);
    OnProgress.Broadcast(0.80f);

    // Split finalization across frames so the mesh finalizer, physics setup, and component swap
    // never stack in one game-thread frame.
    ScheduleGameThreadStage(&UCharacterLoadAsyncAction::BuildRuntimePhysics_GameThread);
}

void UCharacterLoadAsyncAction::BuildRuntimePhysics_GameThread()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("BuildRuntimePhysics_GameThread must run on the game thread")))
    {
        return;
    }

    GameThreadStageTimer.Invalidate();
    if (bCancelled || bFinished)
    {
        FinishAndRelease();
        return;
    }

    if (!IsValid(PendingSkeletalMesh) || !OwnerCharacter.IsValid())
    {
        FailLoad(TEXT("Character mesh or owner became invalid before physics finalization"));
        return;
    }

    ACharacterController* Owner = OwnerCharacter.Get();
    UPhysicsAsset* PhysicsSource = IsValid(Owner) ? Owner->DefaultAsset.PhysicsAsset.Get() : nullptr;
    if (IsValid(PhysicsSource))
    {
        PendingRuntimePhysicsAsset = DuplicateObject<UPhysicsAsset>(
            PhysicsSource,
            Owner,
            MakeUniqueObjectName(
                Owner,
                UPhysicsAsset::StaticClass(),
                FName(TEXT("RuntimeCharacterPhysicsAsset"))));

        if (IsValid(PendingRuntimePhysicsAsset))
        {
            ReleaseTransientRuntimeObject(PendingRuntimePhysicsAsset);
            PendingRuntimePhysicsAsset = UCharacterFunctionLibrary::MergePhysicsAsset(
                PendingRuntimePhysicsAsset,
                nullptr,
                PendingSkeletalMesh);
        }
    }

    OnProgress.Broadcast(0.84f);
    ScheduleGameThreadStage(&UCharacterLoadAsyncAction::BuildHairPhysics_GameThread);
}

void UCharacterLoadAsyncAction::BuildHairPhysics_GameThread()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("BuildHairPhysics_GameThread must run on the game thread")))
    {
        return;
    }

    GameThreadStageTimer.Invalidate();
    if (bCancelled || bFinished)
    {
        FinishAndRelease();
        return;
    }

    if (!IsValid(PendingSkeletalMesh) || !OwnerCharacter.IsValid())
    {
        FailLoad(TEXT("Character mesh or owner became invalid during staged physics setup"));
        return;
    }

    const int32 BoneCount = PendingSkeletalMesh->GetRefSkeleton().GetNum();
    if (IsValid(PendingRuntimePhysicsAsset) &&
        BoneCount <= MaxSkeletonBonesForGeneratedSecondaryPhysics)
    {
        UCharacterFunctionLibrary::SetupAllBodiesBelowCollidersAndConstraints(
            PendingRuntimePhysicsAsset,
            PendingSkeletalMesh,
            BONE_HAIR_ROOT);
    }
    else if (IsValid(PendingRuntimePhysicsAsset) &&
        BoneCount > MaxSkeletonBonesForGeneratedSecondaryPhysics)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Skipping generated secondary-physics chains for a large character rig. Bones=%d Limit=%d Path=%s"),
            BoneCount,
            MaxSkeletonBonesForGeneratedSecondaryPhysics,
            *FilePath);
    }

    OnProgress.Broadcast(0.88f);
    ScheduleGameThreadStage(&UCharacterLoadAsyncAction::BuildDynamicPhysics_GameThread);
}

void UCharacterLoadAsyncAction::BuildDynamicPhysics_GameThread()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("BuildDynamicPhysics_GameThread must run on the game thread")))
    {
        return;
    }

    GameThreadStageTimer.Invalidate();
    if (bCancelled || bFinished)
    {
        FinishAndRelease();
        return;
    }

    if (!IsValid(PendingSkeletalMesh) || !OwnerCharacter.IsValid())
    {
        FailLoad(TEXT("Character mesh or owner became invalid during staged physics setup"));
        return;
    }

    if (IsValid(PendingRuntimePhysicsAsset) &&
        PendingSkeletalMesh->GetRefSkeleton().GetNum() <= MaxSkeletonBonesForGeneratedSecondaryPhysics)
    {
        UCharacterFunctionLibrary::SetupAllBodiesBelowCollidersAndConstraints(
            PendingRuntimePhysicsAsset,
            PendingSkeletalMesh,
            BONE_DYN_ROOT);
    }

    OnProgress.Broadcast(0.92f);
    ScheduleGameThreadStage(&UCharacterLoadAsyncAction::FinalizeRuntimePhysics_GameThread);
}

void UCharacterLoadAsyncAction::FinalizeRuntimePhysics_GameThread()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("FinalizeRuntimePhysics_GameThread must run on the game thread")))
    {
        return;
    }

    GameThreadStageTimer.Invalidate();
    if (bCancelled || bFinished)
    {
        FinishAndRelease();
        return;
    }

    ACharacterController* Owner = OwnerCharacter.Get();
    if (!IsValid(Owner) || !IsValid(PendingSkeletalMesh))
    {
        FailLoad(TEXT("Character mesh or owner became invalid before physics finalization"));
        return;
    }

    UPhysicsAsset* PhysicsSource = Owner->DefaultAsset.PhysicsAsset.Get();
    if (IsValid(PendingRuntimePhysicsAsset) && IsValid(PhysicsSource))
    {
        PendingRuntimePhysicsAsset = UCharacterFunctionLibrary::MergePhysicsAsset(
            PendingRuntimePhysicsAsset,
            PhysicsSource,
            PendingSkeletalMesh);
    }

    OnProgress.Broadcast(0.96f);
    ScheduleGameThreadStage(&UCharacterLoadAsyncAction::CommitRuntimeMesh_GameThread);
}

void UCharacterLoadAsyncAction::CommitRuntimeMesh_GameThread()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("CommitRuntimeMesh_GameThread must run on the game thread")))
    {
        return;
    }

    GameThreadStageTimer.Invalidate();
    if (bCancelled || bFinished)
    {
        FinishAndRelease();
        return;
    }

    ACharacterController* Owner = OwnerCharacter.Get();
    USkeletalMesh* MeshToCommit = PendingSkeletalMesh.Get();
    UPhysicsAsset* PhysicsToCommit = PendingRuntimePhysicsAsset.Get();
    USkeleton* SkeletonToCommit = CurrentRuntimeSkeleton.Get();

    const bool bSuccess =
        IsValid(Owner) && IsValid(MeshToCommit) &&
        Owner->CommitRuntimeCharacterResources(
            MeshToCommit,
            PhysicsToCommit,
            SkeletonToCommit);

    if (bSuccess)
    {
        // Ownership has moved to ACharacterController. Do not clear these committed resources in
        // FinishAndRelease; resetting the action's refs is enough.
        PendingSkeletalMesh = nullptr;
        PendingRuntimePhysicsAsset = nullptr;
        CurrentRuntimeSkeleton = nullptr;
    }
    else
    {
        ReleaseTransientRuntimeObject(PendingRuntimePhysicsAsset);
        ReleaseTransientRuntimeObject(PendingSkeletalMesh);
    }

    OnProgress.Broadcast(1.0f);
    OnCompleted.Broadcast(bSuccess);
    FinishAndRelease();
}

void UCharacterLoadAsyncAction::ScheduleGameThreadStage(
    void (UCharacterLoadAsyncAction::*StageFunction)())
{
    if (!ensureMsgf(IsInGameThread(), TEXT("Character load stage scheduling must run on the game thread")))
    {
        return;
    }

    if (bCancelled || bFinished || StageFunction == nullptr)
    {
        FinishAndRelease();
        return;
    }

    ClearGameThreadStageTimer();
    ACharacterController* Owner = OwnerCharacter.Get();
    UWorld* World = IsValid(Owner) ? Owner->GetWorld() : nullptr;
    if (!World)
    {
        FailLoad(TEXT("Character owner world became invalid while scheduling a game-thread stage"));
        return;
    }

    TWeakObjectPtr<UCharacterLoadAsyncAction> WeakThis(this);
    FTimerDelegate Delegate = FTimerDelegate::CreateLambda([WeakThis, StageFunction]()
    {
        if (UCharacterLoadAsyncAction* StrongThis = WeakThis.Get())
        {
            (StrongThis->*StageFunction)();
        }
    });
    GameThreadStageTimer = World->GetTimerManager().SetTimerForNextTick(Delegate);
}

void UCharacterLoadAsyncAction::ClearGameThreadStageTimer()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("Character load timer mutation must run on the game thread")))
    {
        return;
    }

    if (!GameThreadStageTimer.IsValid())
    {
        return;
    }

    if (ACharacterController* Owner = OwnerCharacter.Get())
    {
        if (UWorld* World = Owner->GetWorld())
        {
            World->GetTimerManager().ClearTimer(GameThreadStageTimer);
        }
    }
    GameThreadStageTimer.Invalidate();
}

bool UCharacterLoadAsyncAction::ResolveCharacterSkin(UglTFRuntimeAsset* Asset)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("ResolveCharacterSkin must run on the game thread")))
    {
        return false;
    }

    DetectedMeshIndex = INDEX_NONE;
    DetectedSkinIndex = INDEX_NONE;
    if (!IsValid(Asset))
    {
        return false;
    }

    const int32 MeshCount = Asset->GetNumMeshes();
    const TArray<FglTFRuntimeNode>& Nodes = Asset->GetNodes();
    constexpr int32 MaxSafeCharacterNodeCount = 65536;
    if (MeshCount <= 0 || Nodes.Num() <= 0 || Nodes.Num() > MaxSafeCharacterNodeCount)
    {
        return false;
    }

    for (const FglTFRuntimeNode& Node : Nodes)
    {
        if (Node.Index >= 0 && Node.Index < Nodes.Num() &&
            Node.MeshIndex >= 0 && Node.MeshIndex < MeshCount &&
            Node.SkinIndex >= 0 && IsFiniteCharacterLoadTransform(Node.Transform))
        {
            DetectedMeshIndex = Node.MeshIndex;
            DetectedSkinIndex = Node.SkinIndex;
            return true;
        }
    }
    return false;
}

void UCharacterLoadAsyncAction::FailLoad(const FString& Reason)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("FailLoad must run on the game thread")))
    {
        return;
    }

    if (bFinished)
    {
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("%s"), *Reason);
    UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("CharacterLoadAsyncAction"), Reason);
    OnProgress.Broadcast(1.0f);
    OnCompleted.Broadcast(false);
    FinishAndRelease();
}

void UCharacterLoadAsyncAction::CancelActiveAssetLoad()
{
    if (AssetLoadCancelToken.IsValid())
    {
        AssetLoadCancelToken->Set(1);
        AssetLoadCancelToken.Reset();
    }
    ++AssetLoadRequestSerial;
}

void UCharacterLoadAsyncAction::ReleaseCurrentAsset()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("ReleaseCurrentAsset must run on the game thread")))
    {
        return;
    }

    if (IsValid(CurrentLoadedAsset))
    {
        FglTFRuntimeSafety::RequestAssetRelease(CurrentLoadedAsset);
    }
    CurrentLoadedAsset = nullptr;

    ReleaseTransientRuntimeObject(PendingRuntimePhysicsAsset);
    ReleaseTransientRuntimeObject(PendingSkeletalMesh);
    ReleaseTransientRuntimeObject(CurrentRuntimeSkeleton);
    PendingRuntimePhysicsAsset = nullptr;
    PendingSkeletalMesh = nullptr;
    CurrentRuntimeSkeleton = nullptr;
    SourceSkeletonReferenceGuard = nullptr;
    SourceMaterialReferenceGuard = nullptr;
    PendingBoneMap.Empty();
}

void UCharacterLoadAsyncAction::CancelAndRelease()
{
    if (!IsInGameThread())
    {
        TWeakObjectPtr<UCharacterLoadAsyncAction> WeakThis(this);
        if (!FSafeFileIO::DispatchTrackedGameThread([WeakThis]()
        {
            if (UCharacterLoadAsyncAction* StrongThis = WeakThis.Get())
            {
                StrongThis->CancelAndRelease();
            }
        }))
        {
            // Shutdown owns final UObject teardown once new game-thread continuations are rejected.
            return;
        }
        return;
    }

    bCancelled = true;
    CancelActiveAssetLoad();
    FglTFRuntimeSafety::CancelQueuedOperations(this);
    ClearGameThreadStageTimer();
    OnCompleted.Clear();
    OnProgress.Clear();
    OwnerCharacter.Reset();
    FilePath.Reset();

    // Keep the async action alive until every worker has acknowledged cancellation. This prevents
    // parser, JSON, and glTFRuntime mesh jobs from overlapping with the next character request.
    TryFinishCancelledRequest();
}

bool UCharacterLoadAsyncAction::HasAsyncWorkInFlight() const
{
    return bAssetLoadInFlight || bBoneMapLoadInFlight || bMeshLoadInFlight;
}

void UCharacterLoadAsyncAction::TryFinishCancelledRequest()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("Cancelled character-load finalization must run on the game thread")))
    {
        return;
    }

    if (bCancelled && !bFinished && !HasAsyncWorkInFlight())
    {
        FinishAndRelease();
    }
}

void UCharacterLoadAsyncAction::FinishAndRelease()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("FinishAndRelease must run on the game thread")))
    {
        return;
    }

    if (bFinished)
    {
        return;
    }

    if (HasAsyncWorkInFlight())
    {
        // Cancellation may arrive while parser/JSON/glTFRuntime work is still running. Keep this
        // action registered and its UObject references alive until every GT drain callback fires.
        bCancelled = true;
        return;
    }

    bFinished = true;
    bCancelled = true;
    bAssetLoadInFlight = false;
    bBoneMapLoadInFlight = false;
    bMeshLoadInFlight = false;
    GlTFRuntimeOperationTicket = 0;
    FglTFRuntimeSafety::CancelQueuedOperations(this);
    CancelActiveAssetLoad();
    ClearGameThreadStageTimer();
    ReleaseCurrentAsset();
    OnCompleted.Clear();
    OnProgress.Clear();
    OwnerCharacter.Reset();
    FilePath.Reset();
    SetReadyToDestroy();

    // The owner may have queued a newer character while this request's worker/finalizer was
    // draining. Notify it only after all parser/generated-object references have been released.
    TWeakObjectPtr<ACharacterController> Observer = ReleaseObserver;
    ReleaseObserver.Reset();
    if (ACharacterController* Character = Observer.Get())
    {
        Character->HandleCharacterLoadActionReleased(this);
    }
}

bool UCharacterLoadAsyncAction::CheckRootBoneName(UglTFRuntimeAsset* Asset)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("CheckRootBoneName must run on the game thread")))
    {
        return true;
    }

    if (!IsValid(Asset))
    {
        return true;
    }

    for (const FglTFRuntimeNode& Node : Asset->GetNodes())
    {
        if (Node.Name.Equals(BONE_ROOT))
        {
            return false;
        }
    }
    return true;
}
