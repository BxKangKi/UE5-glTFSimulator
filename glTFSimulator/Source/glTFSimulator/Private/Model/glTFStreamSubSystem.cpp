// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Model/glTFStreamSubSystem.h"

#include "Character/CharacterController.h"
#include "CoreGlobals.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Model/glTFStreamActor.h"
#include "System/ActorHelper.h"
#include "System/BinaryDataStore.h"
#include "System/FileFunctionLibrary.h"
#include "System/GameManagerSubSystem.h"
#include "System/GlbValidation.h"
#include "System/MacroLibrary.h"
#include "System/SafeFileIO.h"
#include "TimerManager.h"

namespace
{
    constexpr float GLTF_STREAM_DISTANCE_SCALE = 64.0f;
    constexpr float STREAM_POLL_INTERVAL_SECONDS = 0.10f;
    constexpr double PLAYER_ACTOR_WAIT_TIMEOUT_SECONDS = 30.0;
    constexpr double PLAYER_LOAD_TIMEOUT_SECONDS = 120.0;
    constexpr double MODEL_LOAD_TIMEOUT_SECONDS = 180.0;
    constexpr int32 MAX_CONCURRENT_INITIAL_MODEL_LOADS = 3;

    /** Pure-data result produced off the game thread for one initial model path. */
    struct FInitialModelPreflightResult
    {
        bool bGlbValid = false;
        bool bSizeCacheExists = false;
        bool bMetadataValid = false;
        FString Reason;
        FModelData Metadata;
    };

    // The file-specific name avoids anonymous-namespace redefinition when Unreal Unity Build
    // combines this source file with another loader that performs a similar finite-vector check.
    bool IsFiniteStreamMetadataVector(const FVector& Value)
    {
        return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
    }

    bool IsUsableModelMetadata(const FModelData& ModelData)
    {
        return IsFiniteStreamMetadataVector(ModelData.Center) &&
            IsFiniteStreamMetadataVector(ModelData.Size) &&
            !ModelData.Size.IsNearlyZero(0.001f);
    }

    bool EnsureStreamSubsystemGameThread(const TCHAR* Context)
    {
        return ensureMsgf(IsInGameThread(), TEXT("%s must run on the game thread"), Context);
    }

    void NormalizeAndDeduplicatePaths(TArray<FString>& Paths)
    {
        TArray<FString> UniquePaths;
        UniquePaths.Reserve(Paths.Num());
        TSet<FString> SeenPathKeys;
        SeenPathKeys.Reserve(Paths.Num());

        for (const FString& Path : Paths)
        {
            const FString NormalizedPath = GlbValidation::NormalizePath(Path);
            if (NormalizedPath.IsEmpty())
            {
                continue;
            }

            const FString CaseInsensitiveKey = NormalizedPath.ToLower();
            if (!SeenPathKeys.Contains(CaseInsensitiveKey))
            {
                SeenPathKeys.Add(CaseInsensitiveKey);
                UniquePaths.Add(NormalizedPath);
            }
        }

        UniquePaths.Sort([](const FString& A, const FString& B)
        {
            return A.Compare(B, ESearchCase::IgnoreCase) < 0;
        });
        Paths = MoveTemp(UniquePaths);
    }
}

UglTFStreamSubSystem* UglTFStreamSubSystem::Get(UObject* WorldContextObject)
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::Get")))
    {
        return nullptr;
    }

    if (!IsValid(WorldContextObject))
    {
        return nullptr;
    }

    UWorld* World = WorldContextObject->GetWorld();
    UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
    return IsValid(GameInstance) ? GameInstance->GetSubsystem<UglTFStreamSubSystem>() : nullptr;
}

void UglTFStreamSubSystem::Deinitialize()
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::Deinitialize")))
    {
        return;
    }

    StopMainWorldStreaming();
    Super::Deinitialize();
}

void UglTFStreamSubSystem::SetRenderOnlyStreaming(const bool bInRenderOnlyStreaming)
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::SetRenderOnlyStreaming")))
    {
        return;
    }

    bRenderOnlyStreaming = bInRenderOnlyStreaming;
}

bool UglTFStreamSubSystem::IsRenderOnlyStreaming() const
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::IsRenderOnlyStreaming")))
    {
        return false;
    }

    return bRenderOnlyStreaming;
}

bool UglTFStreamSubSystem::IsActiveForWorld(const UWorld* World) const
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::IsActiveForWorld")))
    {
        return false;
    }

    return bActive && World && IsValid(OwnerActor) && OwnerActor->GetWorld() == World;
}

void UglTFStreamSubSystem::StartMainWorldStreaming(AActor* InOwnerActor, TSubclassOf<AglTFStreamActor> InSpawnActorClass, const FString& InModelDirectory, const FString& InPlayerDirectory, const FString& InInitialPlayerName, bool bInRenderOnlyStreaming)
{
    if (!IsInGameThread())
    {
        TWeakObjectPtr<UglTFStreamSubSystem> WeakThis(this);
        TWeakObjectPtr<AActor> WeakOwner(InOwnerActor);
        FSafeFileIO::DispatchTrackedGameThread(
            [WeakThis, WeakOwner, InSpawnActorClass, InModelDirectory, InPlayerDirectory,
                InInitialPlayerName, bInRenderOnlyStreaming]()
            {
                if (UglTFStreamSubSystem* StrongThis = WeakThis.Get())
                {
                    StrongThis->StartMainWorldStreaming(
                        WeakOwner.Get(),
                        InSpawnActorClass,
                        InModelDirectory,
                        InPlayerDirectory,
                        InInitialPlayerName,
                        bInRenderOnlyStreaming);
                }
            });
        return;
    }

    if (!IsValid(InOwnerActor) || !InSpawnActorClass)
    {
        WriteLogAsync(TEXT("StartMainWorldStreaming skipped: owner actor or SpawnActorClass is invalid"));
        return;
    }

    StopMainWorldStreaming();

    OwnerActor = InOwnerActor;
    SpawnActorClass = InSpawnActorClass;
    ModelDirectory = InModelDirectory;
    PlayerDirectory = InPlayerDirectory;
    InitialPlayerName = InInitialPlayerName;
    bRenderOnlyStreaming = bInRenderOnlyStreaming;
    bActive = true;
    bInitialPathScanComplete = false;
    bInitialPlayerLoadComplete = false;
    bInitialPlayerLoadStarted = false;
    bWaitingForPlayerLoad = false;
    bPlayerActivated = false;
    bPendingPlayerIsInitialLoad = false;
    PlayerActorWaitStartedAt = 0.0;
    PlayerLoadStartedAt = 0.0;
    NextModelFileAuditTime = 0.0;
    CurrentPathIndex = 0;
    CurrentPlayerPathIndex = INDEX_NONE;
    PendingPlayerPathIndex = INDEX_NONE;
    CurrentPlayerPath.Reset();
    PendingPlayerPath.Reset();
    ++InitialScanGeneration;
    ActiveInitialPreflightCount = 0;
    ActiveInitialActorScans.Empty();
    ActivePlayerCharacter.Reset();
    InitialPathProgress.Empty();
    LastReportedLoadingStatus = 0.0f;
    LastLoadingProgressFrame = ~uint64(0);
    CompletedInitialPaths.Empty();
    MissingFilePaths.Empty();
    ValidatedModelPaths.Empty();
    FailedPlayerPaths.Empty();
    MetadataUnavailablePaths.Empty();
    ModelMetadataMap.Empty();
    SpawnActorMap.Empty();

    GlbFilePaths = UFileFunctionLibrary::GetFileNamesWithExtension(ModelDirectory, TEXT("glb"));
    NormalizeAndDeduplicatePaths(GlbFilePaths);
    InitialPathProgress.Reserve(GlbFilePaths.Num());
    for (const FString& GlbPath : GlbFilePaths)
    {
        InitialPathProgress.Add(GlbPath, 0.0f);
    }
    DiscoverPlayerPaths();

    WriteLogAsync(FString::Printf(TEXT("glTFStreamSubSystem started. ModelDirectory=%s GLBCount=%d PlayerDirectory=%s PlayerCount=%d InitialPlayer=%s RenderOnly=%s"),
        *ModelDirectory,
        GlbFilePaths.Num(),
        *PlayerDirectory,
        PlayerGlbFilePaths.Num(),
        *InitialPlayerName,
        bRenderOnlyStreaming ? TEXT("true") : TEXT("false")));

    // Initial GLB validation/metadata parsing and metadata-less bounds calculation run in a
    // bounded three-file pipeline. UObject/component work is still marshalled to the game thread.
    ScheduleProcessNextPath();
}

void UglTFStreamSubSystem::StopMainWorldStreaming()
{
    if (!IsInGameThread())
    {
        TWeakObjectPtr<UglTFStreamSubSystem> WeakThis(this);
        FSafeFileIO::DispatchTrackedGameThread([WeakThis]()
        {
            if (UglTFStreamSubSystem* StrongThis = WeakThis.Get())
            {
                StrongThis->StopMainWorldStreaming();
            }
        });
        return;
    }

    ClearTimers();
    bActive = false;
    bRenderOnlyStreaming = false;
    ++InitialScanGeneration;
    ActiveInitialPreflightCount = 0;
    ActiveInitialActorScans.Empty();

    for (TPair<FString, TObjectPtr<AglTFStreamActor>>& Pair : SpawnActorMap)
    {
        if (IsValid(Pair.Value))
        {
            Pair.Value->ReleaseRuntimeResourcesForWorldExit();
            Pair.Value->Destroy();
        }
    }
    SpawnActorMap.Empty();

    // Only touch the pawn explicitly captured by this streaming session. StartMainWorldStreaming()
    // begins by resetting stale GameInstance state; resolving through GameManager here could find and
    // destroy the destination world's newly spawned default pawn before world entry completes.
    DeactivatePlayerCharacter();

    if (ACharacterController* Character = ActivePlayerCharacter.Get())
    {
        Character->PrepareForPawnReplacement();
        Character->Destroy();
    }

    GlbFilePaths.Empty();
    PlayerGlbFilePaths.Empty();
    CurrentPlayerPath.Reset();
    PendingPlayerPath.Reset();
    PlayerDirectory.Reset();
    InitialPlayerName.Reset();
    CompletedInitialPaths.Empty();
    MissingFilePaths.Empty();
    ValidatedModelPaths.Empty();
    FailedPlayerPaths.Empty();
    MetadataUnavailablePaths.Empty();
    ModelMetadataMap.Empty();
    InitialPathProgress.Empty();
    LastReportedLoadingStatus = 0.0f;
    LastLoadingProgressFrame = ~uint64(0);
    ActivePlayerCharacter.Reset();
    CurrentPathIndex = 0;
    CurrentPlayerPathIndex = INDEX_NONE;
    PendingPlayerPathIndex = INDEX_NONE;
    bInitialPathScanComplete = false;
    bInitialPlayerLoadComplete = false;
    bInitialPlayerLoadStarted = false;
    bWaitingForPlayerLoad = false;
    bPlayerActivated = false;
    bPendingPlayerIsInitialLoad = false;
    PlayerActorWaitStartedAt = 0.0;
    PlayerLoadStartedAt = 0.0;
    NextModelFileAuditTime = 0.0;

    WriteLogAsync(TEXT("glTFStreamSubSystem stopped. GLB actors destroyed, pending preflights invalidated, and parser caches scheduled for safe release"));
    OwnerActor = nullptr;
    SpawnActorClass = nullptr;
}

bool UglTFStreamSubSystem::AreInitialModelsReady() const
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::AreInitialModelsReady")))
    {
        return false;
    }

    if (!bActive)
    {
        return true;
    }

    if (!bInitialPathScanComplete || CompletedInitialPaths.Num() < GlbFilePaths.Num())
    {
        return false;
    }

    for (const TPair<FString, TObjectPtr<AglTFStreamActor>>& Pair : SpawnActorMap)
    {
        if (IsValid(Pair.Value) && !Pair.Value->GetIsLoaded())
        {
            return false;
        }
    }
    return true;
}

bool UglTFStreamSubSystem::IsPlayerLoaded() const
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::IsPlayerLoaded")))
    {
        return false;
    }

    if (!bActive)
    {
        return true;
    }

    const ACharacterController* Ctrl = GetPlayerCharacter();
    if (!IsValid(Ctrl))
    {
        return bInitialPlayerLoadComplete && !bWaitingForPlayerLoad;
    }

    if (PlayerGlbFilePaths.Num() == 0)
    {
        return bInitialPlayerLoadComplete;
    }

    if (!bInitialPlayerLoadComplete || bWaitingForPlayerLoad)
    {
        return false;
    }

    return Ctrl->bIsLoaded;
}

bool UglTFStreamSubSystem::IsInitialWorldReady()
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::IsInitialWorldReady")))
    {
        return false;
    }

    BeginInitialPlayerStreamingIfNeeded();

    if (!AreInitialModelsReady())
    {
        return false;
    }

    const bool bReady = IsPlayerLoaded();
    if (bReady)
    {
        ActivatePlayerIfWorldReady();
    }
    return bReady;
}

float UglTFStreamSubSystem::GetLoadingStatus() const
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::GetLoadingStatus")))
    {
        return 0.0f;
    }

    if (!bActive)
    {
        // GameInstance subsystems exist before the selected world starts streaming. Returning one here
        // makes a directly bound progress bar display 100 percent during that startup window.
        return FMath::Clamp(LastReportedLoadingStatus, 0.0f, 1.0f);
    }

    double WeightedProgress = 0.0;
    double TotalWeight = 0.0;

    for (const FString& Path : GlbFilePaths)
    {
        float PathProgress = FMath::Clamp(InitialPathProgress.FindRef(Path), 0.0f, 1.0f);

        if (const TObjectPtr<AglTFStreamActor>* ActorPtr = SpawnActorMap.Find(Path))
        {
            if (const AglTFStreamActor* Actor = ActorPtr->Get())
            {
                // Preflight owns the first 10 percent. The actor then reports a node-normalized size
                // scan and stream pass; cached, invalid, no-op, and range-skipped nodes are included.
                PathProgress = FMath::Max(
                    PathProgress,
                    0.10f + FMath::Clamp(Actor->GetLoadingStatus(), 0.0f, 1.0f) * 0.90f);
            }
        }

        if (CompletedInitialPaths.Contains(Path) || MissingFilePaths.Contains(Path))
        {
            PathProgress = 1.0f;
        }

        // Keep each model's global weight stable for the whole bootstrap. Dynamically switching from
        // a one-unit preflight weight to a parsed-node weight can make the mathematical percentage
        // move backwards and leave a monotonic UI bar frozen near 100 percent.
        WeightedProgress += static_cast<double>(PathProgress);
        TotalWeight += 1.0;
    }

    // Player/bootstrap readiness is always a work item, even when no custom character GLB exists.
    // This prevents an empty or cache-only world from displaying 100 percent before its pawn exists.
    float PlayerProgress = 0.0f;
    if (bInitialPlayerLoadComplete && !bWaitingForPlayerLoad)
    {
        PlayerProgress = 1.0f;
    }
    else if (bInitialPathScanComplete && bInitialPlayerLoadStarted)
    {
        PlayerProgress = 0.10f;
        if (bWaitingForPlayerLoad)
        {
            if (const ACharacterController* Ctrl = GetPlayerCharacter())
            {
                PlayerProgress = 0.10f + FMath::Clamp(Ctrl->GetLoadProgress(), 0.0f, 1.0f) * 0.90f;
            }
        }
        else if (IsValid(GetPlayerCharacter()))
        {
            PlayerProgress = 0.25f;
        }
    }

    WeightedProgress += static_cast<double>(FMath::Clamp(PlayerProgress, 0.0f, 1.0f));
    TotalWeight += 1.0;

    float Result = TotalWeight > 0.0
        ? static_cast<float>(WeightedProgress / TotalWeight)
        : 0.0f;

    const bool bReady = AreInitialModelsReady() && IsPlayerLoaded();
    const float TargetProgress = bReady
        ? 1.0f
        : FMath::Min(FMath::Clamp(Result, 0.0f, 1.0f), 0.99f);

    // A cache hit, distance skip, or tiny model can finish between two Slate paints. Limit only the
    // UI-facing value once per game frame; the underlying node work and readiness are not delayed.
    // This preserves node-derived progress while ensuring a skipped path does not appear as 100%
    // from the first frame in which the loading widget becomes visible.
    if (LastLoadingProgressFrame != GFrameCounter)
    {
        constexpr float MaxVisibleAdvancePerFrame = 0.08f;
        if (TargetProgress > LastReportedLoadingStatus)
        {
            LastReportedLoadingStatus = FMath::Min(
                TargetProgress,
                LastReportedLoadingStatus + MaxVisibleAdvancePerFrame);
        }
        LastLoadingProgressFrame = GFrameCounter;
    }
    return FMath::Clamp(LastReportedLoadingStatus, 0.0f, 1.0f);
}

void UglTFStreamSubSystem::ProcessNextPathAsync()
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::ProcessNextPathAsync")))
    {
        return;
    }

    if (!bActive || !IsValid(OwnerActor))
    {
        return;
    }

    // Keep at most three GLBs in the combined preflight/bounds-calculation pipeline. A path whose
    // metadata already proves it is outside the streaming range releases its slot immediately;
    // an in-range path retains the slot until its stream actor finishes loading.
    while (CurrentPathIndex < GlbFilePaths.Num() &&
        ActiveInitialPreflightCount + ActiveInitialActorScans.Num() < MAX_CONCURRENT_INITIAL_MODEL_LOADS)
    {
        const FString GlbPath = GlbFilePaths[CurrentPathIndex++];
        if (CompletedInitialPaths.Contains(GlbPath))
        {
            SetInitialPathProgress(GlbPath, 1.0f);
            continue;
        }

        SetInitialPathProgress(GlbPath, 0.02f);
        ++ActiveInitialPreflightCount;
        StartInitialPathPreflight(GlbPath, InitialScanGeneration);
    }

    ScheduleWaitForInitialActors();
    FinalizeInitialPathScanIfReady();
}

void UglTFStreamSubSystem::StartInitialPathPreflight(
    const FString& GlbPath,
    const int32 ScanGeneration)
{
    check(IsInGameThread());

    TWeakObjectPtr<UglTFStreamSubSystem> WeakThis(this);
    const bool bQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, GlbPath, ScanGeneration]()
        {
            // WORKER THREAD ONLY: model validation, hashing, and SCZ parsing are pure-data work.
            // User-authored JSON never participates in the streaming-bounds cache.
            FInitialModelPreflightResult Result;
            Result.bGlbValid = GlbValidation::ValidateFile(GlbPath, Result.Reason);

            if (Result.bGlbValid)
            {
                const FString SizeCachePath = FPaths::ChangeExtension(GlbPath, TEXT("scz"));
                Result.bSizeCacheExists = IFileManager::Get().FileExists(*SizeCachePath) ||
                    IFileManager::Get().FileExists(*(SizeCachePath + TEXT(".bak")));

                FString ModelHash;
                FString HashError;
                if (!FBinaryDataStore::ComputeFileSha1(GlbPath, ModelHash, HashError))
                {
                    Result.Reason = HashError.IsEmpty()
                        ? TEXT("Model hash could not be calculated")
                        : HashError;
                }
                else if (Result.bSizeCacheExists)
                {
                    FModelCacheData Cache;
                    FString CacheError;
                    bool bHashMismatch = false;
                    if (FBinaryDataStore::LoadModelCache(
                        SizeCachePath,
                        ModelHash,
                        Cache,
                        CacheError,
                        bHashMismatch))
                    {
                        Result.Metadata.Center = Cache.Center;
                        Result.Metadata.Size = Cache.Extent * 2.0;
                        Result.bMetadataValid = IsUsableModelMetadata(Result.Metadata);
                        if (!Result.bMetadataValid)
                        {
                            Result.Reason = TEXT("Model SCZ contains invalid or empty bounds");
                        }
                    }
                    else
                    {
                        Result.Reason = CacheError.IsEmpty()
                            ? TEXT("Model SCZ could not be loaded safely")
                            : CacheError;

                        // A source hash change makes every cached extent stale. Remove all DAT
                        // generations now so the stream actor can rebuild one clean cache.
                        if (bHashMismatch)
                        {
                            FString InvalidateError;
                            if (!FBinaryDataStore::InvalidateCacheFile(SizeCachePath, InvalidateError) &&
                                !InvalidateError.IsEmpty())
                            {
                                Result.Reason += FString::Printf(
                                    TEXT("; stale SCZ cleanup failed: %s"),
                                    *InvalidateError);
                            }
                            Result.bSizeCacheExists = false;
                        }
                    }
                }
                else
                {
                    Result.Reason = TEXT("Model SCZ does not exist");
                }
            }

            FSafeFileIO::DispatchTrackedGameThread(
                [WeakThis, GlbPath, ScanGeneration, Result = MoveTemp(Result)]() mutable
                {
                    UglTFStreamSubSystem* StrongThis = WeakThis.Get();
                    if (!IsValid(StrongThis) ||
                        ScanGeneration != StrongThis->InitialScanGeneration)
                    {
                        return;
                    }

                    check(IsInGameThread());
                    StrongThis->ActiveInitialPreflightCount =
                        FMath::Max(0, StrongThis->ActiveInitialPreflightCount - 1);
                    StrongThis->SetInitialPathProgress(GlbPath, 0.10f);

                    if (!StrongThis->bActive || !IsValid(StrongThis->OwnerActor))
                    {
                        return;
                    }

                    if (!Result.bGlbValid)
                    {
                        StrongThis->ValidatedModelPaths.Remove(GlbPath);
                        StrongThis->MissingFilePaths.Add(GlbPath);
                        StrongThis->CompletedInitialPaths.Add(GlbPath);
                        StrongThis->SetInitialPathProgress(GlbPath, 1.0f);
                        StrongThis->WriteLogAsync(FString::Printf(
                            TEXT("Invalid model GLB skipped. Path=%s Reason=%s"),
                            *GlbPath,
                            *Result.Reason));
                    }
                    else
                    {
                        StrongThis->ValidatedModelPaths.Add(GlbPath);
                        StrongThis->MissingFilePaths.Remove(GlbPath);

                        bool bNeedsActorLoad = true;
                        if (Result.bMetadataValid)
                        {
                            StrongThis->MetadataUnavailablePaths.Remove(GlbPath);
                            StrongThis->ModelMetadataMap.Add(GlbPath, Result.Metadata);
                            StrongThis->WriteLogAsync(FString::Printf(
                                TEXT("Valid model SCZ loaded in parallel. GLB=%s Center=%s Size=%s"),
                                *GlbPath,
                                *Result.Metadata.Center.ToCompactString(),
                                *Result.Metadata.Size.ToCompactString()));

                            if (!StrongThis->IsPlayerInsideModelRange(Result.Metadata))
                            {
                                bNeedsActorLoad = false;
                                StrongThis->DestroySpawnActor(GlbPath);
                                StrongThis->CompletedInitialPaths.Add(GlbPath);
                                StrongThis->SetInitialPathProgress(GlbPath, 1.0f);
                                StrongThis->WriteLogAsync(FString::Printf(
                                    TEXT("Initial GLB load skipped by metadata range: %s"),
                                    *GlbPath));
                            }
                        }
                        else
                        {
                            StrongThis->MetadataUnavailablePaths.Add(GlbPath);
                            StrongThis->WriteLogAsync(FString::Printf(
                                TEXT("Model SCZ unavailable; bounds will be calculated by a parallel stream actor. GLB=%s SczExists=%s Reason=%s"),
                                *GlbPath,
                                Result.bSizeCacheExists ? TEXT("true") : TEXT("false"),
                                *Result.Reason));
                        }

                        if (bNeedsActorLoad)
                        {
                            if (AglTFStreamActor* Actor = StrongThis->EnsureSpawnActor(GlbPath))
                            {
                                StrongThis->ActiveInitialActorScans.Add(
                                    GlbPath,
                                    FPlatformTime::Seconds());

                                // Cache hits or very small files can finish before this continuation
                                // reaches the poll timer. The normal polling function handles both.
                                (void)Actor;
                            }
                            else
                            {
                                StrongThis->MissingFilePaths.Add(GlbPath);
                                StrongThis->CompletedInitialPaths.Add(GlbPath);
                                StrongThis->SetInitialPathProgress(GlbPath, 1.0f);
                                StrongThis->WriteLogAsync(FString::Printf(
                                    TEXT("Initial stream actor could not be created: %s"),
                                    *GlbPath));
                            }
                        }
                    }

                    StrongThis->ScheduleProcessNextPath();
                    StrongThis->ScheduleWaitForInitialActors();
                    StrongThis->FinalizeInitialPathScanIfReady();
                });
        });

    if (!bQueued)
    {
        // Worker queue rejection is observed synchronously on the game thread. Mark this path
        // complete so shutdown or task-pool rejection cannot stall world bootstrap forever.
        ActiveInitialPreflightCount = FMath::Max(0, ActiveInitialPreflightCount - 1);
        MissingFilePaths.Add(GlbPath);
        CompletedInitialPaths.Add(GlbPath);
        SetInitialPathProgress(GlbPath, 1.0f);
        WriteLogAsync(FString::Printf(
            TEXT("Initial model preflight could not be queued: %s"),
            *GlbPath));
    }
}

void UglTFStreamSubSystem::WaitForCurrentActorAsync()
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::WaitForCurrentActorAsync")))
    {
        return;
    }

    if (!bActive || !IsValid(OwnerActor))
    {
        return;
    }

    const double NowSeconds = FPlatformTime::Seconds();
    TArray<FString> Paths;
    ActiveInitialActorScans.GetKeys(Paths);

    for (const FString& GlbPath : Paths)
    {
        const double* StartedAtSeconds = ActiveInitialActorScans.Find(GlbPath);
        if (!StartedAtSeconds)
        {
            continue;
        }

        AglTFStreamActor* Actor = nullptr;
        if (const TObjectPtr<AglTFStreamActor>* ActorPtr = SpawnActorMap.Find(GlbPath))
        {
            Actor = ActorPtr->Get();
        }

        if (!IsValid(Actor))
        {
            MissingFilePaths.Add(GlbPath);
            CompletedInitialPaths.Add(GlbPath);
            SetInitialPathProgress(GlbPath, 1.0f);
            ActiveInitialActorScans.Remove(GlbPath);
            continue;
        }

        SetInitialPathProgress(
            GlbPath,
            0.10f + FMath::Clamp(Actor->GetLoadingStatus(), 0.0f, 1.0f) * 0.90f);

        if (!Actor->GetIsLoaded())
        {
            if (NowSeconds - *StartedAtSeconds >= MODEL_LOAD_TIMEOUT_SECONDS)
            {
                WriteLogAsync(FString::Printf(
                    TEXT("Model GLB load timed out and was isolated: %s"),
                    *GlbPath));
                DestroySpawnActor(GlbPath);
                MissingFilePaths.Add(GlbPath);
                CompletedInitialPaths.Add(GlbPath);
                SetInitialPathProgress(GlbPath, 1.0f);
                ActiveInitialActorScans.Remove(GlbPath);
            }
            continue;
        }

        CacheActorMetadata(GlbPath, Actor);
        if (ModelMetadataMap.Contains(GlbPath))
        {
            MetadataUnavailablePaths.Remove(GlbPath);
        }
        if (const FModelData* Metadata = ModelMetadataMap.Find(GlbPath))
        {
            if (!IsPlayerInsideModelRange(*Metadata))
            {
                DestroySpawnActor(GlbPath);
            }
        }

        CompletedInitialPaths.Add(GlbPath);
        SetInitialPathProgress(GlbPath, 1.0f);
        ActiveInitialActorScans.Remove(GlbPath);
    }

    ScheduleProcessNextPath();
    ScheduleWaitForInitialActors();
    FinalizeInitialPathScanIfReady();
}

void UglTFStreamSubSystem::UpdateStreamingAsync()
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::UpdateStreamingAsync")))
    {
        return;
    }

    if (!bActive || !IsValid(OwnerActor))
    {
        return;
    }

    constexpr double ModelFileAuditIntervalSeconds = 5.0;
    const double Now = FPlatformTime::Seconds();
    const bool bAuditModelFiles = Now >= NextModelFileAuditTime;
    if (bAuditModelFiles)
    {
        NextModelFileAuditTime = Now + ModelFileAuditIntervalSeconds;
    }

    for (const FString& GlbPath : GlbFilePaths)
    {
        const bool bKnownMissing = MissingFilePaths.Contains(GlbPath);
        const bool bNeedsInitialValidation =
            !ValidatedModelPaths.Contains(GlbPath) && !bKnownMissing;

        // Range decisions remain responsive, while stable paths avoid one filesystem syscall per
        // GLB every 250 ms. Missing/replaced files are retried by the low-frequency audit.
        if (bAuditModelFiles || bNeedsInitialValidation)
        {
            if (!IFileManager::Get().FileExists(*GlbPath))
            {
                ValidatedModelPaths.Remove(GlbPath);
                if (!bKnownMissing)
                {
                    MissingFilePaths.Add(GlbPath);
                    WriteLogAsync(FString::Printf(TEXT("Model GLB disappeared during streaming and was isolated. Path=%s"), *GlbPath));
                }
                DestroySpawnActor(GlbPath);
                continue;
            }

            if (!ValidatedModelPaths.Contains(GlbPath))
            {
                FString ValidationReason;
                if (!GlbValidation::ValidateFile(GlbPath, ValidationReason))
                {
                    if (!MissingFilePaths.Contains(GlbPath))
                    {
                        MissingFilePaths.Add(GlbPath);
                        WriteLogAsync(FString::Printf(TEXT("Invalid model GLB isolated during streaming. Path=%s Reason=%s"), *GlbPath, *ValidationReason));
                    }
                    DestroySpawnActor(GlbPath);
                    continue;
                }
                ValidatedModelPaths.Add(GlbPath);
                MissingFilePaths.Remove(GlbPath);
            }
        }

        if (MissingFilePaths.Contains(GlbPath) || !ValidatedModelPaths.Contains(GlbPath))
        {
            continue;
        }

        if (const FModelData* CachedMetadata = ModelMetadataMap.Find(GlbPath))
        {
            const TObjectPtr<AglTFStreamActor>* ActorPtr = SpawnActorMap.Find(GlbPath);
            const bool bActorLoaded = ActorPtr && IsValid(ActorPtr->Get());
            const float RadiusMultiplier = bActorLoaded ? 1.10f : 1.0f;
            if (IsPlayerInsideModelRange(*CachedMetadata, RadiusMultiplier))
            {
                EnsureSpawnActor(GlbPath);
            }
            else
            {
                DestroySpawnActor(GlbPath);
            }
            continue;
        }

        AglTFStreamActor* ExistingActor = nullptr;
        if (const TObjectPtr<AglTFStreamActor>* ActorPtr = SpawnActorMap.Find(GlbPath))
        {
            ExistingActor = ActorPtr->Get();
        }
        if (IsValid(ExistingActor) && ExistingActor->GetIsLoaded())
        {
            CacheActorMetadata(GlbPath, ExistingActor);
            if (const FModelData* GeneratedMetadata = ModelMetadataMap.Find(GlbPath))
            {
                MetadataUnavailablePaths.Remove(GlbPath);
                if (!IsPlayerInsideModelRange(*GeneratedMetadata, 1.10f))
                {
                    DestroySpawnActor(GlbPath);
                }
            }
            continue;
        }

        // Metadata was already checked during the initial scan. Do not reparse the same JSON every update.
        EnsureSpawnActor(GlbPath);
    }

    ScheduleUpdateStreaming();
}


void UglTFStreamSubSystem::DiscoverPlayerPaths()
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::DiscoverPlayerPaths")))
    {
        return;
    }

    PlayerGlbFilePaths.Empty();
    if (!PlayerDirectory.IsEmpty())
    {
        // This scans file names only. Character GLB contents are not parsed or loaded here.
        PlayerGlbFilePaths = UFileFunctionLibrary::GetFileNamesWithExtension(PlayerDirectory, TEXT("glb"));
        NormalizeAndDeduplicatePaths(PlayerGlbFilePaths);
    }

    WriteLogAsync(FString::Printf(
        TEXT("Player GLB path scan completed without loading character assets. Directory=%s Count=%d"),
        *PlayerDirectory,
        PlayerGlbFilePaths.Num()));
}

int32 UglTFStreamSubSystem::FindNextLoadablePlayerIndex(int32 StartIndex) const
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::FindNextLoadablePlayerIndex")))
    {
        return INDEX_NONE;
    }

    const int32 Count = PlayerGlbFilePaths.Num();
    if (Count <= 0)
    {
        return INDEX_NONE;
    }

    const int32 SafeStart = StartIndex == INDEX_NONE ? 0 : ((StartIndex % Count) + Count) % Count;
    for (int32 Offset = 0; Offset < Count; ++Offset)
    {
        const int32 CandidateIndex = (SafeStart + Offset) % Count;
        const FString& CandidatePath = PlayerGlbFilePaths[CandidateIndex];
        if (!CandidatePath.IsEmpty() && !IsPlayerPathQuarantined(CandidatePath))
        {
            return CandidateIndex;
        }
    }
    return INDEX_NONE;
}

bool UglTFStreamSubSystem::IsPlayerPathQuarantined(const FString& PlayerPath) const
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::IsPlayerPathQuarantined")))
    {
        return true;
    }

    const FFailedPlayerFileState* FailedState = FailedPlayerPaths.Find(PlayerPath);
    if (!FailedState)
    {
        return false;
    }

    // A file replaced in-place is a new load candidate. Keep quarantining only the exact bytes
    // (identified cheaply by size + timestamp) that previously failed this session.
    return FailedState->FileSize == IFileManager::Get().FileSize(*PlayerPath) &&
        FailedState->Timestamp == IFileManager::Get().GetTimeStamp(*PlayerPath);
}

void UglTFStreamSubSystem::QuarantinePlayerPath(const FString& PlayerPath)
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::QuarantinePlayerPath")))
    {
        return;
    }

    if (PlayerPath.IsEmpty())
    {
        return;
    }

    FFailedPlayerFileState& FailedState = FailedPlayerPaths.FindOrAdd(PlayerPath);
    FailedState.FileSize = IFileManager::Get().FileSize(*PlayerPath);
    FailedState.Timestamp = IFileManager::Get().GetTimeStamp(*PlayerPath);
}

bool UglTFStreamSubSystem::ResolveInitialPlayerIndex()
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::ResolveInitialPlayerIndex")))
    {
        return false;
    }

    PendingPlayerPathIndex = INDEX_NONE;

    if (PlayerGlbFilePaths.Num() == 0)
    {
        return false;
    }

    // Empty and legacy "Player" values mean that no external character has been selected yet.
    // Choose the first deterministic, loadable GLB so a newly imported character works on first run.
    const FString TrimmedInitialName = InitialPlayerName.TrimStartAndEnd();
    if (TrimmedInitialName.IsEmpty() ||
        TrimmedInitialName.Equals(TEXT("Player"), ESearchCase::IgnoreCase))
    {
        PendingPlayerPathIndex = FindNextLoadablePlayerIndex(0);
        if (PendingPlayerPathIndex != INDEX_NONE)
        {
            WriteLogAsync(FString::Printf(
                TEXT("No explicit external character was selected; using the first available GLB: %s"),
                *PlayerGlbFilePaths[PendingPlayerPathIndex]));
            return true;
        }
        return false;
    }

    const FString WantedName = FPaths::GetCleanFilename(InitialPlayerName);
    const FString WantedBaseName = FPaths::GetBaseFilename(InitialPlayerName);
    int32 PreferredIndex = INDEX_NONE;

    for (int32 Index = 0; Index < PlayerGlbFilePaths.Num(); ++Index)
    {
        const FString& CandidatePath = PlayerGlbFilePaths[Index];
        const FString CandidateName = FPaths::GetCleanFilename(CandidatePath);
        const FString CandidateBaseName = FPaths::GetBaseFilename(CandidatePath);
        if (CandidatePath.Equals(InitialPlayerName, ESearchCase::IgnoreCase) ||
            CandidateName.Equals(WantedName, ESearchCase::IgnoreCase) ||
            CandidateBaseName.Equals(WantedBaseName, ESearchCase::IgnoreCase))
        {
            PreferredIndex = Index;
            break;
        }
    }

    if (PreferredIndex == INDEX_NONE)
    {
        const FString CombinedPath = GlbValidation::NormalizePath(
            FPaths::Combine(PlayerDirectory, InitialPlayerName));
        if (IFileManager::Get().FileExists(*CombinedPath))
        {
            PlayerGlbFilePaths.AddUnique(CombinedPath);
            PlayerGlbFilePaths.Sort([](const FString& A, const FString& B)
            {
                return A.Compare(B, ESearchCase::IgnoreCase) < 0;
            });
            PreferredIndex = PlayerGlbFilePaths.IndexOfByKey(CombinedPath);
        }
    }

    if (!PlayerGlbFilePaths.IsValidIndex(PreferredIndex))
    {
        WriteLogAsync(FString::Printf(
            TEXT("Initial player GLB was not found; keeping the default character. Requested=%s"),
            *InitialPlayerName));
        return false;
    }

    if (IsPlayerPathQuarantined(PlayerGlbFilePaths[PreferredIndex]))
    {
        WriteLogAsync(FString::Printf(
            TEXT("Initial player GLB is quarantined for this session; keeping the default character. Path=%s"),
            *PlayerGlbFilePaths[PreferredIndex]));
        return false;
    }

    PendingPlayerPathIndex = PreferredIndex;
    return true;
}

void UglTFStreamSubSystem::BeginInitialPlayerStreamingIfNeeded()
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::BeginInitialPlayerStreamingIfNeeded")))
    {
        return;
    }

    if (!bActive || bInitialPlayerLoadStarted || !bInitialPathScanComplete)
    {
        return;
    }

    bInitialPlayerLoadStarted = true;
    WriteLogAsync(TEXT("Initial character selection begins after world-model scanning; only the selected character is loaded"));
    StartPlayerStreaming();
}

void UglTFStreamSubSystem::StartPlayerStreaming()
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::StartPlayerStreaming")))
    {
        return;
    }

    if (!bActive)
    {
        return;
    }

    if (!ResolveInitialPlayerIndex())
    {
        CompletePlayerStreamingWithExistingCharacter(
            TEXT("No explicit valid initial character was selected"));
        return;
    }

    PlayerActorWaitStartedAt = FPlatformTime::Seconds();
    ScheduleWaitForPlayerActor();
}

void UglTFStreamSubSystem::WaitForPlayerActorAsync()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("WaitForPlayerActorAsync must run on the game thread")))
    {
        return;
    }

    if (!bActive || !IsValid(OwnerActor))
    {
        return;
    }

    ACharacterController* Ctrl = GetPlayerCharacter();
    if (!IsValid(Ctrl))
    {
        if (FPlatformTime::Seconds() - PlayerActorWaitStartedAt >= PLAYER_ACTOR_WAIT_TIMEOUT_SECONDS)
        {
            CompletePlayerStreamingWithExistingCharacter(
                TEXT("Timed out waiting for the default player pawn"));
            return;
        }
        ScheduleWaitForPlayerActor();
        return;
    }

    ActivePlayerCharacter = Ctrl;
    RequestLoadPlayerAtIndex(PendingPlayerPathIndex, true);
}

void UglTFStreamSubSystem::RequestLoadPlayerAtIndex(int32 PlayerPathIndex, bool bIsInitialLoad)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("RequestLoadPlayerAtIndex must run on the game thread")))
    {
        return;
    }

    if (!bActive || PlayerGlbFilePaths.Num() == 0 || bWaitingForPlayerLoad)
    {
        return;
    }

    if (!PlayerGlbFilePaths.IsValidIndex(PlayerPathIndex))
    {
        if (bIsInitialLoad)
        {
            CompletePlayerStreamingWithExistingCharacter(
                FString::Printf(TEXT("Invalid initial player path index %d"), PlayerPathIndex));
        }
        return;
    }

    ACharacterController* Ctrl = GetPlayerCharacter();
    if (!IsValid(Ctrl))
    {
        if (bIsInitialLoad)
        {
            CompletePlayerStreamingWithExistingCharacter(
                TEXT("The gameplay pawn was unavailable for character loading"));
        }
        return;
    }

    PendingPlayerPathIndex = PlayerPathIndex;
    PendingPlayerPath = PlayerGlbFilePaths[PlayerPathIndex];
    bPendingPlayerIsInitialLoad = bIsInitialLoad;
    ActivePlayerCharacter = Ctrl;

    if (bIsInitialLoad)
    {
        bInitialPlayerLoadComplete = false;
        bPlayerActivated = false;
        Ctrl->Activate(false);
    }

    bWaitingForPlayerLoad = true;
    PlayerLoadStartedAt = FPlatformTime::Seconds();
    Ctrl->bIsLoaded = false;

    // ACharacterController releases the previous generated mesh first, keeps the directly
    // assigned default mesh active, and starts one background validation/parser/mesh request.
    Ctrl->Load(PendingPlayerPath);

    WriteLogAsync(FString::Printf(
        TEXT("On-demand character load requested on the existing pawn. Initial=%s Index=%d Path=%s"),
        bIsInitialLoad ? TEXT("true") : TEXT("false"),
        PendingPlayerPathIndex,
        *PendingPlayerPath));
    ScheduleWaitForPlayerLoad();
}

void UglTFStreamSubSystem::HandlePlayerLoadFailure(const FString& Reason)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("HandlePlayerLoadFailure must run on the game thread")))
    {
        return;
    }

    const FString FailedPath = PendingPlayerPath;
    const bool bWasInitialLoad = bPendingPlayerIsInitialLoad;
    if (!FailedPath.IsEmpty())
    {
        QuarantinePlayerPath(FailedPath);
    }

    if (ACharacterController* Ctrl = GetPlayerCharacter())
    {
        Ctrl->CancelCharacterLoad(true);
        Ctrl->bIsLoaded = true;
        ActivePlayerCharacter = Ctrl;
    }

    WriteLogAsync(FString::Printf(
        TEXT("Character GLB quarantined; default character remains active. Path=%s Reason=%s"),
        *FailedPath,
        *Reason));

    PendingPlayerPath.Reset();
    PendingPlayerPathIndex = INDEX_NONE;
    bPendingPlayerIsInitialLoad = false;
    bWaitingForPlayerLoad = false;
    PlayerLoadStartedAt = 0.0;

    // Never iterate through and load every character while entering a world. A failed selected
    // character falls back immediately; other GLBs are loaded only when the player requests them.
    if (bWasInitialLoad)
    {
        CompletePlayerStreamingWithExistingCharacter(
            TEXT("The selected character failed; other character GLBs were not preloaded"));
    }
    else
    {
        bInitialPlayerLoadComplete = true;
        ActivatePlayerIfWorldReady();
    }
}

void UglTFStreamSubSystem::CompletePlayerStreamingWithExistingCharacter(const FString& Reason)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("CompletePlayerStreamingWithExistingCharacter must run on the game thread")))
    {
        return;
    }

    ACharacterController* ExistingCharacter = GetPlayerCharacter();
    if (IsValid(ExistingCharacter))
    {
        ActivePlayerCharacter = ExistingCharacter;
        ExistingCharacter->bIsLoaded = true;
        ExistingCharacter->SetActorHiddenInGame(false);
        ExistingCharacter->SetActorEnableCollision(true);

        if (UGameManagerSubSystem* GameSystem = UGameManagerSubSystem::GetSubSystem(OwnerActor))
        {
            GameSystem->SetPlayerActor(ExistingCharacter);
            GameSystem->SetCameraComponent(ExistingCharacter->GetFollowCameraComponent());
            GameSystem->SetPlayerLocation(ExistingCharacter->GetActorLocation(), ExistingCharacter);
        }
    }

    PendingPlayerPath.Reset();
    PendingPlayerPathIndex = INDEX_NONE;
    bPendingPlayerIsInitialLoad = false;
    bWaitingForPlayerLoad = false;
    bInitialPlayerLoadComplete = true;
    PlayerActorWaitStartedAt = 0.0;
    PlayerLoadStartedAt = 0.0;

    WriteLogAsync(FString::Printf(
        TEXT("Player streaming completed with the existing/default character. Reason=%s"),
        *Reason));
    ActivatePlayerIfWorldReady();
}

void UglTFStreamSubSystem::WaitForPlayerLoadAsync()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("WaitForPlayerLoadAsync must run on the game thread")))
    {
        return;
    }

    if (!bActive || !IsValid(OwnerActor))
    {
        return;
    }

    ACharacterController* Ctrl = GetPlayerCharacter();
    if (!IsValid(Ctrl))
    {
        HandlePlayerLoadFailure(TEXT("Gameplay pawn was destroyed before character load completion"));
        return;
    }

    if (FPlatformTime::Seconds() - PlayerLoadStartedAt >= PLAYER_LOAD_TIMEOUT_SECONDS)
    {
        HandlePlayerLoadFailure(TEXT("Character mesh load timed out"));
        return;
    }

    if (!Ctrl->bIsLoaded)
    {
        ScheduleWaitForPlayerLoad();
        return;
    }

    if (!Ctrl->WasLastMeshLoadSuccessful())
    {
        HandlePlayerLoadFailure(TEXT("Character loader completed with the default-mesh fallback"));
        return;
    }

    CurrentPlayerPathIndex = PendingPlayerPathIndex;
    CurrentPlayerPath = PendingPlayerPath;
    PendingPlayerPath.Reset();
    PendingPlayerPathIndex = INDEX_NONE;
    bPendingPlayerIsInitialLoad = false;
    bWaitingForPlayerLoad = false;
    bInitialPlayerLoadComplete = true;
    PlayerLoadStartedAt = 0.0;

    PersistCurrentPlayerSelection();
    WriteLogAsync(FString::Printf(
        TEXT("On-demand character load completed. Index=%d Path=%s"),
        CurrentPlayerPathIndex,
        *CurrentPlayerPath));
    ActivatePlayerIfWorldReady();
}

bool UglTFStreamSubSystem::CycleNextPlayerCharacter()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("CycleNextPlayerCharacter must run on the game thread")))
    {
        return false;
    }

    if (!bActive || !bInitialPlayerLoadComplete || bWaitingForPlayerLoad)
    {
        return false;
    }

    // Always refresh so characters copied into the folder after world load become available.
    DiscoverPlayerPaths();
    CurrentPlayerPathIndex = CurrentPlayerPath.IsEmpty()
        ? INDEX_NONE
        : PlayerGlbFilePaths.IndexOfByKey(CurrentPlayerPath);

    const int32 BaseIndex = PlayerGlbFilePaths.IsValidIndex(CurrentPlayerPathIndex)
        ? CurrentPlayerPathIndex + 1
        : 0;
    const int32 NextIndex = FindNextLoadablePlayerIndex(BaseIndex);
    if (NextIndex == INDEX_NONE || NextIndex == CurrentPlayerPathIndex)
    {
        WriteLogAsync(TEXT("CycleNextPlayerCharacter skipped: no additional valid player GLB is available"));
        return false;
    }

    RequestLoadPlayerAtIndex(NextIndex, false);
    return bWaitingForPlayerLoad;
}

ACharacterController* UglTFStreamSubSystem::GetPlayerCharacter() const
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::GetPlayerCharacter")))
    {
        return nullptr;
    }

    if (IsValid(ActivePlayerCharacter.Get()))
    {
        return ActivePlayerCharacter.Get();
    }

    if (UGameManagerSubSystem* GameSystem = UGameManagerSubSystem::GetSubSystem(OwnerActor))
    {
        if (ACharacterController* Character = GameSystem->GetPlayerActor<ACharacterController>())
        {
            return Character;
        }
    }
    return nullptr;
}

void UglTFStreamSubSystem::DeactivatePlayerCharacter()
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::DeactivatePlayerCharacter")))
    {
        return;
    }

    ACharacterController* Ctrl = ActivePlayerCharacter.Get();
    if (!IsValid(Ctrl))
    {
        return;
    }

    Ctrl->Activate(false);
    Ctrl->bIsLoaded = false;

    if (USkeletalMeshComponent* MeshComponent = Ctrl->GetMesh())
    {
        // Never clear the skinned asset to nullptr while an AnimBP/ControlRig may still own
        // worker tasks. PrepareForPawnReplacement performs the final GT-only default-mesh swap.
        MeshComponent->SetAllBodiesSimulatePhysics(false);
        MeshComponent->SetSimulatePhysics(false);
        MeshComponent->PutAllRigidBodiesToSleep();
        MeshComponent->bPauseAnims = false;
        MeshComponent->SetComponentTickEnabled(true);
    }
}

void UglTFStreamSubSystem::ActivatePlayerIfWorldReady()
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::ActivatePlayerIfWorldReady")))
    {
        return;
    }

    if (bPlayerActivated || !AreInitialModelsReady())
    {
        return;
    }

    ACharacterController* Ctrl = GetPlayerCharacter();
    if (!IsValid(Ctrl) || bWaitingForPlayerLoad)
    {
        return;
    }

    if (!Ctrl->bIsLoaded)
    {
        return;
    }

    Ctrl->Activate(true);
    bPlayerActivated = true;
    WriteLogAsync(FString::Printf(
        TEXT("Player character activated. Path=%s"),
        *CurrentPlayerPath));
}

void UglTFStreamSubSystem::PersistCurrentPlayerSelection()
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::PersistCurrentPlayerSelection")))
    {
        return;
    }

    if (!IsValid(OwnerActor) || CurrentPlayerPath.IsEmpty())
    {
        return;
    }

    if (UGameManagerSubSystem* GameSystem = UGameManagerSubSystem::GetSubSystem(OwnerActor))
    {
        const FString SelectedPlayer = FPaths::GetCleanFilename(CurrentPlayerPath);
        GameSystem->SetSelectedPlayerForRuntime(SelectedPlayer);
        WriteLogAsync(FString::Printf(
            TEXT("Current player selection saved to data/world.dat. Player=%s"),
            *SelectedPlayer));
    }
}

bool UglTFStreamSubSystem::IsValidModelMetadata(const FModelData& ModelData) const
{
    // This helper is pure data, but keeping it on the subsystem's owning thread prevents callers
    // from accidentally pairing it with unsynchronized reads of ModelMetadataMap.
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::IsValidModelMetadata")))
    {
        return false;
    }

    return IsUsableModelMetadata(ModelData);
}

bool UglTFStreamSubSystem::IsPlayerInsideModelRange(
    const FModelData& ModelData,
    const float RadiusMultiplier) const
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::IsPlayerInsideModelRange")))
    {
        return false;
    }

    if (!IsValidModelMetadata(ModelData))
    {
        return true;
    }

    const FVector PlayerLocation = GetPlayerLocation();
    const float Radius = FMath::Max3(ModelData.Size.X, ModelData.Size.Y, ModelData.Size.Z) * GLTF_STREAM_DISTANCE_SCALE;
    const float SafeRadius =
        FMath::Max(1.0f, Radius) * FMath::Clamp(RadiusMultiplier, 1.0f, 2.0f);
    return FVector::DistSquared(PlayerLocation, ModelData.Center) <= FMath::Square(SafeRadius);
}

FVector UglTFStreamSubSystem::GetPlayerLocation() const
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::GetPlayerLocation")))
    {
        return FVector::ZeroVector;
    }

    if (UGameManagerSubSystem* GameSystem = UGameManagerSubSystem::GetSubSystem(OwnerActor))
    {
        return GameSystem->GetPlayerLocation();
    }
    return FVector::ZeroVector;
}

AglTFStreamActor* UglTFStreamSubSystem::EnsureSpawnActor(const FString& GlbPath)
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::EnsureSpawnActor")))
    {
        return nullptr;
    }

    if (const TObjectPtr<AglTFStreamActor>* ActorPtr = SpawnActorMap.Find(GlbPath))
    {
        if (IsValid(ActorPtr->Get()))
        {
            return ActorPtr->Get();
        }
    }

    if (!ValidatedModelPaths.Contains(GlbPath))
    {
        FString ValidationReason;
        if (!GlbValidation::ValidateFile(GlbPath, ValidationReason))
        {
            ValidatedModelPaths.Remove(GlbPath);
            MissingFilePaths.Add(GlbPath);
            WriteLogAsync(FString::Printf(TEXT("SpawnActor skipped for invalid GLB. Path=%s Reason=%s"), *GlbPath, *ValidationReason));
            return nullptr;
        }
        ValidatedModelPaths.Add(GlbPath);
        MissingFilePaths.Remove(GlbPath);
    }

    if (!IsValid(OwnerActor) || !SpawnActorClass)
    {
        return nullptr;
    }

    UWorld* World = OwnerActor->GetWorld();
    if (!World)
    {
        return nullptr;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = OwnerActor;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::Undefined;

    AglTFStreamActor* NewActor = FActorHelper::SpawnActorDeferred<AglTFStreamActor>(
        World,
        SpawnActorClass,
        OwnerActor->GetActorTransform(),
        SpawnParams);

    if (IsValid(NewActor))
    {
        NewActor->SetRenderOnlyStreaming(bRenderOnlyStreaming);
        NewActor->Init(GlbPath);
        NewActor->FinishSpawning(OwnerActor->GetActorTransform());
        SpawnActorMap.Add(GlbPath, NewActor);
        SetInitialPathProgress(GlbPath, 0.10f);
        WriteLogAsync(FString::Printf(TEXT("SpawnActor created for GLB: %s"), *GlbPath));
    }

    return NewActor;
}

void UglTFStreamSubSystem::DestroySpawnActor(const FString& GlbPath)
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::DestroySpawnActor")))
    {
        return;
    }

    TObjectPtr<AglTFStreamActor>* ActorPtr = SpawnActorMap.Find(GlbPath);
    if (!ActorPtr)
    {
        return;
    }

    if (IsValid(ActorPtr->Get()))
    {
        ActorPtr->Get()->ReleaseRuntimeResourcesForWorldExit();
        ActorPtr->Get()->Destroy();
        WriteLogAsync(FString::Printf(TEXT("SpawnActor destroyed because player is outside model range: %s"), *GlbPath));
    }
    SpawnActorMap.Remove(GlbPath);
}

void UglTFStreamSubSystem::CacheActorMetadata(const FString& GlbPath, const AglTFStreamActor* Actor)
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::CacheActorMetadata")))
    {
        return;
    }

    if (!Actor || !Actor->HasModelMetadata())
    {
        return;
    }

    const FModelData Metadata = Actor->GetModelMetadata();
    if (IsValidModelMetadata(Metadata))
    {
        ModelMetadataMap.Add(GlbPath, Metadata);
        WriteLogAsync(FString::Printf(TEXT("Actor metadata cached. GLB=%s Center=%s Size=%s"),
            *GlbPath,
            *Metadata.Center.ToCompactString(),
            *Metadata.Size.ToCompactString()));
    }
}

void UglTFStreamSubSystem::SetInitialPathProgress(const FString& GlbPath, float ProgressValue)
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::SetInitialPathProgress")) || GlbPath.IsEmpty())
    {
        return;
    }

    float& StoredProgress = InitialPathProgress.FindOrAdd(GlbPath);
    const float SafeProgress = FMath::IsFinite(ProgressValue)
        ? FMath::Clamp(ProgressValue, 0.0f, 1.0f)
        : StoredProgress;
    StoredProgress = FMath::Max(StoredProgress, SafeProgress);
}

void UglTFStreamSubSystem::ScheduleProcessNextPath()
{
    check(IsInGameThread());
    if (!bActive || !IsValid(OwnerActor))
    {
        return;
    }

    if (UWorld* World = OwnerActor->GetWorld())
    {
        // Multiple worker completions can arrive in one frame. Collapse them into one next-tick
        // scheduler pass instead of stacking duplicate timer delegates.
        World->GetTimerManager().ClearTimer(TimerHandle_ProcessPath);
        TimerHandle_ProcessPath = World->GetTimerManager().SetTimerForNextTick(
            FTimerDelegate::CreateUObject(this, &UglTFStreamSubSystem::ProcessNextPathAsync));
    }
}

void UglTFStreamSubSystem::ScheduleWaitForInitialActors()
{
    check(IsInGameThread());
    if (!bActive || !IsValid(OwnerActor) || ActiveInitialActorScans.IsEmpty())
    {
        return;
    }

    if (UWorld* World = OwnerActor->GetWorld())
    {
        // One timer polls every active initial actor. Reusing a single handle avoids one timer per
        // GLB and keeps all map iteration on the game thread.
        World->GetTimerManager().SetTimer(
            TimerHandle_WaitActor,
            FTimerDelegate::CreateUObject(this, &UglTFStreamSubSystem::WaitForCurrentActorAsync),
            STREAM_POLL_INTERVAL_SECONDS,
            false);
    }
}

void UglTFStreamSubSystem::FinalizeInitialPathScanIfReady()
{
    check(IsInGameThread());
    if (!bActive || bInitialPathScanComplete ||
        CurrentPathIndex < GlbFilePaths.Num() ||
        ActiveInitialPreflightCount > 0 ||
        !ActiveInitialActorScans.IsEmpty() ||
        CompletedInitialPaths.Num() < GlbFilePaths.Num())
    {
        return;
    }

    bInitialPathScanComplete = true;
    for (const FString& GlbPath : GlbFilePaths)
    {
        SetInitialPathProgress(GlbPath, 1.0f);
    }
    WriteLogAsync(FString::Printf(
        TEXT("Initial GLB scan completed with bounded parallelism. Paths=%d"),
        GlbFilePaths.Num()));
    BeginInitialPlayerStreamingIfNeeded();
    ScheduleUpdateStreaming();
}

void UglTFStreamSubSystem::ScheduleUpdateStreaming()
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::ScheduleUpdateStreaming")))
    {
        return;
    }

    if (!bActive || !IsValid(OwnerActor))
    {
        return;
    }

    if (UWorld* World = OwnerActor->GetWorld())
    {
        World->GetTimerManager().SetTimer(
            TimerHandle_UpdateStreaming,
            FTimerDelegate::CreateUObject(this, &UglTFStreamSubSystem::UpdateStreamingAsync),
            0.25f,
            false);
    }
}

void UglTFStreamSubSystem::ScheduleWaitForPlayerActor()
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::ScheduleWaitForPlayerActor")))
    {
        return;
    }

    if (!bActive || !IsValid(OwnerActor))
    {
        return;
    }

    if (UWorld* World = OwnerActor->GetWorld())
    {
        World->GetTimerManager().SetTimer(
            TimerHandle_WaitPlayer,
            FTimerDelegate::CreateUObject(this, &UglTFStreamSubSystem::WaitForPlayerActorAsync),
            STREAM_POLL_INTERVAL_SECONDS,
            false);
    }
}

void UglTFStreamSubSystem::ScheduleWaitForPlayerLoad()
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::ScheduleWaitForPlayerLoad")))
    {
        return;
    }

    if (!bActive || !IsValid(OwnerActor))
    {
        return;
    }

    if (UWorld* World = OwnerActor->GetWorld())
    {
        World->GetTimerManager().SetTimer(
            TimerHandle_WaitPlayer,
            FTimerDelegate::CreateUObject(this, &UglTFStreamSubSystem::WaitForPlayerLoadAsync),
            STREAM_POLL_INTERVAL_SECONDS,
            false);
    }
}

void UglTFStreamSubSystem::ClearTimers()
{
    if (!EnsureStreamSubsystemGameThread(TEXT("UglTFStreamSubSystem::ClearTimers")))
    {
        return;
    }

    UWorld* World = IsValid(OwnerActor) ? OwnerActor->GetWorld() : GetWorld();
    if (World)
    {
        World->GetTimerManager().ClearTimer(TimerHandle_ProcessPath);
        World->GetTimerManager().ClearTimer(TimerHandle_WaitActor);
        World->GetTimerManager().ClearTimer(TimerHandle_UpdateStreaming);
        World->GetTimerManager().ClearTimer(TimerHandle_WaitPlayer);
        World->GetTimerManager().ClearAllTimersForObject(this);
    }
}

void UglTFStreamSubSystem::WriteLogAsync(const FString& Message) const
{
    UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("glTFStreamSubSystem"), Message);
}
