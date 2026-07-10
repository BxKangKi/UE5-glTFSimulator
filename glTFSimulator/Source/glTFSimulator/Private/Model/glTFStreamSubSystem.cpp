// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Model/glTFStreamSubSystem.h"

#include "Character/CharacterController.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "System/AssetManageSubSystem.h"
#include "Model/glTFStreamActor.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "System/ActorHelper.h"
#include "System/FileFunctionLibrary.h"
#include "System/GameManagerSubSystem.h"
#include "System/MacroLibrary.h"
#include "TimerManager.h"
#include "World/WorldData.h"

namespace
{
    constexpr float GLTF_STREAM_DISTANCE_SCALE = 64.0f;
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
    CurrentPathIndex = 0;
    CurrentPlayerPathIndex = INDEX_NONE;
    CurrentPlayerPath.Reset();
    WaitingPath.Reset();
    ActivePlayerCharacter.Reset();
    PendingPlayerCharacter.Reset();
    PreviousPlayerCharacter.Reset();
    CompletedInitialPaths.Empty();
    MissingFilePaths.Empty();
    ModelMetadataMap.Empty();
    SpawnActorMap.Empty();

    GlbFilePaths = UFileFunctionLibrary::GetFileNamesWithExtension(ModelDirectory, TEXT("glb"));
    GlbFilePaths.Sort();
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

    BeginInitialPlayerStreamingIfNeeded();
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

    if (ACharacterController* PendingCharacter = PendingPlayerCharacter.Get())
    {
        if (IsValid(PendingCharacter))
        {
            PendingCharacter->PrepareForPawnReplacement();
            PendingCharacter->Destroy();
        }
    }

    DestroyPreviousPlayerCharacter();

    if (ACharacterController* ActiveCharacter = ActivePlayerCharacter.Get())
    {
        if (IsValid(ActiveCharacter))
        {
            ActiveCharacter->PrepareForPawnReplacement();
            ActiveCharacter->Destroy();
        }
    }

    GlbFilePaths.Empty();
    PlayerGlbFilePaths.Empty();
    CurrentPlayerPath.Reset();
    PlayerDirectory.Reset();
    InitialPlayerName.Reset();
    CompletedInitialPaths.Empty();
    MissingFilePaths.Empty();
    ModelMetadataMap.Empty();
    WaitingPath.Reset();
    ActivePlayerCharacter.Reset();
    PendingPlayerCharacter.Reset();
    PreviousPlayerCharacter.Reset();
    CurrentPathIndex = 0;
    CurrentPlayerPathIndex = INDEX_NONE;
    bInitialPathScanComplete = false;
    bInitialPlayerLoadComplete = false;
    bInitialPlayerLoadStarted = false;
    bWaitingForPlayerLoad = false;
    bPlayerActivated = false;

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
        return false;
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
        ScheduleUpdateStreaming();
        return;
    }

    const FString GlbPath = GlbFilePaths[CurrentPathIndex++];
    if (!IFileManager::Get().FileExists(*GlbPath))
    {
        MissingFilePaths.Add(GlbPath);
        CompletedInitialPaths.Add(GlbPath);
        WriteLogAsync(FString::Printf(TEXT("GLB file missing. Skipped: %s"), *GlbPath));
        ScheduleProcessNextPath();
        return;
    }

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

    WriteLogAsync(FString::Printf(TEXT("Model metadata missing, damaged, or still default. GLB=%s JsonExists=%s"),
        *GlbPath,
        bJsonExists ? TEXT("true") : TEXT("false")));

    WaitingPath = GlbPath;
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
        if (UWorld* World = OwnerActor->GetWorld())
        {
            TimerHandle_WaitActor = World->GetTimerManager().SetTimerForNextTick(
                FTimerDelegate::CreateUObject(this, &UglTFStreamSubSystem::WaitForCurrentActorAsync));
        }
        return;
    }

    CacheActorMetadata(WaitingPath, Actor);
    if (const FModelData* Metadata = ModelMetadataMap.Find(WaitingPath))
    {
        if (!IsPlayerInsideModelRange(*Metadata))
        {
            DestroySpawnActor(WaitingPath);
        }
    }

    CompletedInitialPaths.Add(WaitingPath);
    WaitingPath.Reset();
    ScheduleProcessNextPath();
}

void UglTFStreamSubSystem::UpdateStreamingAsync()
{
    if (!bActive || !IsValid(OwnerActor))
    {
        return;
    }

    for (const FString& GlbPath : GlbFilePaths)
    {
        if (!IFileManager::Get().FileExists(*GlbPath))
        {
            if (!MissingFilePaths.Contains(GlbPath))
            {
                MissingFilePaths.Add(GlbPath);
                WriteLogAsync(FString::Printf(TEXT("GLB file missing during streaming update. Skipped: %s"), *GlbPath));
            }
            DestroySpawnActor(GlbPath);
            continue;
        }

        FModelData Metadata;
        bool bJsonExists = false;
        if (TryLoadValidModelMetadata(GlbPath, Metadata, bJsonExists))
        {
            ModelMetadataMap.Add(GlbPath, Metadata);
            WriteLogAsync(FString::Printf(TEXT("Streaming metadata refreshed. GLB=%s Center=%s Size=%s"),
                *GlbPath,
                *Metadata.Center.ToCompactString(),
                *Metadata.Size.ToCompactString()));

            if (IsPlayerInsideModelRange(Metadata))
            {
                EnsureSpawnActor(GlbPath);
            }
            else
            {
                WriteLogAsync(FString::Printf(TEXT("Streaming GLB load skipped during streaming update by metadata range. GLB=%s Center=%s Size=%s"),
                    *GlbPath,
                    *Metadata.Center.ToCompactString(),
                    *Metadata.Size.ToCompactString()));
                DestroySpawnActor(GlbPath);
            }
        }
        else if (!SpawnActorMap.Contains(GlbPath))
        {
            EnsureSpawnActor(GlbPath);
        }
    }

    ScheduleUpdateStreaming();
}


void UglTFStreamSubSystem::DiscoverPlayerPaths()
{
    PlayerGlbFilePaths.Empty();

    if (!PlayerDirectory.IsEmpty())
    {
        PlayerGlbFilePaths = UFileFunctionLibrary::GetFileNamesWithExtension(PlayerDirectory, TEXT("glb"));
        PlayerGlbFilePaths.Sort();
    }

    WriteLogAsync(FString::Printf(TEXT("Player GLB path scan completed. Directory=%s Count=%d"), *PlayerDirectory, PlayerGlbFilePaths.Num()));
}

bool UglTFStreamSubSystem::ResolveInitialPlayerIndex()
{
    if (PlayerGlbFilePaths.Num() == 0)
    {
        CurrentPlayerPathIndex = INDEX_NONE;
        return false;
    }

    const FString WantedName = FPaths::GetCleanFilename(InitialPlayerName);
    const FString WantedBaseName = FPaths::GetBaseFilename(InitialPlayerName);

    if (!InitialPlayerName.IsEmpty())
    {
        for (int32 Index = 0; Index < PlayerGlbFilePaths.Num(); ++Index)
        {
            const FString& CandidatePath = PlayerGlbFilePaths[Index];
            const FString CandidateName = FPaths::GetCleanFilename(CandidatePath);
            const FString CandidateBaseName = FPaths::GetBaseFilename(CandidatePath);

            if (CandidatePath == InitialPlayerName || CandidateName == WantedName || CandidateBaseName == WantedBaseName)
            {
                CurrentPlayerPathIndex = Index;
                return true;
            }
        }

        const FString CombinedPath = FPaths::Combine(PlayerDirectory, InitialPlayerName);
        if (IFileManager::Get().FileExists(*CombinedPath))
        {
            PlayerGlbFilePaths.AddUnique(CombinedPath);
            PlayerGlbFilePaths.Sort();
            CurrentPlayerPathIndex = PlayerGlbFilePaths.IndexOfByKey(CombinedPath);
            return CurrentPlayerPathIndex != INDEX_NONE;
        }

        WriteLogAsync(FString::Printf(TEXT("Initial player GLB was not found in player list. Requested=%s"), *InitialPlayerName));
    }

    CurrentPlayerPathIndex = 0;
    return true;
}

void UglTFStreamSubSystem::BeginInitialPlayerStreamingIfNeeded()
{
    if (!bActive || bInitialPlayerLoadStarted)
    {
        return;
    }

    bInitialPlayerLoadStarted = true;
    WriteLogAsync(TEXT("Initial player streaming starts in parallel with initial world GLB loading"));
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
        bInitialPlayerLoadComplete = true;
        bWaitingForPlayerLoad = false;
        WriteLogAsync(TEXT("No player GLB was found. Player streaming is marked complete with existing/default character."));
        ActivatePlayerIfWorldReady();
        return;
    }

    ScheduleWaitForPlayerActor();
}

void UglTFStreamSubSystem::WaitForPlayerActorAsync()
{
    if (!bActive || !IsValid(OwnerActor))
    {
        return;
    }

    ACharacterController* Ctrl = GetPlayerCharacter();
    if (!IsValid(Ctrl))
    {
        ScheduleWaitForPlayerActor();
        return;
    }

    RequestLoadPlayerAtIndex(CurrentPlayerPathIndex, true);
}

void UglTFStreamSubSystem::RequestLoadPlayerAtIndex(int32 PlayerPathIndex, bool bIsInitialLoad)
{
    if (!bActive || PlayerGlbFilePaths.Num() == 0)
    {
        return;
    }

    if (!PlayerGlbFilePaths.IsValidIndex(PlayerPathIndex))
    {
        WriteLogAsync(FString::Printf(TEXT("Player load skipped. Invalid player index=%d Count=%d"), PlayerPathIndex, PlayerGlbFilePaths.Num()));
        return;
    }

    const FString PlayerPath = PlayerGlbFilePaths[PlayerPathIndex];
    if (!IFileManager::Get().FileExists(*PlayerPath))
    {
        WriteLogAsync(FString::Printf(TEXT("Player GLB file missing. Skipped: %s"), *PlayerPath));
        if (bIsInitialLoad)
        {
            bInitialPlayerLoadComplete = true;
            bWaitingForPlayerLoad = false;
            ActivatePlayerIfWorldReady();
        }
        return;
    }

    ACharacterController* Ctrl = SpawnReplacementPlayerCharacterForLoad(PlayerPath);
    if (!IsValid(Ctrl))
    {
        CurrentPlayerPathIndex = PlayerPathIndex;
        ScheduleWaitForPlayerActor();
        return;
    }

    CurrentPlayerPathIndex = PlayerPathIndex;
    CurrentPlayerPath = PlayerPath;
    bInitialPlayerLoadComplete = false;
    bWaitingForPlayerLoad = true;
    bPlayerActivated = false;

    Ctrl->Activate(false);
    Ctrl->bIsLoaded = false;
    Ctrl->Load(PlayerPath);

    WriteLogAsync(FString::Printf(TEXT("Player character respawn/load requested. Index=%d Path=%s"), CurrentPlayerPathIndex, *CurrentPlayerPath));
    ScheduleWaitForPlayerLoad();
}

ACharacterController* UglTFStreamSubSystem::SpawnReplacementPlayerCharacterForLoad(const FString& PlayerPath)
{
    if (!IsValid(OwnerActor))
    {
        return nullptr;
    }

    UWorld* World = OwnerActor->GetWorld();
    if (!World)
    {
        return nullptr;
    }

    APlayerController* PlayerController = UGameplayStatics::GetPlayerController(OwnerActor, 0);
    ACharacterController* CurrentCharacter = GetPlayerCharacter();
    if (!IsValid(CurrentCharacter) && PlayerController)
    {
        CurrentCharacter = Cast<ACharacterController>(PlayerController->GetPawn());
    }

    if (IsValid(PendingPlayerCharacter.Get()))
    {
        PendingPlayerCharacter->PrepareForPawnReplacement();
        PendingPlayerCharacter->Destroy();
        PendingPlayerCharacter.Reset();
    }

    TSubclassOf<ACharacterController> CharacterClass = IsValid(CurrentCharacter) ? CurrentCharacter->GetClass() : ACharacterController::StaticClass();
    if (!CharacterClass)
    {
        WriteLogAsync(FString::Printf(TEXT("Player respawn failed: no valid CharacterClass for %s"), *PlayerPath));
        return nullptr;
    }

    const FVector SpawnLocation = IsValid(CurrentCharacter) ? CurrentCharacter->GetActorLocation() : GetPlayerLocation();
    const FRotator SpawnRotation = IsValid(CurrentCharacter) ? CurrentCharacter->GetActorRotation() : FRotator::ZeroRotator;
    FTransform SpawnTransform(SpawnRotation, SpawnLocation);

    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = OwnerActor;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    ACharacterController* NewCharacter = World->SpawnActor<ACharacterController>(CharacterClass, SpawnTransform, SpawnParams);
    if (!IsValid(NewCharacter))
    {
        WriteLogAsync(FString::Printf(TEXT("Player respawn failed: SpawnActor returned null. Path=%s"), *PlayerPath));
        return nullptr;
    }

    NewCharacter->SetActorHiddenInGame(true);
    NewCharacter->SetActorEnableCollision(false);
    NewCharacter->Activate(false);
    NewCharacter->bIsLoaded = false;
    NewCharacter->PrepareForMeshReload();

    PreviousPlayerCharacter = CurrentCharacter;
    PendingPlayerCharacter = NewCharacter;

    if (UGameManagerSubSystem* GameSystem = UGameManagerSubSystem::GetSubSystem(OwnerActor))
    {
        GameSystem->SetPlayerActor(NewCharacter);
        GameSystem->SetPlayerLocation(SpawnLocation);
    }

    WriteLogAsync(FString::Printf(TEXT("Replacement player pawn spawned for runtime GLB load. Old=%s New=%s Path=%s"),
        IsValid(CurrentCharacter) ? *CurrentCharacter->GetName() : TEXT("None"),
        *NewCharacter->GetName(),
        *PlayerPath));

    return NewCharacter;
}

bool UglTFStreamSubSystem::CommitPendingPlayerCharacter()
{
    ACharacterController* NewCharacter = PendingPlayerCharacter.Get();
    if (!IsValid(NewCharacter))
    {
        NewCharacter = GetPlayerCharacter();
    }

    if (!IsValid(NewCharacter))
    {
        return false;
    }

    APlayerController* PlayerController = UGameplayStatics::GetPlayerController(OwnerActor, 0);
    if (PlayerController && PlayerController->GetPawn() != NewCharacter)
    {
        PlayerController->Possess(NewCharacter);
    }

    NewCharacter->SetActorHiddenInGame(false);
    NewCharacter->SetActorEnableCollision(true);
    NewCharacter->Activate(false);

    if (UGameManagerSubSystem* GameSystem = UGameManagerSubSystem::GetSubSystem(OwnerActor))
    {
        GameSystem->SetPlayerActor(NewCharacter);
        GameSystem->SetPlayerLocation(NewCharacter->GetActorLocation());
    }

    ActivePlayerCharacter = NewCharacter;
    PendingPlayerCharacter.Reset();
    DestroyPreviousPlayerCharacter();
    return true;
}

void UglTFStreamSubSystem::DestroyPreviousPlayerCharacter()
{
    ACharacterController* OldCharacter = PreviousPlayerCharacter.Get();
    ACharacterController* ActiveCharacter = ActivePlayerCharacter.Get();

    if (IsValid(OldCharacter) && OldCharacter != ActiveCharacter)
    {
        OldCharacter->PrepareForPawnReplacement();
        OldCharacter->Destroy();
    }

    PreviousPlayerCharacter.Reset();
}

void UglTFStreamSubSystem::WaitForPlayerLoadAsync()
{
    if (!bActive || !IsValid(OwnerActor))
    {
        return;
    }

    ACharacterController* Ctrl = PendingPlayerCharacter.Get();
    if (!IsValid(Ctrl))
    {
        Ctrl = GetPlayerCharacter();
    }

    if (!IsValid(Ctrl))
    {
        ScheduleWaitForPlayerActor();
        return;
    }

    if (!Ctrl->bIsLoaded)
    {
        ScheduleWaitForPlayerLoad();
        return;
    }

    if (!CommitPendingPlayerCharacter())
    {
        ScheduleWaitForPlayerActor();
        return;
    }

    bWaitingForPlayerLoad = false;
    bInitialPlayerLoadComplete = true;

    PersistCurrentPlayerSelection();

    WriteLogAsync(FString::Printf(TEXT("Player character load completed. Index=%d Path=%s"), CurrentPlayerPathIndex, *CurrentPlayerPath));
    ActivatePlayerIfWorldReady();
}

bool UglTFStreamSubSystem::CycleNextPlayerCharacter()
{
    if (!bActive)
    {
        WriteLogAsync(TEXT("CycleNextPlayerCharacter skipped: glTFStreamSubSystem is not active"));
        return false;
    }

    if (!bInitialPlayerLoadComplete || bWaitingForPlayerLoad)
    {
        WriteLogAsync(TEXT("CycleNextPlayerCharacter skipped: player character loading is not ready"));
        return false;
    }

    if (PlayerGlbFilePaths.Num() == 0)
    {
        DiscoverPlayerPaths();
    }

    if (PlayerGlbFilePaths.Num() == 0)
    {
        WriteLogAsync(TEXT("CycleNextPlayerCharacter skipped: there are no player GLB files"));
        return false;
    }

    const int32 BaseIndex = PlayerGlbFilePaths.IsValidIndex(CurrentPlayerPathIndex) ? CurrentPlayerPathIndex : 0;
    const int32 NextIndex = (BaseIndex + 1) % PlayerGlbFilePaths.Num();
    RequestLoadPlayerAtIndex(NextIndex, false);
    return true;
}

ACharacterController* UglTFStreamSubSystem::GetPlayerCharacter() const
{
    if (IsValid(PendingPlayerCharacter.Get()))
    {
        return PendingPlayerCharacter.Get();
    }

    if (IsValid(ActivePlayerCharacter.Get()))
    {
        return ActivePlayerCharacter.Get();
    }

    if (UGameManagerSubSystem* GameSystem = UGameManagerSubSystem::GetSubSystem(OwnerActor))
    {
        return GameSystem->GetPlayerActor<ACharacterController>();
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
        // Never clear the player skeletal mesh to nullptr during world exit or character
        // streaming. AnimBP/ControlRig can still be evaluating on worker threads and may
        // crash if the mesh suddenly has no bone container. Generated character memory is
        // released by swapping to the next freshly generated glTFRuntime skeletal mesh or
        // by destroying the owning actor/world, not by installing an empty mesh.
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

    if (PlayerGlbFilePaths.Num() == 0)
    {
        Ctrl->bIsLoaded = true;
    }

    if (!Ctrl->bIsLoaded)
    {
        return;
    }

    Ctrl->Activate(true);
    bPlayerActivated = true;
    WriteLogAsync(FString::Printf(TEXT("Player character activated. Path=%s"), *CurrentPlayerPath));
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
        WriteLogAsync(FString::Printf(TEXT("Current player selection saved to level.json. Player=%s Path=%s"), *WorldData->Player, *LevelJsonPath));
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

bool UglTFStreamSubSystem::IsPlayerInsideModelRange(const FModelData& ModelData) const
{
    if (!IsValidModelMetadata(ModelData))
    {
        return true;
    }

    const FVector PlayerLocation = GetPlayerLocation();
    const float Radius = FMath::Max3(ModelData.Size.X, ModelData.Size.Y, ModelData.Size.Z) * GLTF_STREAM_DISTANCE_SCALE;
    const float SafeRadius = FMath::Max(1.0f, Radius);
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
        TimerHandle_WaitPlayer = World->GetTimerManager().SetTimerForNextTick(
            FTimerDelegate::CreateUObject(this, &UglTFStreamSubSystem::WaitForPlayerActorAsync));
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
        TimerHandle_WaitPlayer = World->GetTimerManager().SetTimerForNextTick(
            FTimerDelegate::CreateUObject(this, &UglTFStreamSubSystem::WaitForPlayerLoadAsync));
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
