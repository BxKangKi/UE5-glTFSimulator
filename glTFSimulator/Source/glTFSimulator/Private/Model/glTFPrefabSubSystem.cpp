// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Model/glTFPrefabSubSystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "System/GlbValidation.h"
#include "System/SafeFileIO.h"
#include "System/glTFRuntimeSafety.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeParser.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    bool IsPrefabDescriptor(const FString& PrefabFilePath, FString& OutReason)
    {
        const FString JsonPath = FPaths::ChangeExtension(PrefabFilePath, TEXT("json"));
        if (!IFileManager::Get().FileExists(*JsonPath))
        {
            OutReason = FString::Printf(TEXT("prefab descriptor JSON is missing: %s"), *JsonPath);
            return false;
        }

        FSafeJsonLimits Limits;
        Limits.MaxFileBytes = 64ll * 1024ll * 1024ll;
        Limits.MaxDepth = 32;
        Limits.MaxValues = 100000;
        Limits.MaxContainerEntries = 100000;
        Limits.MaxStringCharacters = 32768;
        Limits.MaxPrimitiveCharacters = 1024;
        Limits.bAllowBackupRecovery = false;

        const FSafeJsonLoadResult LoadResult = FSafeFileIO::LoadJsonBlocking(JsonPath, Limits);
        if (!LoadResult.IsSuccess() || !LoadResult.JsonObject.IsValid())
        {
            OutReason = FString::Printf(TEXT("prefab descriptor JSON is invalid: %s"), *LoadResult.Error);
            return false;
        }

        FString AssetType;
        if (!LoadResult.JsonObject->TryGetStringField(TEXT("AssetType"), AssetType))
        {
            OutReason = TEXT("prefab descriptor JSON has no AssetType field");
            return false;
        }

        AssetType.TrimStartAndEndInline();
        if (!AssetType.Equals(TEXT("prefab"), ESearchCase::IgnoreCase))
        {
            OutReason = FString::Printf(
                TEXT("prefab descriptor AssetType is '%s', expected 'prefab'"),
                *AssetType);
            return false;
        }

        return true;
    }

    int32 FindFirstRenderableMeshIndex(UglTFRuntimeAsset* RuntimeAsset)
    {
        if (!IsValid(RuntimeAsset))
        {
            return INDEX_NONE;
        }

        const int32 MeshCount = RuntimeAsset->GetNumMeshes();
        if (MeshCount <= 0)
        {
            return INDEX_NONE;
        }

        for (const FglTFRuntimeNode& Node : RuntimeAsset->GetNodes())
        {
            if (Node.MeshIndex >= 0 && Node.MeshIndex < MeshCount)
            {
                return Node.MeshIndex;
            }
        }

        return 0;
    }
}

struct UglTFPrefabSubSystem::FPrefabLoadResult
{
    bool bSuccess = false;
    FString PrefabFilePath;
    FString FailureReason;
    TSharedPtr<FglTFRuntimeParser> Parser;
};

UglTFPrefabSubSystem* UglTFPrefabSubSystem::Get(UObject* WorldContextObject)
{
    if (!IsInGameThread() || !IsValid(WorldContextObject))
    {
        return nullptr;
    }

    UWorld* World = WorldContextObject->GetWorld();
    UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
    return IsValid(GameInstance)
        ? GameInstance->GetSubsystem<UglTFPrefabSubSystem>()
        : nullptr;
}

bool UglTFPrefabSubSystem::EnsurePrefabSubsystemGameThread(const TCHAR* FunctionName) const
{
    return ensureMsgf(
        IsInGameThread(),
        TEXT("%s must run on the game thread"),
        FunctionName);
}

FString UglTFPrefabSubSystem::NormalizePrefabName(const FString& PrefabName)
{
    FString Normalized = PrefabName;
    Normalized.TrimStartAndEndInline();
    if (Normalized.IsEmpty() ||
        Normalized.Contains(TEXT("/")) ||
        Normalized.Contains(TEXT("\\")) ||
        Normalized.Contains(TEXT("..")))
    {
        return FString();
    }

    if (FPaths::GetExtension(Normalized).Equals(TEXT("glb"), ESearchCase::IgnoreCase))
    {
        Normalized = FPaths::GetBaseFilename(Normalized);
    }

    return Normalized;
}

FString UglTFPrefabSubSystem::ResolvePrefabPath(
    const FString& ModelFilePath,
    const FString& PrefabName) const
{
    if (!EnsurePrefabSubsystemGameThread(TEXT("UglTFPrefabSubSystem::ResolvePrefabPath")))
    {
        return FString();
    }

    const FString NormalizedName = NormalizePrefabName(PrefabName);
    if (NormalizedName.IsEmpty())
    {
        return FString();
    }

    const FString NormalizedModelPath = GlbValidation::NormalizePath(ModelFilePath);
    if (NormalizedModelPath.IsEmpty())
    {
        return FString();
    }

    const int32 StreamMarker = NormalizedModelPath.Find(
        TEXT("/stream/"),
        ESearchCase::IgnoreCase,
        ESearchDir::FromEnd);
    if (StreamMarker == INDEX_NONE)
    {
        return FString();
    }

    const FString ModelRoot = NormalizedModelPath.Left(StreamMarker);
    return GlbValidation::NormalizePath(
        FPaths::Combine(ModelRoot, TEXT("prefab"), NormalizedName + TEXT(".glb")));
}

void UglTFPrefabSubSystem::AcquirePrefabReference(
    const FString& ModelFilePath,
    const FString& PrefabName,
    const FString& ReferenceToken,
    FOnPrefabRuntimeAssetReady Callback)
{
    if (!EnsurePrefabSubsystemGameThread(TEXT("UglTFPrefabSubSystem::AcquirePrefabReference")))
    {
        return;
    }

    const FString PrefabFilePath = ResolvePrefabPath(ModelFilePath, PrefabName);
    if (PrefabFilePath.IsEmpty() || ReferenceToken.IsEmpty())
    {
        Callback.ExecuteIfBound(nullptr, INDEX_NONE, false, TEXT("invalid prefab reference"));
        return;
    }

    FPrefabRuntimeState& State = RuntimeStates.FindOrAdd(PrefabFilePath);
    if (!State.ReferenceTokens.Contains(ReferenceToken))
    {
        State.ReferenceTokens.Add(ReferenceToken);
        ++State.ReferenceCount;
    }

    if (TObjectPtr<UglTFRuntimeAsset>* LoadedAsset = LoadedAssets.Find(PrefabFilePath))
    {
        const int32 MeshIndex = FindFirstRenderableMeshIndex(LoadedAsset->Get());
        if (MeshIndex != INDEX_NONE)
        {
            Callback.ExecuteIfBound(LoadedAsset->Get(), MeshIndex, true, FString());
            return;
        }
    }

    if (!State.FailureReason.IsEmpty() && !State.bLoading)
    {
        const FString Failure = State.FailureReason;
        Callback.ExecuteIfBound(nullptr, INDEX_NONE, false, Failure);
        return;
    }

    State.PendingCallbacks.Add(MoveTemp(Callback));
    if (!State.bLoading)
    {
        StartPrefabLoad(PrefabFilePath, State);
    }
}

void UglTFPrefabSubSystem::StartPrefabLoad(
    const FString& PrefabFilePath,
    FPrefabRuntimeState& State)
{
    State.bLoading = true;
    State.FailureReason.Reset();
    State.LoadCancelToken = MakeShared<FThreadSafeCounter, ESPMode::ThreadSafe>(0);

    const TSharedPtr<FThreadSafeCounter, ESPMode::ThreadSafe> CancelToken = State.LoadCancelToken;
    TWeakObjectPtr<UglTFPrefabSubSystem> WeakThis(this);

    const bool bQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, PrefabFilePath, CancelToken]()
        {
            TSharedPtr<FPrefabLoadResult, ESPMode::ThreadSafe> Result = MakeShared<FPrefabLoadResult, ESPMode::ThreadSafe>();
            Result->PrefabFilePath = PrefabFilePath;

            if (!CancelToken.IsValid() || CancelToken->GetValue() != 0)
            {
                Result->FailureReason = TEXT("prefab load cancelled");
            }
            else if (!IFileManager::Get().FileExists(*PrefabFilePath))
            {
                Result->FailureReason = FString::Printf(
                    TEXT("prefab GLB file does not exist: %s"),
                    *PrefabFilePath);
            }
            else if (!IsPrefabDescriptor(PrefabFilePath, Result->FailureReason))
            {
            }
            else if (!GlbValidation::ValidateRuntimeMeshFile(PrefabFilePath, Result->FailureReason))
            {
                Result->FailureReason = FString::Printf(
                    TEXT("prefab runtime mesh preflight failed: %s"),
                    *Result->FailureReason);
            }
            else
            {
                FglTFRuntimeConfig Config;
                Config.bAllowExternalFiles = true;
                Result->Parser = FglTFRuntimeSafety::CreateParserSafely(PrefabFilePath, Config);
                if (Result->Parser.IsValid())
                {
                    Result->bSuccess = true;
                }
                else
                {
                    Result->FailureReason = TEXT("glTFRuntime prefab parser creation failed");
                }
            }

            if (!CancelToken.IsValid() || CancelToken->GetValue() != 0)
            {
                return;
            }

            FSafeFileIO::DispatchTrackedGameThread(
                [WeakThis, Result]()
                {
                    if (UglTFPrefabSubSystem* StrongThis = WeakThis.Get())
                    {
                        StrongThis->FinishPrefabLoad(Result->PrefabFilePath, Result);
                    }
                });
        });

    if (!bQueued)
    {
        State.bLoading = false;
        State.LoadCancelToken.Reset();
        const FString Failure = TEXT("prefab load worker could not be queued");
        TArray<FOnPrefabRuntimeAssetReady> Callbacks = MoveTemp(State.PendingCallbacks);
        State.PendingCallbacks.Empty();
        State.ReferenceTokens.Empty();
        State.ReferenceCount = 0;
        for (FOnPrefabRuntimeAssetReady& Callback : Callbacks)
        {
            Callback.ExecuteIfBound(nullptr, INDEX_NONE, false, Failure);
        }
        TryUnloadPrefab(PrefabFilePath);
    }
}

void UglTFPrefabSubSystem::FinishPrefabLoad(
    const FString& PrefabFilePath,
    TSharedPtr<FPrefabLoadResult, ESPMode::ThreadSafe> Result)
{
    if (!EnsurePrefabSubsystemGameThread(TEXT("UglTFPrefabSubSystem::FinishPrefabLoad")))
    {
        return;
    }

    FPrefabRuntimeState* State = RuntimeStates.Find(PrefabFilePath);
    if (!State)
    {
        return;
    }

    State->bLoading = false;
    State->LoadCancelToken.Reset();

    if (!Result.IsValid() || !Result->bSuccess || !Result->Parser.IsValid())
    {
        const FString Failure = Result.IsValid() && !Result->FailureReason.IsEmpty()
            ? Result->FailureReason
            : TEXT("unknown prefab load failure");
        State->FailureReason = Failure;
        TArray<FOnPrefabRuntimeAssetReady> Callbacks = MoveTemp(State->PendingCallbacks);
        State->PendingCallbacks.Empty();
        State->ReferenceTokens.Empty();
        State->ReferenceCount = 0;
        for (FOnPrefabRuntimeAssetReady& Callback : Callbacks)
        {
            Callback.ExecuteIfBound(nullptr, INDEX_NONE, false, Failure);
        }
        TryUnloadPrefab(PrefabFilePath);
        return;
    }

    UglTFRuntimeAsset* RuntimeAsset = NewObject<UglTFRuntimeAsset>(this, NAME_None, RF_Transient);
    if (!IsValid(RuntimeAsset) || !RuntimeAsset->SetParser(Result->Parser.ToSharedRef()))
    {
        if (IsValid(RuntimeAsset))
        {
            FglTFRuntimeSafety::RequestAssetRelease(RuntimeAsset);
        }

        const FString Failure = TEXT("glTFRuntime prefab asset initialization failed");
        State->FailureReason = Failure;
        TArray<FOnPrefabRuntimeAssetReady> Callbacks = MoveTemp(State->PendingCallbacks);
        State->PendingCallbacks.Empty();
        State->ReferenceTokens.Empty();
        State->ReferenceCount = 0;
        for (FOnPrefabRuntimeAssetReady& Callback : Callbacks)
        {
            Callback.ExecuteIfBound(nullptr, INDEX_NONE, false, Failure);
        }
        TryUnloadPrefab(PrefabFilePath);
        return;
    }

    const int32 MeshIndex = FindFirstRenderableMeshIndex(RuntimeAsset);
    if (MeshIndex == INDEX_NONE)
    {
        FglTFRuntimeSafety::RequestAssetRelease(RuntimeAsset);
        const FString Failure = TEXT("prefab GLB has no renderable mesh");
        State->FailureReason = Failure;
        TArray<FOnPrefabRuntimeAssetReady> Callbacks = MoveTemp(State->PendingCallbacks);
        State->PendingCallbacks.Empty();
        State->ReferenceTokens.Empty();
        State->ReferenceCount = 0;
        for (FOnPrefabRuntimeAssetReady& Callback : Callbacks)
        {
            Callback.ExecuteIfBound(nullptr, INDEX_NONE, false, Failure);
        }
        TryUnloadPrefab(PrefabFilePath);
        return;
    }

    LoadedAssets.Add(PrefabFilePath, RuntimeAsset);
    State->FailureReason.Reset();

    TArray<FOnPrefabRuntimeAssetReady> Callbacks = MoveTemp(State->PendingCallbacks);
    State->PendingCallbacks.Empty();
    for (FOnPrefabRuntimeAssetReady& Callback : Callbacks)
    {
        Callback.ExecuteIfBound(RuntimeAsset, MeshIndex, true, FString());
    }

    UE_LOG(LogTemp, Log,
        TEXT("glTFPrefabSubSystem: prefab loaded and shared. Path=%s MeshIndex=%d References=%d"),
        *PrefabFilePath,
        MeshIndex,
        State->ReferenceCount);

    TryUnloadPrefab(PrefabFilePath);
}

void UglTFPrefabSubSystem::ReleasePrefabReference(
    const FString& PrefabFilePath,
    const FString& ReferenceToken)
{
    if (!EnsurePrefabSubsystemGameThread(TEXT("UglTFPrefabSubSystem::ReleasePrefabReference")))
    {
        return;
    }

    FPrefabRuntimeState* State = RuntimeStates.Find(PrefabFilePath);
    if (!State || !State->ReferenceTokens.Remove(ReferenceToken))
    {
        return;
    }

    State->ReferenceCount = FMath::Max(0, State->ReferenceCount - 1);
    UE_LOG(LogTemp, Verbose,
        TEXT("glTFPrefabSubSystem: prefab reference released. Path=%s References=%d ActiveUses=%d"),
        *PrefabFilePath,
        State->ReferenceCount,
        State->ActiveUseCount);
    TryUnloadPrefab(PrefabFilePath);
}

bool UglTFPrefabSubSystem::BeginPrefabAssetUse(
    const FString& PrefabFilePath,
    const FString& ReferenceToken)
{
    if (!EnsurePrefabSubsystemGameThread(TEXT("UglTFPrefabSubSystem::BeginPrefabAssetUse")))
    {
        return false;
    }

    FPrefabRuntimeState* State = RuntimeStates.Find(PrefabFilePath);
    if (!State || !State->ReferenceTokens.Contains(ReferenceToken) ||
        !LoadedAssets.Contains(PrefabFilePath) || State->ActiveUseTokens.Contains(ReferenceToken))
    {
        return false;
    }

    State->ActiveUseTokens.Add(ReferenceToken);
    ++State->ActiveUseCount;
    return true;
}

void UglTFPrefabSubSystem::EndPrefabAssetUse(
    const FString& PrefabFilePath,
    const FString& ReferenceToken)
{
    if (!EnsurePrefabSubsystemGameThread(TEXT("UglTFPrefabSubSystem::EndPrefabAssetUse")))
    {
        return;
    }

    FPrefabRuntimeState* State = RuntimeStates.Find(PrefabFilePath);
    if (!State || !State->ActiveUseTokens.Remove(ReferenceToken))
    {
        return;
    }

    State->ActiveUseCount = FMath::Max(0, State->ActiveUseCount - 1);
    TryUnloadPrefab(PrefabFilePath);
}

void UglTFPrefabSubSystem::TryUnloadPrefab(const FString& PrefabFilePath)
{
    FPrefabRuntimeState* State = RuntimeStates.Find(PrefabFilePath);
    if (!State || State->bLoading || State->ReferenceCount > 0 || State->ActiveUseCount > 0)
    {
        return;
    }

    if (TObjectPtr<UglTFRuntimeAsset>* AssetPtr = LoadedAssets.Find(PrefabFilePath))
    {
        if (IsValid(AssetPtr->Get()))
        {
            UE_LOG(LogTemp, Log,
                TEXT("glTFPrefabSubSystem: unloading prefab GLTFRuntimeAsset. Path=%s"),
                *PrefabFilePath);
            FglTFRuntimeSafety::RequestAssetRelease(AssetPtr->Get());
        }
        LoadedAssets.Remove(PrefabFilePath);
    }

    RuntimeStates.Remove(PrefabFilePath);
}

int32 UglTFPrefabSubSystem::GetReferenceCount(const FString& PrefabFilePath) const
{
    if (!EnsurePrefabSubsystemGameThread(TEXT("UglTFPrefabSubSystem::GetReferenceCount")))
    {
        return 0;
    }

    if (const FPrefabRuntimeState* State = RuntimeStates.Find(PrefabFilePath))
    {
        return State->ReferenceCount;
    }

    return 0;
}

void UglTFPrefabSubSystem::Deinitialize()
{
    if (!EnsurePrefabSubsystemGameThread(TEXT("UglTFPrefabSubSystem::Deinitialize")))
    {
        return;
    }

    for (TPair<FString, FPrefabRuntimeState>& Pair : RuntimeStates)
    {
        if (Pair.Value.LoadCancelToken.IsValid())
        {
            Pair.Value.LoadCancelToken->Increment();
        }
        Pair.Value.PendingCallbacks.Empty();
        Pair.Value.ReferenceTokens.Empty();
        Pair.Value.ActiveUseTokens.Empty();
    }

    for (TPair<FString, TObjectPtr<UglTFRuntimeAsset>>& Pair : LoadedAssets)
    {
        if (IsValid(Pair.Value))
        {
            FglTFRuntimeSafety::RequestAssetRelease(Pair.Value.Get());
        }
    }

    LoadedAssets.Empty();
    RuntimeStates.Empty();
    Super::Deinitialize();
}
