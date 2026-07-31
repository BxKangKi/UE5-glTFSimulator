// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Model/glTFStreamSubSystem.h"

#include "Character/CharacterController.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "System/AssetManageSubSystem.h"
#include "Model/glTFStreamActor.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "System/ActorHelper.h"
#include "System/FileFunctionLibrary.h"
#include "System/GameManagerSubSystem.h"
#include "System/GlbValidation.h"
#include "System/MacroLibrary.h"
#include "TimerManager.h"
#include "World/WorldData.h"

namespace
{
    constexpr float GLTF_STREAM_DISTANCE_SCALE = 64.0f;
    constexpr float STREAM_POLL_INTERVAL_SECONDS = 0.10f;
    constexpr double PLAYER_ACTOR_WAIT_TIMEOUT_SECONDS = 30.0;
    constexpr double PLAYER_LOAD_TIMEOUT_SECONDS = 120.0;
    constexpr double MODEL_LOAD_TIMEOUT_SECONDS = 180.0;
    constexpr int64 MAX_MODEL_METADATA_JSON_BYTES = 64ll * 1024ll * 1024ll;

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
    StopMainWorldStreaming();
    Super::Deinitialize();
}

void UglTFStreamSubSystem::StartMainWorldStreaming(AActor* InOwnerActor, TSubclassOf<AglTFStreamActor> InSpawnActorClass, const FString& InModelDirectory, const FString& InPlayerDirectory, const FString& InInitialPlayerName, bool bInRenderOnlyStreaming)
{
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
    ModelWaitStartedAt = 0.0;
    NextModelFileAuditTime = 0.0;
    CurrentPathIndex = 0;
    CurrentPlayerPathIndex = INDEX_NONE;
    PendingPlayerPathIndex = INDEX_NONE;
    CurrentPlayerPath.Reset();
    PendingPlayerPath.Reset();
    WaitingPath.Reset();
    ActivePlayerCharacter.Reset();
    CompletedInitialPaths.Empty();
    MissingFilePaths.Empty();
    ValidatedModelPaths.Empty();
    FailedPlayerPaths.Empty();
    MetadataUnavailablePaths.Empty();
    ModelMetadataMap.Empty();
    SpawnActorMap.Empty();

    GlbFilePaths = UFileFunctionLibrary::GetFileNamesWithExtension(ModelDirectory, TEXT("glb"));
    NormalizeAndDeduplicatePaths(GlbFilePaths);
    DiscoverPlayerPaths();

    if (UAssetManageSubSystem* AssetManager = UAssetManageSubSystem::Get(InOwnerActor))
    {
        AssetManager->ActivateForMainWorld(InOwnerActor);
    }

    WriteLogAsync(FString::Printf(TEXT("glTFStreamSubSystem started. ModelDirectory=%s GLBCount=%d PlayerDirectory=%s PlayerCount=%d InitialPlayer=%s RenderOnly=%s"),
        *ModelDirectory,
        GlbFilePaths.Num(),
        *PlayerDirectory,
        PlayerGlbFilePaths.Num(),
        *InitialPlayerName,
        bRenderOnlyStreaming ? TEXT("true") : TEXT("false")));

    // Load external world models first. Serializing the character build avoids overlapping
    // glTF parser, skeletal-mesh, physics-asset, texture, and render-resource creation peaks.
    ScheduleProcessNextPath();
}

void UglTFStreamSubSystem::StopMainWorldStreaming()
{
    ClearTimers();
    bActive = false;
    bRenderOnlyStreaming = false;

    for (TPair<FString, TObjectPtr<AglTFStreamActor>>& Pair : SpawnActorMap)
    {
        if (IsValid(Pair.Value))
        {
            Pair.Value->ReleaseRuntimeResourcesForWorldExit();
            Pair.Value->Destroy();
        }
    }
    SpawnActorMap.Empty();

    DeactivatePlayerCharacter();

    // Character streaming now owns only one gameplay pawn. Cancel its in-flight loader,
    // detach the current runtime mesh, and destroy the pawn once during world shutdown.
    if (ACharacterController* Character = GetPlayerCharacter())
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
    WaitingPath.Reset();
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
    ModelWaitStartedAt = 0.0;
    NextModelFileAuditTime = 0.0;

    UAssetManageSubSystem* AssetManager = UAssetManageSubSystem::Get(OwnerActor);
    if (!AssetManager)
    {
        if (UGameInstance* GameInstance = GetGameInstance())
        {
            AssetManager = GameInstance->GetSubsystem<UAssetManageSubSystem>();
        }
    }
    if (AssetManager)
    {
        AssetManager->DeactivateAndRelease();
    }

    WriteLogAsync(TEXT("glTFStreamSubSystem stopped. GLB actors destroyed, player streaming state cleared, and runtime assets released"));
    OwnerActor = nullptr;
    SpawnActorClass = nullptr;
}

bool UglTFStreamSubSystem::AreInitialModelsReady() const
{
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
    if (!bActive)
    {
        return 1.0f;
    }

    float Total = 0.0f;
    int32 WorkItemCount = 0;

    for (const FString& Path : GlbFilePaths)
    {
        ++WorkItemCount;
        if (const TObjectPtr<AglTFStreamActor>* ActorPtr = SpawnActorMap.Find(Path))
        {
            Total += IsValid(ActorPtr->Get()) ? ActorPtr->Get()->GetLoadingStatus() : 1.0f;
        }
        else if (CompletedInitialPaths.Contains(Path) || MissingFilePaths.Contains(Path))
        {
            Total += 1.0f;
        }
    }

    const bool bHasPlayerLoadWork = PlayerGlbFilePaths.Num() > 0;
    if (bHasPlayerLoadWork)
    {
        ++WorkItemCount;
        float PlayerLoadProgress = IsPlayerLoaded() ? 1.0f : 0.0f;
        if (PlayerLoadProgress < 1.0f)
        {
            const ACharacterController* Ctrl = GetPlayerCharacter();
            if (IsValid(Ctrl))
            {
                PlayerLoadProgress = Ctrl->GetLoadProgress();
            }
        }
        Total += FMath::Clamp(PlayerLoadProgress, 0.0f, 1.0f);
    }

    if (WorkItemCount <= 0)
    {
        return 1.0f;
    }

    return FMath::Clamp(Total / static_cast<float>(WorkItemCount), 0.0f, 1.0f);
}

void UglTFStreamSubSystem::ProcessNextPathAsync()
{
    if (!bActive || !IsValid(OwnerActor))
    {
        return;
    }

    if (CurrentPathIndex >= GlbFilePaths.Num())
    {
        bInitialPathScanComplete = true;
        WriteLogAsync(TEXT("Initial GLB path scan completed"));
        BeginInitialPlayerStreamingIfNeeded();
        ScheduleUpdateStreaming();
        return;
    }

    const FString GlbPath = GlbFilePaths[CurrentPathIndex++];
    FString ValidationReason;
    if (!GlbValidation::ValidateFile(GlbPath, ValidationReason))
    {
        ValidatedModelPaths.Remove(GlbPath);
        MissingFilePaths.Add(GlbPath);
        CompletedInitialPaths.Add(GlbPath);
        WriteLogAsync(FString::Printf(TEXT("Invalid model GLB skipped. Path=%s Reason=%s"), *GlbPath, *ValidationReason));
        ScheduleProcessNextPath();
        return;
    }
    ValidatedModelPaths.Add(GlbPath);
    MissingFilePaths.Remove(GlbPath);

    bool bJsonExists = false;
    FModelData Metadata;
    if (TryLoadValidModelMetadata(GlbPath, Metadata, bJsonExists))
    {
        ModelMetadataMap.Add(GlbPath, Metadata);
        WriteLogAsync(FString::Printf(TEXT("Valid model metadata loaded. GLB=%s Center=%s Size=%s"),
            *GlbPath,
            *Metadata.Center.ToCompactString(),
            *Metadata.Size.ToCompactString()));

        if (IsPlayerInsideModelRange(Metadata))
        {
            EnsureSpawnActor(GlbPath);
        }
        else
        {
            WriteLogAsync(FString::Printf(TEXT("Streaming GLB load skipped by metadata range. GLB=%s Center=%s Size=%s"),
                *GlbPath,
                *Metadata.Center.ToCompactString(),
                *Metadata.Size.ToCompactString()));
            DestroySpawnActor(GlbPath);
        }

        CompletedInitialPaths.Add(GlbPath);
        ScheduleProcessNextPath();
        return;
    }

    MetadataUnavailablePaths.Add(GlbPath);
    WriteLogAsync(FString::Printf(TEXT("Model metadata missing, damaged, oversized, or still default. GLB=%s JsonExists=%s"),
        *GlbPath,
        bJsonExists ? TEXT("true") : TEXT("false")));

    WaitingPath = GlbPath;
    ModelWaitStartedAt = FPlatformTime::Seconds();
    EnsureSpawnActor(GlbPath);

    if (UWorld* World = OwnerActor->GetWorld())
    {
        World->GetTimerManager().SetTimerForNextTick(this, &UglTFStreamSubSystem::WaitForCurrentActorAsync);
    }
}

void UglTFStreamSubSystem::WaitForCurrentActorAsync()
{
    if (!bActive || !IsValid(OwnerActor) || WaitingPath.IsEmpty())
    {
        return;
    }

    AglTFStreamActor* Actor = nullptr;
    if (const TObjectPtr<AglTFStreamActor>* ActorPtr = SpawnActorMap.Find(WaitingPath))
    {
        Actor = ActorPtr->Get();
    }

    if (!IsValid(Actor))
    {
        CompletedInitialPaths.Add(WaitingPath);
        WaitingPath.Reset();
        ScheduleProcessNextPath();
        return;
    }

    if (!Actor->GetIsLoaded())
    {
        if (FPlatformTime::Seconds() - ModelWaitStartedAt >= MODEL_LOAD_TIMEOUT_SECONDS)
        {
            const FString TimedOutPath = WaitingPath;
            WriteLogAsync(FString::Printf(TEXT("Model GLB load timed out and was isolated: %s"), *TimedOutPath));
            DestroySpawnActor(TimedOutPath);
            MissingFilePaths.Add(TimedOutPath);
            CompletedInitialPaths.Add(TimedOutPath);
            WaitingPath.Reset();
            ModelWaitStartedAt = 0.0;
            ScheduleProcessNextPath();
            return;
        }

        if (UWorld* World = OwnerActor->GetWorld())
        {
            World->GetTimerManager().SetTimer(
                TimerHandle_WaitActor,
                FTimerDelegate::CreateUObject(this, &UglTFStreamSubSystem::WaitForCurrentActorAsync),
                STREAM_POLL_INTERVAL_SECONDS,
                false);
        }
        return;
    }

    CacheActorMetadata(WaitingPath, Actor);
    if (ModelMetadataMap.Contains(WaitingPath))
    {
        MetadataUnavailablePaths.Remove(WaitingPath);
    }
    if (const FModelData* Metadata = ModelMetadataMap.Find(WaitingPath))
    {
        if (!IsPlayerInsideModelRange(*Metadata))
        {
            DestroySpawnActor(WaitingPath);
        }
    }

    CompletedInitialPaths.Add(WaitingPath);
    WaitingPath.Reset();
    ModelWaitStartedAt = 0.0;
    ScheduleProcessNextPath();
}

void UglTFStreamSubSystem::UpdateStreamingAsync()
{
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
    ACharacterController* Ctrl = GetPlayerCharacter();
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
    if (!IsValid(OwnerActor) || CurrentPlayerPath.IsEmpty())
    {
        return;
    }

    UGameManagerSubSystem* GameSystem = UGameManagerSubSystem::GetSubSystem(OwnerActor);
    if (!GameSystem)
    {
        return;
    }

    UWorldData* WorldData = GameSystem->GetWorldData();
    if (!IsValid(WorldData))
    {
        return;
    }

    WorldData->Player = FPaths::GetCleanFilename(CurrentPlayerPath);

    const FString WorldName = GameSystem->GetCurrentWorldName();
    if (!WorldName.IsEmpty())
    {
        FString LevelJsonPath = FPaths::Combine(PATH_ROOT, WorldName);
        LevelJsonPath.Append(LEVEL_FILE_NAME);
        UFileFunctionLibrary::ToJsonAsync(UWorldData::SerializeData(WorldData), LevelJsonPath);
        WriteLogAsync(FString::Printf(
            TEXT("Current player selection saved to level.json. Player=%s Path=%s"),
            *WorldData->Player,
            *LevelJsonPath));
    }
}

bool UglTFStreamSubSystem::TryLoadValidModelMetadata(const FString& GlbPath, FModelData& OutModelData, bool& bOutJsonExists) const
{
    bOutJsonExists = false;
    OutModelData = FModelData();

    const FString JsonPath = FPaths::ChangeExtension(GlbPath, TEXT("json"));
    if (!FPaths::FileExists(JsonPath))
    {
        return false;
    }

    bOutJsonExists = true;
    const int64 JsonFileSize = IFileManager::Get().FileSize(*JsonPath);
    if (JsonFileSize < 0 || JsonFileSize > MAX_MODEL_METADATA_JSON_BYTES)
    {
        return false;
    }

    FString JsonString;
    if (!FFileHelper::LoadFileToString(JsonString, *JsonPath))
    {
        return false;
    }

    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
    {
        return false;
    }

    OutModelData.Deserialization(JsonObject);
    return IsValidModelMetadata(OutModelData);
}

bool UglTFStreamSubSystem::IsValidModelMetadata(const FModelData& ModelData) const
{
    return !ModelData.Size.IsNearlyZero(0.001f);
}

bool UglTFStreamSubSystem::IsPlayerInsideModelRange(
    const FModelData& ModelData,
    const float RadiusMultiplier) const
{
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
    if (UGameManagerSubSystem* GameSystem = UGameManagerSubSystem::GetSubSystem(OwnerActor))
    {
        return GameSystem->GetPlayerLocation();
    }
    return FVector::ZeroVector;
}

AglTFStreamActor* UglTFStreamSubSystem::EnsureSpawnActor(const FString& GlbPath)
{
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
        WriteLogAsync(FString::Printf(TEXT("SpawnActor created for GLB: %s"), *GlbPath));
    }

    return NewActor;
}

void UglTFStreamSubSystem::DestroySpawnActor(const FString& GlbPath)
{
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

void UglTFStreamSubSystem::ScheduleProcessNextPath()
{
    if (!bActive || !IsValid(OwnerActor))
    {
        return;
    }

    if (UWorld* World = OwnerActor->GetWorld())
    {
        TimerHandle_ProcessPath = World->GetTimerManager().SetTimerForNextTick(
            FTimerDelegate::CreateUObject(this, &UglTFStreamSubSystem::ProcessNextPathAsync));
    }
}

void UglTFStreamSubSystem::ScheduleUpdateStreaming()
{
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
