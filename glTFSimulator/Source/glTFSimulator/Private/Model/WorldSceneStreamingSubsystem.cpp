// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldSceneStreamingSubsystem.cpp
 * 역할: 거리 기반으로 씬과 플레이어 모델을 스트리밍합니다.
 * 핵심 기능: 씬 액터 클래스 선택·생성, 초기 준비 상태, 플레이어 모델 교체.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "Model/WorldSceneStreamingSubsystem.h"

#include "Character/CharacterController.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "CoreGlobals.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Model/StaticActor.h"
#include "Simulator/ModelDatabaseSubsystem.h"
#include "System/ActorHelper.h"
#include "System/FileFunctionLibrary.h"
#include "System/GameManagerSubSystem.h"
#include "Setting/GameSettings.h"
#include "System/SafeFileIO.h"
#include "Weather/WeatherSubsystem.h"

namespace
{
    constexpr float SceneDistanceScale = 64.0f;
    constexpr float StreamPollIntervalSeconds = 0.10f;
    constexpr float StreamUpdateIntervalSeconds = 0.25f;
    constexpr double PlayerActorWaitTimeoutSeconds = 30.0;
    constexpr double PlayerLoadTimeoutSeconds = 120.0;

    bool EnsureStreamGameThread(const TCHAR* Context)
    {
        return ensureMsgf(IsInGameThread(), TEXT("%s must run on the game thread"), Context);
    }

    bool IsFiniteVector(const FVector& Value)
    {
        return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
    }

    FString StableCharacterId(const FWorldCharacterStreamRecord& Record)
    {
        return Record.UUID.ToString(EGuidFormats::DigitsWithHyphensLower);
    }
}

UWorldSceneStreamingSubsystem* UWorldSceneStreamingSubsystem::Get(UObject* WorldContextObject)
{
    if (!EnsureStreamGameThread(TEXT("UWorldSceneStreamingSubsystem::Get")) || !IsValid(WorldContextObject))
    {
        return nullptr;
    }

    UWorld* World = WorldContextObject->GetWorld();
    UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
    return IsValid(GameInstance) ? GameInstance->GetSubsystem<UWorldSceneStreamingSubsystem>() : nullptr;
}

void UWorldSceneStreamingSubsystem::Deinitialize()
{
    if (EnsureStreamGameThread(TEXT("UWorldSceneStreamingSubsystem::Deinitialize")))
    {
        StopWorldStreaming();
    }
    Super::Deinitialize();
}

void UWorldSceneStreamingSubsystem::SetRenderOnlyStreaming(const bool bInRenderOnlyStreaming)
{
    if (!EnsureStreamGameThread(TEXT("UWorldSceneStreamingSubsystem::SetRenderOnlyStreaming")))
    {
        return;
    }

    bRenderOnlyStreaming = bInRenderOnlyStreaming;
    for (const TPair<FString, TObjectPtr<AStaticActor>>& Pair : ActiveSceneActors)
    {
        if (IsValid(Pair.Value))
        {
            Pair.Value->SetRenderOnlyStreaming(bRenderOnlyStreaming);
        }
    }
}

bool UWorldSceneStreamingSubsystem::IsRenderOnlyStreaming() const
{
    return EnsureStreamGameThread(TEXT("UWorldSceneStreamingSubsystem::IsRenderOnlyStreaming"))
        && bRenderOnlyStreaming;
}

bool UWorldSceneStreamingSubsystem::IsActiveForWorld(const UWorld* World) const
{
    return EnsureStreamGameThread(TEXT("UWorldSceneStreamingSubsystem::IsActiveForWorld"))
        && bActive
        && World
        && IsValid(OwnerActor)
        && OwnerActor->GetWorld() == World;
}

bool UWorldSceneStreamingSubsystem::HasStartupFailed() const
{
    return EnsureStreamGameThread(TEXT("UWorldSceneStreamingSubsystem::HasStartupFailed"))
        && bStartupFailed;
}

void UWorldSceneStreamingSubsystem::StartWorldStreaming(
    AActor* InOwnerActor,
    const FString& InWorldRoot,
    const FString& InInitialPlayerName,
    const bool bInRenderOnlyStreaming)
{
    if (!IsInGameThread())
    {
        TWeakObjectPtr<UWorldSceneStreamingSubsystem> WeakThis(this);
        TWeakObjectPtr<AActor> WeakOwner(InOwnerActor);
        FSafeFileIO::DispatchTrackedGameThread(
            [WeakThis, WeakOwner, InWorldRoot, InInitialPlayerName,
                bInRenderOnlyStreaming]()
            {
                if (UWorldSceneStreamingSubsystem* StrongThis = WeakThis.Get())
                {
                    StrongThis->StartWorldStreaming(
                        WeakOwner.Get(), InWorldRoot,
                        InInitialPlayerName, bInRenderOnlyStreaming);
                }
            });
        return;
    }

    StopWorldStreaming();
    WorldRoot = FSafeFileIO::NormalizeFilePath(InWorldRoot);
    InitialPlayerName = InInitialPlayerName.TrimStartAndEnd();
    bRenderOnlyStreaming = bInRenderOnlyStreaming;

    if (!IsValid(InOwnerActor) || WorldRoot.IsEmpty())
    {
        bStartupFailed = true;
        WriteLogAsync(TEXT("Streaming startup rejected: owner or world root is invalid"));
        return;
    }

    UGameInstance* GameInstance = InOwnerActor->GetGameInstance();
    UModelDatabaseSubsystem* Database = IsValid(GameInstance)
        ? GameInstance->GetSubsystem<UModelDatabaseSubsystem>() : nullptr;
    if (!IsValid(Database) || !Database->IsReady() || !Database->IsBuiltWorld()
        || Database->GetWorldRoot() != WorldRoot)
    {
        // Runtime streaming has exactly one legal input. Falling back to the resources tree here would
        // violate the archive/source separation and could silently ship an unbounded file loader.
        bStartupFailed = true;
        WriteLogAsync(TEXT("Streaming startup rejected: a verified .gwd archive is not open"));
        return;
    }

    OwnerActor = InOwnerActor;
    bActive = true;

    TArray<FModelDefinition> Definitions;
    Database->GetDefinitions(Definitions);
    SceneRecords.Reserve(Definitions.Num());
    CharacterRecords.Reserve(Definitions.Num());

    for (const FModelDefinition& Definition : Definitions)
    {
        FModelDefinition ResolvedDefinition;
        FString RuntimeReference;
        if (!Database->Resolve(Definition.UUID, ResolvedDefinition, RuntimeReference))
        {
            continue;
        }

        if (ResolvedDefinition.ModelType == EModelDefinitionType::Static)
        {
            FGWorldModelSummary Summary;
            if (!Database->GetSummary(Definition.UUID, Summary))
            {
                WriteLogAsync(FString::Printf(
                    TEXT("Scene omitted because its archive summary is missing. UUID=%s"),
                    *Definition.UUID.ToString()));
                continue;
            }

            FWorldSceneStreamRecord Record;
            Record.UUID = Definition.UUID;
            Record.RuntimeReference = MoveTemp(RuntimeReference);
            Record.Bounds.Center = Summary.Center;
            Record.Bounds.Size = Summary.Size;
            SceneRecords.Add(MoveTemp(Record));
        }
        else if (ResolvedDefinition.ModelType == EModelDefinitionType::Character)
        {
            FWorldCharacterStreamRecord Record;
            Record.UUID = Definition.UUID;
            Record.RuntimeReference = MoveTemp(RuntimeReference);
            Record.Name = ResolvedDefinition.Name;
            Record.DisplayName = ResolvedDefinition.DisplayName;
            CharacterRecords.Add(MoveTemp(Record));
        }
    }

    SceneRecords.Sort([](const FWorldSceneStreamRecord& A,
                         const FWorldSceneStreamRecord& B)
    {
        return A.RuntimeReference < B.RuntimeReference;
    });
    CharacterRecords.Sort([](const FWorldCharacterStreamRecord& A,
                             const FWorldCharacterStreamRecord& B)
    {
        return A.RuntimeReference < B.RuntimeReference;
    });

    // The first pass uses only compact directory summaries. EnsureSceneActor causes exact metadata
    // and decoded .dat member reads solely for a scene whose bounds intersects the player's radius.
    UpdateStreaming();
    ScheduleStreamingUpdates();
    BeginInitialPlayerStreamingIfNeeded();

    WriteLogAsync(FString::Printf(
        TEXT("Built-world streaming started. Scenes=%d Characters=%d RenderOnly=%s"),
        SceneRecords.Num(), CharacterRecords.Num(),
        bRenderOnlyStreaming ? TEXT("true") : TEXT("false")));
}

void UWorldSceneStreamingSubsystem::StopWorldStreaming()
{
    if (!IsInGameThread())
    {
        TWeakObjectPtr<UWorldSceneStreamingSubsystem> WeakThis(this);
        FSafeFileIO::DispatchTrackedGameThread([WeakThis]()
        {
            if (UWorldSceneStreamingSubsystem* StrongThis = WeakThis.Get())
            {
                StrongThis->StopWorldStreaming();
            }
        });
        return;
    }

    ClearTimers();
    bActive = false;

    for (TPair<FString, TObjectPtr<AStaticActor>>& Pair : ActiveSceneActors)
    {
        if (IsValid(Pair.Value))
        {
            Pair.Value->ReleaseRuntimeResourcesForWorldExit();
            Pair.Value->Destroy();
        }
    }
    ActiveSceneActors.Empty();

    // A character loader may still own native parser work. Its cancellation path swaps back to the
    // safe default mesh before the pawn is destroyed, preventing animation/physics use-after-free.
    DeactivatePlayerCharacter();
    if (ACharacterController* Character = ActivePlayerCharacter.Get())
    {
        Character->PrepareForPawnReplacement();
        Character->Destroy();
    }

    OwnerActor = nullptr;
    ActivePlayerCharacter.Reset();
    SceneRecords.Empty();
    CharacterRecords.Empty();
    FailedCharacterReferences.Empty();
    WorldRoot.Reset();
    InitialPlayerName.Reset();
    CurrentCharacterReference.Reset();
    PendingCharacterReference.Reset();
    CurrentCharacterIndex = INDEX_NONE;
    PendingCharacterIndex = INDEX_NONE;
    bStartupFailed = false;
    bInitialScenePassComplete = false;
    bInitialPlayerLoadStarted = false;
    bInitialPlayerLoadComplete = false;
    bWaitingForPlayerLoad = false;
    bPendingPlayerIsInitialLoad = false;
    bPlayerActivated = false;
    bRenderOnlyStreaming = false;
    PlayerActorWaitStartedAt = 0.0;
    PlayerLoadStartedAt = 0.0;
    LastReportedLoadingStatus = 0.0f;
    LastLoadingProgressFrame = ~uint64(0);
}

bool UWorldSceneStreamingSubsystem::AreInitialModelsReady() const
{
    if (!EnsureStreamGameThread(TEXT("UWorldSceneStreamingSubsystem::AreInitialModelsReady")))
    {
        return false;
    }
    if (bStartupFailed)
    {
        return false;
    }
    if (!bActive)
    {
        return true;
    }
    if (!bInitialScenePassComplete)
    {
        return false;
    }

    for (const TPair<FString, TObjectPtr<AStaticActor>>& Pair : ActiveSceneActors)
    {
        if (IsValid(Pair.Value) && !Pair.Value->GetIsLoaded())
        {
            return false;
        }
    }
    return true;
}

bool UWorldSceneStreamingSubsystem::IsPlayerLoaded() const
{
    if (!EnsureStreamGameThread(TEXT("UWorldSceneStreamingSubsystem::IsPlayerLoaded")))
    {
        return false;
    }
    if (bStartupFailed)
    {
        return false;
    }
    if (!bActive)
    {
        return true;
    }
    if (!bInitialPlayerLoadComplete || bWaitingForPlayerLoad)
    {
        return false;
    }

    const ACharacterController* Character = GetPlayerCharacter();
    return !IsValid(Character) || Character->bIsLoaded;
}

bool UWorldSceneStreamingSubsystem::IsInitialWorldReady()
{
    if (!EnsureStreamGameThread(TEXT("UWorldSceneStreamingSubsystem::IsInitialWorldReady")))
    {
        return false;
    }

    BeginInitialPlayerStreamingIfNeeded();
    const bool bReady = AreInitialModelsReady() && IsPlayerLoaded();
    if (bReady)
    {
        ActivatePlayerIfWorldReady();
    }
    return bReady;
}

float UWorldSceneStreamingSubsystem::GetLoadingStatus() const
{
    if (!EnsureStreamGameThread(TEXT("UWorldSceneStreamingSubsystem::GetLoadingStatus")))
    {
        return 0.0f;
    }
    if (bStartupFailed)
    {
        return 0.0f;
    }
    if (!bActive)
    {
        return 1.0f;
    }

    double ProgressSum = 0.0;
    double ProgressCount = 0.0;
    for (const FWorldSceneStreamRecord& Record : SceneRecords)
    {
        float Progress = bInitialScenePassComplete ? 1.0f : 0.0f;
        if (const TObjectPtr<AStaticActor>* Actor = ActiveSceneActors.Find(Record.RuntimeReference))
        {
            if (IsValid(Actor->Get()))
            {
                Progress = FMath::Clamp(Actor->Get()->GetLoadingStatus(), 0.0f, 1.0f);
            }
        }
        ProgressSum += Progress;
        ProgressCount += 1.0;
    }

    float PlayerProgress = 0.0f;
    if (bInitialPlayerLoadComplete && !bWaitingForPlayerLoad)
    {
        PlayerProgress = 1.0f;
    }
    else if (bInitialPlayerLoadStarted)
    {
        PlayerProgress = 0.10f;
        if (bWaitingForPlayerLoad)
        {
            if (const ACharacterController* Character = GetPlayerCharacter())
            {
                PlayerProgress = 0.10f
                    + FMath::Clamp(Character->GetLoadProgress(), 0.0f, 1.0f) * 0.90f;
            }
        }
    }
    ProgressSum += PlayerProgress;
    ProgressCount += 1.0;

    const float RawProgress = ProgressCount > 0.0
        ? static_cast<float>(ProgressSum / ProgressCount) : 0.0f;
    const float Target = AreInitialModelsReady() && IsPlayerLoaded()
        ? 1.0f : FMath::Min(RawProgress, 0.99f);

    if (LastLoadingProgressFrame != GFrameCounter)
    {
        constexpr float MaxVisibleAdvancePerFrame = 0.08f;
        LastReportedLoadingStatus = FMath::Max(
            LastReportedLoadingStatus,
            FMath::Min(Target, LastReportedLoadingStatus + MaxVisibleAdvancePerFrame));
        LastLoadingProgressFrame = GFrameCounter;
    }
    return FMath::Clamp(LastReportedLoadingStatus, 0.0f, 1.0f);
}

void UWorldSceneStreamingSubsystem::UpdateStreaming()
{
    if (!EnsureStreamGameThread(TEXT("UWorldSceneStreamingSubsystem::UpdateStreaming"))
        || !bActive || !IsValid(OwnerActor))
    {
        return;
    }

    int32 SpawnBudget = 2;
    float UnloadMultiplier = 1.10f;
    if (const UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(OwnerActor.Get()))
    {
        if (const UGameSettings* Settings = Manager->GetGameSettings())
        {
            SpawnBudget = Settings->GetStreamingSceneSpawnBudget();
            UnloadMultiplier = Settings->GetStreamingUnloadDistanceMultiplier();
        }
    }

    struct FPendingSceneLoad
    {
        const FWorldSceneStreamRecord* Record = nullptr;
        double DistanceSq = 0.0;
    };
    TArray<FPendingSceneLoad> PendingLoads;
    PendingLoads.Reserve(FMath::Min(SceneRecords.Num(), SpawnBudget * 4));
    const FVector PlayerLocation = GetPlayerLocation();

    for (const FWorldSceneStreamRecord& Record : SceneRecords)
    {
        const TObjectPtr<AStaticActor>* Existing = ActiveSceneActors.Find(Record.RuntimeReference);
        const bool bHasActor = Existing && IsValid(Existing->Get());

        if (bHasActor)
        {
            if (!IsPlayerInsideSceneRange(Record.Bounds, UnloadMultiplier))
            {
                DestroySceneActor(Record.RuntimeReference);
            }
            continue;
        }

        if (IsPlayerInsideSceneRange(Record.Bounds, 1.0f))
        {
            const FVector WorldCenter = IsValid(OwnerActor)
                ? OwnerActor->GetActorTransform().TransformPosition(Record.Bounds.Center)
                : Record.Bounds.Center;
            FPendingSceneLoad& Candidate = PendingLoads.AddDefaulted_GetRef();
            Candidate.Record = &Record;
            Candidate.DistanceSq = FVector::DistSquared(PlayerLocation, WorldCenter);
        }
        else if (Existing)
        {
            // Remove stale null entries without allocating an actor just to clean the map.
            ActiveSceneActors.Remove(Record.RuntimeReference);
        }
    }

    // Spawn the closest coarse scenes first, but cap UObject/actor construction per update. This is
    // the main protection against the initial built-world load monopolizing one game-thread frame.
    PendingLoads.Sort([](const FPendingSceneLoad& A, const FPendingSceneLoad& B)
    {
        return A.DistanceSq < B.DistanceSq;
    });
    const int32 SpawnCount = FMath::Min(SpawnBudget, PendingLoads.Num());
    for (int32 Index = 0; Index < SpawnCount; ++Index)
    {
        if (PendingLoads[Index].Record)
        {
            EnsureSceneActor(*PendingLoads[Index].Record);
        }
    }

    // Initial readiness remains false while an in-range scene is deliberately deferred to a later
    // update by the spawn budget. Out-of-range archive records never block startup.
    bInitialScenePassComplete = PendingLoads.Num() <= SpawnBudget;
    BeginInitialPlayerStreamingIfNeeded();
}

void UWorldSceneStreamingSubsystem::ScheduleStreamingUpdates()
{
    if (!bActive || !IsValid(OwnerActor))
    {
        return;
    }
    if (UWorld* World = OwnerActor->GetWorld())
    {
        World->GetTimerManager().SetTimer(
            TimerHandle_UpdateStreaming,
            FTimerDelegate::CreateUObject(this, &UWorldSceneStreamingSubsystem::UpdateStreaming),
            StreamUpdateIntervalSeconds,
            true);
    }
}

bool UWorldSceneStreamingSubsystem::IsPlayerInsideSceneRange(
    const FModelData& Bounds,
    const float RadiusMultiplier) const
{
    if (!IsFiniteVector(Bounds.Center) || !IsFiniteVector(Bounds.Size)
        || Bounds.Size.IsNearlyZero(0.001f))
    {
        // Marker-only/zero-sized models cannot be culled safely from a radius. Loading them is the
        // conservative behavior, and the archive still limits the read to this one model member.
        return true;
    }

    float DistanceMultiplier = SceneDistanceScale;
    if (const UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(OwnerActor.Get()))
    {
        if (const UGameSettings* Settings = Manager->GetGameSettings())
        {
            DistanceMultiplier = Settings->GetEffectiveStreamingDistanceMultiplier();
        }
    }
    const double Radius = FMath::Max3(Bounds.Size.X, Bounds.Size.Y, Bounds.Size.Z)
        * static_cast<double>(FMath::Max(1.0f, DistanceMultiplier))
        * static_cast<double>(FMath::Clamp(RadiusMultiplier, 1.0f, 2.0f));
    const FVector WorldCenter = IsValid(OwnerActor)
        ? OwnerActor->GetActorTransform().TransformPosition(Bounds.Center)
        : Bounds.Center;
    return FMath::IsFinite(Radius)
        && FVector::DistSquared(GetPlayerLocation(), WorldCenter)
            <= FMath::Square(FMath::Max(1.0, Radius));
}

FVector UWorldSceneStreamingSubsystem::GetPlayerLocation() const
{
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(OwnerActor.Get()))
    {
        return Manager->GetPlayerLocation();
    }
    return FVector::ZeroVector;
}

AStaticActor* UWorldSceneStreamingSubsystem::EnsureSceneActor(
    const FWorldSceneStreamRecord& Record)
{
    if (TObjectPtr<AStaticActor>* Existing = ActiveSceneActors.Find(Record.RuntimeReference))
    {
        if (IsValid(Existing->Get()))
        {
            return Existing->Get();
        }
        ActiveSceneActors.Remove(Record.RuntimeReference);
    }

    if (!IsValid(OwnerActor))
    {
        return nullptr;
    }
    UWorld* World = OwnerActor->GetWorld();
    if (!World)
    {
        return nullptr;
    }

    UClass* SceneClass = AStaticActor::StaticClass();
    if (const auto* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        UClass* Configured = Manager->StaticActorClass.Get();
        if (IsValid(Configured) && Configured->IsChildOf(AStaticActor::StaticClass())
            && !Configured->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
            SceneClass = Configured;
    }
    FActorSpawnParameters Parameters;
    Parameters.Owner = OwnerActor;
    // Scene container placement must never fail because it overlaps existing world geometry.
    Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AStaticActor* Actor = FActorHelper::SpawnActorDeferred<AStaticActor>(
        World,
        SceneClass,
        OwnerActor->GetActorTransform(),
        Parameters);
    if (!IsValid(Actor) && SceneClass != AStaticActor::StaticClass())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Configured WorldStream Static class failed deferred spawn; retrying native AStaticActor. Class=%s"),
            *GetNameSafe(SceneClass));
        Actor = FActorHelper::SpawnActorDeferred<AStaticActor>(
            World, AStaticActor::StaticClass(), OwnerActor->GetActorTransform(), Parameters);
    }
    if (!IsValid(Actor))
    {
        return nullptr;
    }

    Actor->SetRenderOnlyStreaming(bRenderOnlyStreaming);
    Actor->Init(Record.RuntimeReference);
    Actor->FinishSpawning(OwnerActor->GetActorTransform());
    ActiveSceneActors.Add(Record.RuntimeReference, Actor);
    return Actor;
}

void UWorldSceneStreamingSubsystem::DestroySceneActor(const FString& RuntimeReference)
{
    TObjectPtr<AStaticActor>* Existing = ActiveSceneActors.Find(RuntimeReference);
    if (!Existing)
    {
        return;
    }
    if (IsValid(Existing->Get()))
    {
        Existing->Get()->ReleaseRuntimeResourcesForWorldExit();
        Existing->Get()->Destroy();
    }
    ActiveSceneActors.Remove(RuntimeReference);
}

void UWorldSceneStreamingSubsystem::BeginInitialPlayerStreamingIfNeeded()
{
    if (!bActive || bInitialPlayerLoadStarted || !bInitialScenePassComplete)
    {
        return;
    }

    bInitialPlayerLoadStarted = true;
    if (!ResolveInitialCharacterIndex())
    {
        CompletePlayerStreamingWithExistingCharacter(
            TEXT("No selected built character is available"));
        return;
    }

    PlayerActorWaitStartedAt = FPlatformTime::Seconds();
    ScheduleWaitForPlayerActor();
}

bool UWorldSceneStreamingSubsystem::ResolveInitialCharacterIndex()
{
    PendingCharacterIndex = INDEX_NONE;
    if (CharacterRecords.IsEmpty())
    {
        return false;
    }

    if (InitialPlayerName.IsEmpty()
        || InitialPlayerName.Equals(TEXT("Player"), ESearchCase::IgnoreCase))
    {
        PendingCharacterIndex = FindNextLoadableCharacterIndex(0);
        return PendingCharacterIndex != INDEX_NONE;
    }

    FString CandidateSelector = InitialPlayerName;
    if (!CandidateSelector.RemoveFromStart(TEXT("gwd://"), ESearchCase::IgnoreCase))
    {
        CandidateSelector.RemoveFromStart(TEXT("gworld://"), ESearchCase::IgnoreCase); // V3 compatibility
    }
    for (int32 Index = 0; Index < CharacterRecords.Num(); ++Index)
    {
        const FWorldCharacterStreamRecord& Record = CharacterRecords[Index];
        if (Record.RuntimeReference.Equals(InitialPlayerName, ESearchCase::IgnoreCase)
            || StableCharacterId(Record).Equals(CandidateSelector, ESearchCase::IgnoreCase)
            || Record.Name.Equals(InitialPlayerName, ESearchCase::IgnoreCase)
            || Record.DisplayName.Equals(InitialPlayerName, ESearchCase::IgnoreCase))
        {
            PendingCharacterIndex = FailedCharacterReferences.Contains(Record.RuntimeReference)
                ? INDEX_NONE : Index;
            return PendingCharacterIndex != INDEX_NONE;
        }
    }

    WriteLogAsync(FString::Printf(
        TEXT("Selected character was not found in .gwd; using the default pawn. Selector=%s"),
        *InitialPlayerName));
    return false;
}

int32 UWorldSceneStreamingSubsystem::FindNextLoadableCharacterIndex(const int32 StartIndex) const
{
    const int32 Count = CharacterRecords.Num();
    if (Count <= 0)
    {
        return INDEX_NONE;
    }

    const int32 SafeStart = ((StartIndex % Count) + Count) % Count;
    for (int32 Offset = 0; Offset < Count; ++Offset)
    {
        const int32 Index = (SafeStart + Offset) % Count;
        if (!CharacterRecords[Index].RuntimeReference.IsEmpty()
            && !FailedCharacterReferences.Contains(CharacterRecords[Index].RuntimeReference))
        {
            return Index;
        }
    }
    return INDEX_NONE;
}

void UWorldSceneStreamingSubsystem::WaitForPlayerActor()
{
    if (!bActive || !IsValid(OwnerActor))
    {
        return;
    }

    ACharacterController* Character = GetPlayerCharacter();
    if (!IsValid(Character))
    {
        if (FPlatformTime::Seconds() - PlayerActorWaitStartedAt
            >= PlayerActorWaitTimeoutSeconds)
        {
            CompletePlayerStreamingWithExistingCharacter(
                TEXT("Timed out waiting for the gameplay pawn"));
            return;
        }
        ScheduleWaitForPlayerActor();
        return;
    }

    ActivePlayerCharacter = Character;
    RequestCharacterAtIndex(PendingCharacterIndex, true);
}

void UWorldSceneStreamingSubsystem::RequestCharacterAtIndex(
    const int32 CharacterIndex,
    const bool bIsInitialLoad)
{
    if (!bActive || bWaitingForPlayerLoad || !CharacterRecords.IsValidIndex(CharacterIndex))
    {
        if (bIsInitialLoad)
        {
            CompletePlayerStreamingWithExistingCharacter(TEXT("Invalid character selection"));
        }
        return;
    }

    ACharacterController* Character = GetPlayerCharacter();
    if (!IsValid(Character))
    {
        if (bIsInitialLoad)
        {
            CompletePlayerStreamingWithExistingCharacter(TEXT("Gameplay pawn is unavailable"));
        }
        return;
    }

    PendingCharacterIndex = CharacterIndex;
    PendingCharacterReference = CharacterRecords[CharacterIndex].RuntimeReference;
    bPendingPlayerIsInitialLoad = bIsInitialLoad;
    bWaitingForPlayerLoad = true;
    PlayerLoadStartedAt = FPlatformTime::Seconds();
    ActivePlayerCharacter = Character;

    if (bIsInitialLoad)
    {
        bInitialPlayerLoadComplete = false;
        bPlayerActivated = false;
        Character->Activate(false);
    }

    Character->bIsLoaded = false;
    Character->Load(PendingCharacterReference);
    ScheduleWaitForPlayerLoad();
}

void UWorldSceneStreamingSubsystem::WaitForPlayerLoad()
{
    if (!bActive || !IsValid(OwnerActor))
    {
        return;
    }

    ACharacterController* Character = GetPlayerCharacter();
    if (!IsValid(Character))
    {
        HandlePlayerLoadFailure(TEXT("Gameplay pawn was destroyed during character loading"));
        return;
    }
    if (FPlatformTime::Seconds() - PlayerLoadStartedAt >= PlayerLoadTimeoutSeconds)
    {
        HandlePlayerLoadFailure(TEXT("Character payload load timed out"));
        return;
    }
    if (!Character->bIsLoaded)
    {
        ScheduleWaitForPlayerLoad();
        return;
    }
    if (!Character->WasLastMeshLoadSuccessful())
    {
        HandlePlayerLoadFailure(TEXT("Character loader selected its default-mesh fallback"));
        return;
    }

    CurrentCharacterIndex = PendingCharacterIndex;
    CurrentCharacterReference = PendingCharacterReference;
    PendingCharacterIndex = INDEX_NONE;
    PendingCharacterReference.Reset();
    bPendingPlayerIsInitialLoad = false;
    bWaitingForPlayerLoad = false;
    bInitialPlayerLoadComplete = true;
    PlayerLoadStartedAt = 0.0;
    PersistCurrentPlayerSelection();
    ActivatePlayerIfWorldReady();
}

void UWorldSceneStreamingSubsystem::HandlePlayerLoadFailure(const FString& Reason)
{
    const bool bWasInitialLoad = bPendingPlayerIsInitialLoad;
    if (!PendingCharacterReference.IsEmpty())
    {
        // .gwd is immutable for this session, so a failing member cannot become healthy due to
        // an in-place source-file replacement. Quarantine its UUID without further disk polling.
        FailedCharacterReferences.Add(PendingCharacterReference);
    }

    if (ACharacterController* Character = GetPlayerCharacter())
    {
        Character->CancelCharacterLoad(true);
        Character->bIsLoaded = true;
        ActivePlayerCharacter = Character;
    }

    WriteLogAsync(FString::Printf(
        TEXT("Built character isolated; default mesh retained. Reference=%s Reason=%s"),
        *PendingCharacterReference, *Reason));
    PendingCharacterIndex = INDEX_NONE;
    PendingCharacterReference.Reset();
    bPendingPlayerIsInitialLoad = false;
    bWaitingForPlayerLoad = false;
    PlayerLoadStartedAt = 0.0;

    if (bWasInitialLoad)
    {
        CompletePlayerStreamingWithExistingCharacter(Reason);
    }
    else
    {
        bInitialPlayerLoadComplete = true;
        ActivatePlayerIfWorldReady();
    }
}

void UWorldSceneStreamingSubsystem::CompletePlayerStreamingWithExistingCharacter(
    const FString& Reason)
{
    ACharacterController* Character = GetPlayerCharacter();
    if (IsValid(Character))
    {
        ActivePlayerCharacter = Character;
        Character->bIsLoaded = true;
        Character->SetActorHiddenInGame(false);
        Character->SetActorEnableCollision(true);

        if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(OwnerActor.Get()))
        {
            Manager->SetPlayerActor(Character);
            USceneComponent* Camera = Character->GetFollowCameraComponent();
            Manager->SetCameraComponent(Camera);
            Manager->SetPlayerLocation(Character->GetActorLocation(), Character);
            if (UGameInstance* GameInstance = GetGameInstance())
            {
                if (UWeatherSubsystem* Weather = GameInstance->GetSubsystem<UWeatherSubsystem>())
                {
                    Weather->SetWeatherCamera(Camera);
                }
            }
        }
    }

    PendingCharacterIndex = INDEX_NONE;
    PendingCharacterReference.Reset();
    bPendingPlayerIsInitialLoad = false;
    bWaitingForPlayerLoad = false;
    bInitialPlayerLoadComplete = true;
    PlayerActorWaitStartedAt = 0.0;
    PlayerLoadStartedAt = 0.0;
    WriteLogAsync(FString::Printf(
        TEXT("Player streaming completed with the existing pawn. Reason=%s"), *Reason));
    ActivatePlayerIfWorldReady();
}

bool UWorldSceneStreamingSubsystem::CycleNextPlayerCharacter()
{
    if (!EnsureStreamGameThread(TEXT("UWorldSceneStreamingSubsystem::CycleNextPlayerCharacter"))
        || !bActive || !bInitialPlayerLoadComplete || bWaitingForPlayerLoad)
    {
        return false;
    }

    const int32 BaseIndex = CharacterRecords.IsValidIndex(CurrentCharacterIndex)
        ? CurrentCharacterIndex + 1 : 0;
    const int32 NextIndex = FindNextLoadableCharacterIndex(BaseIndex);
    if (NextIndex == INDEX_NONE || NextIndex == CurrentCharacterIndex)
    {
        return false;
    }

    RequestCharacterAtIndex(NextIndex, false);
    return bWaitingForPlayerLoad;
}

ACharacterController* UWorldSceneStreamingSubsystem::GetPlayerCharacter() const
{
    if (IsValid(ActivePlayerCharacter.Get()))
    {
        return ActivePlayerCharacter.Get();
    }
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(OwnerActor.Get()))
    {
        return Manager->GetPlayerActor<ACharacterController>();
    }
    return nullptr;
}

void UWorldSceneStreamingSubsystem::DeactivatePlayerCharacter()
{
    ACharacterController* Character = ActivePlayerCharacter.Get();
    if (!IsValid(Character))
    {
        return;
    }

    Character->Activate(false);
    Character->bIsLoaded = false;
    if (USkeletalMeshComponent* Mesh = Character->GetMesh())
    {
        Mesh->SetAllBodiesSimulatePhysics(false);
        Mesh->SetSimulatePhysics(false);
        Mesh->PutAllRigidBodiesToSleep();
        Mesh->bPauseAnims = false;
        Mesh->SetComponentTickEnabled(true);
    }
}

void UWorldSceneStreamingSubsystem::ActivatePlayerIfWorldReady()
{
    if (bPlayerActivated || !AreInitialModelsReady() || bWaitingForPlayerLoad)
    {
        return;
    }
    ACharacterController* Character = GetPlayerCharacter();
    if (!IsValid(Character) || !Character->bIsLoaded)
    {
        return;
    }

    Character->Activate(true);
    bPlayerActivated = true;
}

void UWorldSceneStreamingSubsystem::PersistCurrentPlayerSelection()
{
    if (!CharacterRecords.IsValidIndex(CurrentCharacterIndex))
    {
        return;
    }
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(OwnerActor.Get()))
    {
        // Persist a stable UUID, not a source filename. The value lives in WorldName.dat.
        Manager->SetSelectedPlayerForRuntime(StableCharacterId(CharacterRecords[CurrentCharacterIndex]));
    }
}

void UWorldSceneStreamingSubsystem::ScheduleWaitForPlayerActor()
{
    if (bActive && IsValid(OwnerActor))
    {
        if (UWorld* World = OwnerActor->GetWorld())
        {
            World->GetTimerManager().SetTimer(
                TimerHandle_WaitPlayer,
                FTimerDelegate::CreateUObject(this, &UWorldSceneStreamingSubsystem::WaitForPlayerActor),
                StreamPollIntervalSeconds,
                false);
        }
    }
}

void UWorldSceneStreamingSubsystem::ScheduleWaitForPlayerLoad()
{
    if (bActive && IsValid(OwnerActor))
    {
        if (UWorld* World = OwnerActor->GetWorld())
        {
            World->GetTimerManager().SetTimer(
                TimerHandle_WaitPlayer,
                FTimerDelegate::CreateUObject(this, &UWorldSceneStreamingSubsystem::WaitForPlayerLoad),
                StreamPollIntervalSeconds,
                false);
        }
    }
}

void UWorldSceneStreamingSubsystem::ClearTimers()
{
    UWorld* World = IsValid(OwnerActor) ? OwnerActor->GetWorld() : GetWorld();
    if (World)
    {
        World->GetTimerManager().ClearTimer(TimerHandle_UpdateStreaming);
        World->GetTimerManager().ClearTimer(TimerHandle_WaitPlayer);
        World->GetTimerManager().ClearAllTimersForObject(this);
    }
}

void UWorldSceneStreamingSubsystem::WriteLogAsync(const FString& Message) const
{
    UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("WorldSceneStreamingSubsystem"), Message);
}
