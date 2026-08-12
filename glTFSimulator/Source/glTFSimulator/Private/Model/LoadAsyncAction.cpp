// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "Model/LoadAsyncAction.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "System/FileFunctionLibrary.h"
#include "System/JsonHelper.h"
#include "System/SafeFileIO.h"
#include "System/glTFRuntimeSafety.h"
#include "System/StringHelper.h"
#include "TimerManager.h"

namespace
{
    static bool IsValidModelBounds(const FModelData& ModelData)
    {
        return !ModelData.Size.IsNearlyZero(0.001f);
    }

    constexpr int64 MAX_MODEL_JSON_BYTES = 64ll * 1024ll * 1024ll;
    constexpr int32 MAX_MODEL_NODE_COUNT = 500000;

    // Loading progress is staged so parsing, cache validation, every parsed node, bounds assembly,
    // and the verified SCZ commit all occupy visible portions of the loading bar.
    constexpr float MODEL_PROGRESS_METADATA_STARTED = 0.02f;
    constexpr float MODEL_PROGRESS_NODE_SCAN_STARTED = 0.10f;
    constexpr float MODEL_PROGRESS_NODE_SCAN_FINISHED = 0.90f;
    constexpr float MODEL_PROGRESS_BOUNDS_READY = 0.94f;
    constexpr float MODEL_PROGRESS_CACHE_COMMIT = 0.98f;

    // Keep file-local helper names unique even under Unreal Unity Build, where anonymous
    // namespaces from several .cpp files are merged into the same translation unit.
    static bool IsFiniteModelLoadVector(const FVector& Vector)
    {
        return FMath::IsFinite(Vector.X) &&
            FMath::IsFinite(Vector.Y) &&
            FMath::IsFinite(Vector.Z);
    }

    static bool IsFiniteModelLoadQuat(const FQuat& Rotation)
    {
        return FMath::IsFinite(Rotation.X) &&
            FMath::IsFinite(Rotation.Y) &&
            FMath::IsFinite(Rotation.Z) &&
            FMath::IsFinite(Rotation.W);
    }

    static bool IsFiniteModelLoadTransform(const FTransform& Transform)
    {
        const FQuat Rotation = Transform.GetRotation();
        return !Transform.ContainsNaN() &&
            IsFiniteModelLoadVector(Transform.GetLocation()) &&
            IsFiniteModelLoadVector(Transform.GetScale3D()) &&
            IsFiniteModelLoadQuat(Rotation) &&
            Rotation.IsNormalized();
    }

    static bool IsWaterNodeName(const FString& NodeName)
    {
        return FStringHelper::GetTextAfterChar(NodeName, ';').ToUpper().Contains(TEXT("WATER"));
    }

    /** UObject, timer, delegate, and glTFRuntimeAsset access is confined to the game thread. */
    static bool EnsureLoadActionGameThread(const TCHAR* FunctionName)
    {
        return ensureMsgf(IsInGameThread(), TEXT("%s must run on the game thread"), FunctionName);
    }

    /** Pure-data payload transferred from the settings/cache worker back to the game thread. */
    struct FModelMetadataWorkerResult
    {
        FModelData ModelData;
        FModelCacheData ModelCache;
        FString ModelHash;
        bool bCacheValid = false;
        bool bCacheDirty = false;
        TArray<FString> Warnings;
    };
}


ULoadAsyncAction *ULoadAsyncAction::LoadAsync(
    UObject *WorldContextObject,
    UglTFRuntimeAsset *Asset,
    const FglTFRuntimeStaticMeshConfig &StaticMeshConfig,
    const int32 ChunkSize,
    const FString& InSourceFilePath,
    const FString& InJsonFilePath,
    const FString& InSizeCacheFilePath,
    const bool bInCreateMissingJsonTemplate)
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::LoadAsync")))
    {
        return nullptr;
    }

    auto *Action = NewObject<ULoadAsyncAction>();
    Action->WorldContextObject = WorldContextObject;
    Action->Asset = Asset;
    Action->StaticMeshConfig = StaticMeshConfig;
    Action->ChunkSize = FMath::Max(1, ChunkSize);
    Action->SourceFilePath = FSafeFileIO::NormalizeFilePath(InSourceFilePath);
    Action->JsonFilePath = FSafeFileIO::NormalizeFilePath(InJsonFilePath);
    Action->SizeCacheFilePath = FSafeFileIO::NormalizeFilePath(InSizeCacheFilePath);
    Action->bCreateMissingJsonTemplate = bInCreateMissingJsonTemplate;
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}

void ULoadAsyncAction::Activate()
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::Activate")))
    {
        return;
    }

    bCancelled = false;
    bStaticMeshLoadInFlight = false;
    bCacheSaveInFlight = false;
    LastProgressValue = 0.0f;
    PendingCompletionWrapper = FLoadAsyncWrapper();
    GlTFRuntimeOperationTicket = 0;

    if (!IsValid(WorldContextObject) || !IsValid(Asset))
    {
        UE_LOG(LogTemp, Error, TEXT("Activate - WorldContextObject or Asset is not valid"));
        WriteLogAsync(TEXT("LoadAsyncAction Activate failed: WorldContextObject or Asset is invalid"));
        ReleaseActionReferences();
        SetReadyToDestroy();
        return;
    }

    const TArray<FglTFRuntimeNode> ParsedNodes = Asset->GetNodes();
    const int32 MeshCount = Asset->GetNumMeshes();
    if (ParsedNodes.Num() > MAX_MODEL_NODE_COUNT)
    {
        WriteLogAsync(FString::Printf(TEXT("Model node count exceeds the safety limit. Count=%d Limit=%d"), ParsedNodes.Num(), MAX_MODEL_NODE_COUNT));
        PendingCompletionWrapper = FLoadAsyncWrapper();
        FinalizeCompletion();
        return;
    }

    Nodes.Reset();
    NodeWorkValidity.Reset();
    Nodes.Reserve(ParsedNodes.Num());
    NodeWorkValidity.Reserve(ParsedNodes.Num());
    TSet<FName> SeenNodeNames;
    for (const FglTFRuntimeNode& Node : ParsedNodes)
    {
        const FString TrimmedName = Node.Name.TrimStartAndEnd();
        const FName NodeName(*TrimmedName);
        const bool bWaterNode = IsWaterNodeName(TrimmedName);
        const bool bHasValidMesh = Node.MeshIndex >= 0 && Node.MeshIndex < MeshCount;
        const bool bValidWorkNode = !TrimmedName.IsEmpty() && !NodeName.IsNone() &&
            IsFiniteModelLoadTransform(Node.Transform) &&
            (bWaterNode || bHasValidMesh) &&
            !SeenNodeNames.Contains(NodeName);

        // Keep every parsed node in the work list. Skipped nodes still advance progress instead of
        // disappearing from the denominator and making the bar jump directly to completion.
        FglTFRuntimeNode WorkNode = Node;
        WorkNode.Name = TrimmedName;
        Nodes.Add(MoveTemp(WorkNode));
        NodeWorkValidity.Add(bValidWorkNode ? 1 : 0);
        if (bValidWorkNode)
        {
            SeenNodeNames.Add(NodeName);
        }
    }

    MaxCount = Nodes.Num();
    CurrentIndex = 0;
    NodeMap.Reserve(MaxCount);
    WaterNodeMap.Reserve(FMath::Max(1, MaxCount / 16));
    BroadcastProgressValue(0.0f);

    LoadSettingsAndCacheAsync();
}

void ULoadAsyncAction::LoadSettingsAndCacheAsync()
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::LoadSettingsAndCacheAsync")))
    {
        return;
    }

    BroadcastProgressValue(MODEL_PROGRESS_METADATA_STARTED);

    const FString LocalSourcePath = SourceFilePath;
    const FString LocalJsonPath = JsonFilePath;
    const FString LocalCachePath = SizeCacheFilePath;
    const bool bLocalCreateMissingJsonTemplate = bCreateMissingJsonTemplate;
    TWeakObjectPtr<ULoadAsyncAction> WeakThis(this);

    const bool bWorkerQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, LocalSourcePath, LocalJsonPath, LocalCachePath, bLocalCreateMissingJsonTemplate]()
    {
        FModelMetadataWorkerResult WorkerResult;

        // The editable JSON document is application read-only. A missing skeleton is created once,
        // but an existing file is never rewritten and JSON backup recovery is intentionally disabled.
        if (bLocalCreateMissingJsonTemplate && !FPaths::FileExists(LocalJsonPath))
        {
            FModelData EmptyData;
            const FSafeFileWriteResult TemplateResult = FSafeFileIO::CreateJsonIfMissingBlocking(
                EmptyData.Serialization(),
                LocalJsonPath,
                MAX_MODEL_JSON_BYTES);
            if (!TemplateResult.IsSuccess())
            {
                WorkerResult.Warnings.Add(FString::Printf(
                    TEXT("Failed to create the read-only model settings template. Path=%s Reason=%s"),
                    *LocalJsonPath,
                    *TemplateResult.Error));
            }
        }

        if (FPaths::FileExists(LocalJsonPath))
        {
            FSafeJsonLimits JsonLimits;
            JsonLimits.MaxFileBytes = MAX_MODEL_JSON_BYTES;
            JsonLimits.MaxContainerEntries = MAX_MODEL_NODE_COUNT;
            JsonLimits.bAllowBackupRecovery = false;
            const FSafeJsonLoadResult LoadResult = FSafeFileIO::LoadJsonBlocking(LocalJsonPath, JsonLimits);
            if (LoadResult.IsSuccess())
            {
                const TSharedPtr<FJsonObject>& JsonObject = LoadResult.JsonObject;
                WorkerResult.ModelData.Deserialization(JsonObject);

                if (WorkerResult.ModelData.MeshData.Num() == 0)
                {
                    // Read-only compatibility for the old MeshData array format. Runtime never writes it back.
                    const TArray<TSharedPtr<FJsonValue>>* JsonArrayPtr = nullptr;
                    if (JsonObject->TryGetArrayField(TEXT("MeshData"), JsonArrayPtr) && JsonArrayPtr)
                    {
                        for (const TSharedPtr<FJsonValue>& Value : *JsonArrayPtr)
                        {
                            if (Value.IsValid() && Value->Type == EJson::Object)
                            {
                                FMeshData MeshData;
                                if (MeshData.Deserialization(Value->AsObject()))
                                {
                                    WorkerResult.ModelData.MeshData.Add(NAME_None, MeshData);
                                }
                            }
                        }
                    }
                }
            }
            else
            {
                WorkerResult.Warnings.Add(FString::Printf(
                    TEXT("Failed to read bounded read-only model JSON. Path=%s Reason=%s"),
                    *LocalJsonPath,
                    *LoadResult.Error));
            }
        }

        FString HashError;
        if (!FBinaryDataStore::ComputeFileSha1(LocalSourcePath, WorkerResult.ModelHash, HashError))
        {
            WorkerResult.Warnings.Add(FString::Printf(
                TEXT("Model SCZ cache disabled because hashing failed. Path=%s Reason=%s"),
                *LocalSourcePath,
                *HashError));
        }
        else
        {
            bool bHashMismatch = false;
            FString CacheError;
            if (FBinaryDataStore::LoadModelCache(
                    LocalCachePath,
                    WorkerResult.ModelHash,
                    WorkerResult.ModelCache,
                    CacheError,
                    bHashMismatch))
            {
                WorkerResult.bCacheValid = true;
            }
            else
            {
                WorkerResult.bCacheDirty = true;
                const bool bHadCacheGeneration = FPaths::FileExists(LocalCachePath) ||
                    FPaths::FileExists(LocalCachePath + TEXT(".bak"));
                if (bHadCacheGeneration)
                {
                    FString DeleteError;
                    if (!FBinaryDataStore::InvalidateCacheFile(LocalCachePath, DeleteError))
                    {
                        WorkerResult.Warnings.Add(FString::Printf(
                            TEXT("Failed to remove stale/corrupt model SCZ. Path=%s Reason=%s"),
                            *LocalCachePath,
                            *DeleteError));
                    }
                    WorkerResult.Warnings.Add(FString::Printf(
                        TEXT("Model SCZ invalidated (%s). Extents will be recalculated. Path=%s Reason=%s"),
                        bHashMismatch ? TEXT("source hash changed") : TEXT("cache validation failed"),
                        *LocalCachePath,
                        *CacheError));
                }
            }
        }

        if (!FSafeFileIO::DispatchTrackedGameThread(
            [WeakThis, WorkerResult = MoveTemp(WorkerResult)]() mutable
        {
            ULoadAsyncAction* StrongThis = WeakThis.Get();
            if (!StrongThis)
            {
                return;
            }

            if (StrongThis->bCancelled || !IsValid(StrongThis->WorldContextObject.Get()))
            {
                StrongThis->ReleaseActionReferences();
                StrongThis->SetReadyToDestroy();
                return;
            }

            for (const FString& Warning : WorkerResult.Warnings)
            {
                StrongThis->WriteLogAsync(Warning);
            }

            StrongThis->LoadedJsonModelData = MoveTemp(WorkerResult.ModelData);
            StrongThis->LoadedModelCache = MoveTemp(WorkerResult.ModelCache);
            StrongThis->CurrentModelHash = MoveTemp(WorkerResult.ModelHash);
            StrongThis->bUseCachedMeshExtents = WorkerResult.bCacheValid;
            StrongThis->bModelCacheDirty = WorkerResult.bCacheDirty;
            StrongThis->BroadcastProgressValue(MODEL_PROGRESS_NODE_SCAN_STARTED);
            StrongThis->ProcessChunk();
        }))
        {
            return;
        }
    });

    if (!bWorkerQueued)
    {
        WriteLogAsync(TEXT("Model metadata worker could not be queued; completing with an empty result"));
        PendingCompletionWrapper = FLoadAsyncWrapper();
        FinalizeCompletion();
    }
}

void ULoadAsyncAction::ProcessChunk()
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::ProcessChunk")))
    {
        return;
    }

    if (bCancelled)
    {
        ReleaseActionReferences();
        SetReadyToDestroy();
        return;
    }

    if (!IsValid(WorldContextObject))
    {
        FLoadAsyncWrapper Wrapper;
        Completed.Broadcast(Wrapper);
        ReleaseActionReferences();
        SetReadyToDestroy();
        return;
    }

    if (CurrentIndex < MaxCount)
    {
        CurrentNode = Nodes[CurrentIndex];
        if (!NodeWorkValidity.IsValidIndex(CurrentIndex) || NodeWorkValidity[CurrentIndex] == 0)
        {
            UpdateNext();
            return;
        }
        CalculateSize();
    }
    else
    {
        SanitizeParsedData();
        RefreshGeneratedModelData();

        PendingCompletionWrapper = FLoadAsyncWrapper();
        PendingCompletionWrapper.NodeMap = MoveTemp(NodeMap);
        PendingCompletionWrapper.WaterNodeMap = MoveTemp(WaterNodeMap);
        PendingCompletionWrapper.MeshMap = MoveTemp(MeshMap);
        PendingCompletionWrapper.ModelData = GeneratedModelData;
        BroadcastProgressValue(MODEL_PROGRESS_BOUNDS_READY);

        // Completion is intentionally delayed until a newly generated SCZ has been durably committed.
        // This lets the world-bake UI treat the completion event as a real on-disk cache guarantee.
        SaveGeneratedCacheThenComplete();
    }
}

void ULoadAsyncAction::CancelAndRelease()
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::CancelAndRelease")))
    {
        return;
    }

    bCancelled = true;
    Completed.Clear();
    Progress.Clear();

    // Remove a request that is waiting in the per-asset queue. Native work already running is
    // allowed to finish and will release its ticket from the plugin's terminal callback.
    FglTFRuntimeSafety::CancelQueuedOperations(this);

    if (UWorld* World = IsValid(WorldContextObject.Get()) ? WorldContextObject->GetWorld() : nullptr)
    {
        World->GetTimerManager().ClearTimer(ProcessTimerHandle);
    }

    // glTFRuntime can still be generating a mesh on a worker thread. Keep the action and
    // parser asset alive until its callback returns; the cancel flag prevents any owner mutation.
    if (!bStaticMeshLoadInFlight && !bCacheSaveInFlight)
    {
        ReleaseActionReferences();
        SetReadyToDestroy();
    }
}

void ULoadAsyncAction::ReleaseActionReferences()
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::ReleaseActionReferences")))
    {
        return;
    }

    bCancelled = true;

    // Never clear an active ticket or parser while glTFRuntime still owns native work. The terminal
    // callback is responsible for releasing the queue slot and then re-entering this cleanup path.
    if ((bStaticMeshLoadInFlight && GlTFRuntimeOperationTicket != 0) || bCacheSaveInFlight)
    {
        FglTFRuntimeSafety::CancelQueuedOperations(this);
        return;
    }

    bStaticMeshLoadInFlight = false;
    bCacheSaveInFlight = false;
    GlTFRuntimeOperationTicket = 0;
    FglTFRuntimeSafety::CancelQueuedOperations(this);

    if (UWorld* World = IsValid(WorldContextObject.Get()) ? WorldContextObject->GetWorld() : nullptr)
    {
        World->GetTimerManager().ClearTimer(ProcessTimerHandle);
    }

    if (IsValid(Asset))
    {
        // The coordinator keeps Asset alive and waits for this parser's native work to finish
        // before clearing mutable glTFRuntime caches. Direct ClearCache here could race a worker.
        FglTFRuntimeSafety::RequestAssetRelease(Asset);
    }

    Completed.Clear();
    Progress.Clear();
    Asset = nullptr;
    WorldContextObject = nullptr;
    Nodes.Empty();
    NodeWorkValidity.Empty();
    MeshMap.Empty();
    NodeMap.Empty();
    WaterNodeMap.Empty();
    LoadedJsonModelData = FModelData();
    GeneratedModelData = FModelData();
    LoadedModelCache = FModelCacheData();
    GeneratedModelCache = FModelCacheData();
    PendingCompletionWrapper = FLoadAsyncWrapper();
    CurrentModelHash.Reset();
    bUseCachedMeshExtents = false;
    bModelCacheDirty = false;
    bCreateMissingJsonTemplate = true;
    CurrentMeshName = NAME_None;
    CurrentIndex = 0;
    MaxCount = 0;
    LastProgressValue = 0.0f;
    SourceFilePath.Reset();
    JsonFilePath.Reset();
    SizeCacheFilePath.Reset();
}

void ULoadAsyncAction::SanitizeParsedData()
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::SanitizeParsedData")))
    {
        return;
    }

    for (auto It = MeshMap.CreateIterator(); It; ++It)
    {
        FModelMeshData& RuntimeMeshData = It.Value();
        if (RuntimeMeshData.LOD0 == INDEX_NONE || RuntimeMeshData.Extent.ContainsNaN() ||
            RuntimeMeshData.Size.ContainsNaN() || RuntimeMeshData.Extent.GetMin() < 0.0)
        {
            It.RemoveCurrent();
            continue;
        }

        if (const FMeshData* ParsedMeshData = LoadedJsonModelData.MeshData.Find(It.Key()))
        {
            RuntimeMeshData.Data = *ParsedMeshData;
        }
        else if (const FMeshData* LegacyDefault = LoadedJsonModelData.MeshData.Find(NAME_None))
        {
            RuntimeMeshData.Data = *LegacyDefault;
        }
    }

    for (auto It = NodeMap.CreateIterator(); It; ++It)
    {
        const FModelNodeData& NodeData = It.Value();
        if (It.Key().IsNone() || NodeData.MeshName.IsNone() ||
            !IsFiniteModelLoadTransform(NodeData.Transform) || !MeshMap.Contains(NodeData.MeshName))
        {
            It.RemoveCurrent();
        }
    }

    for (auto It = WaterNodeMap.CreateIterator(); It; ++It)
    {
        if (It.Key().IsNone() || !IsFiniteModelLoadTransform(It.Value().Transform) ||
            !FMath::IsFinite(It.Value().StreamRadius) || It.Value().StreamRadius <= 0.0f)
        {
            It.RemoveCurrent();
        }
    }

    TSet<FName> ReferencedMeshes;
    ReferencedMeshes.Reserve(NodeMap.Num());
    for (const TPair<FName, FModelNodeData>& Pair : NodeMap)
    {
        ReferencedMeshes.Add(Pair.Value.MeshName);
    }
    for (auto It = MeshMap.CreateIterator(); It; ++It)
    {
        if (!ReferencedMeshes.Contains(It.Key()))
        {
            It.RemoveCurrent();
        }
    }
}

void ULoadAsyncAction::RefreshGeneratedModelData()
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::RefreshGeneratedModelData")))
    {
        return;
    }

    GeneratedModelData = LoadedJsonModelData;
    GeneratedModelCache = FModelCacheData();
    GeneratedModelCache.ModelHash = CurrentModelHash;

    FBox Bounds(ForceInit);
    for (const TPair<FName, FModelNodeData>& Pair : NodeMap)
    {
        const FModelNodeData& NodeData = Pair.Value;
        const FModelMeshData* MeshData = MeshMap.Find(NodeData.MeshName);
        if (!MeshData || MeshData->Extent.IsNearlyZero(0.001f))
        {
            continue;
        }

        const FVector SafeExtent = MeshData->Extent.GetAbs();
        const FBox LocalBounds(-SafeExtent, SafeExtent);
        Bounds += LocalBounds.TransformBy(NodeData.Transform);
    }

    for (const TPair<FName, FWaterStreamNodeData>& Pair : WaterNodeMap)
    {
        const FVector WaterExtent = Pair.Value.Transform.GetScale3D().GetAbs() * 50.0f;
        Bounds += FBox::BuildAABB(
            Pair.Value.Transform.GetLocation(),
            FVector(
                FMath::Max(WaterExtent.X, 100.0f),
                FMath::Max(WaterExtent.Y, 100.0f),
                FMath::Max(WaterExtent.Z, 100.0f)));
    }

    if (Bounds.IsValid)
    {
        GeneratedModelData.Center = Bounds.GetCenter();
        GeneratedModelData.Size = Bounds.GetSize();
    }
    else
    {
        GeneratedModelData.Center = bUseCachedMeshExtents ? LoadedModelCache.Center : FVector::ZeroVector;
        GeneratedModelData.Size = bUseCachedMeshExtents ? LoadedModelCache.Extent * 2.0f : FVector::ZeroVector;
    }

    GeneratedModelCache.Center = GeneratedModelData.Center;
    GeneratedModelCache.Extent = GeneratedModelData.Size.GetAbs() * 0.5f;
    for (const TPair<FName, FModelMeshData>& Pair : MeshMap)
    {
        if (!Pair.Key.IsNone() && !Pair.Value.Extent.ContainsNaN())
        {
            GeneratedModelCache.MeshExtents.Add(Pair.Key, Pair.Value.Extent.GetAbs());
        }
    }

    WriteLogAsync(FString::Printf(
        TEXT("Model bounds ready. SCZ=%s Cache=%s Center=%s Extent=%s"),
        *SizeCacheFilePath,
        bUseCachedMeshExtents && !bModelCacheDirty ? TEXT("hit") : TEXT("rebuilt"),
        *GeneratedModelCache.Center.ToCompactString(),
        *GeneratedModelCache.Extent.ToCompactString()));
}

void ULoadAsyncAction::SaveGeneratedCacheThenComplete()
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::SaveGeneratedCacheThenComplete")))
    {
        return;
    }

    if (!bModelCacheDirty || SizeCacheFilePath.IsEmpty() || CurrentModelHash.IsEmpty() ||
        !GeneratedModelCache.IsSane())
    {
        FinalizeCompletion();
        return;
    }

    BroadcastProgressValue(MODEL_PROGRESS_CACHE_COMMIT);

    const FString LocalCachePath = SizeCacheFilePath;
    const FModelCacheData CacheSnapshot = GeneratedModelCache;
    TWeakObjectPtr<ULoadAsyncAction> WeakThis(this);
    bCacheSaveInFlight = true;

    const bool bWorkerQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, LocalCachePath, CacheSnapshot]()
    {
        const FSafeFileWriteResult Result = FBinaryDataStore::SaveModelCacheBlocking(
            LocalCachePath,
            CacheSnapshot);

        FSafeFileIO::DispatchTrackedGameThread([WeakThis, LocalCachePath, Result]() mutable
        {
            ULoadAsyncAction* StrongThis = WeakThis.Get();
            if (!StrongThis)
            {
                return;
            }

            StrongThis->bCacheSaveInFlight = false;
            if (!Result.IsSuccess())
            {
                StrongThis->WriteLogAsync(FString::Printf(
                    TEXT("Failed to save model SCZ cache. Path=%s Reason=%s"),
                    *LocalCachePath,
                    *Result.Error));
            }

            if (StrongThis->bCancelled)
            {
                StrongThis->ReleaseActionReferences();
                StrongThis->SetReadyToDestroy();
                return;
            }

            StrongThis->FinalizeCompletion();
        });
    });

    if (!bWorkerQueued)
    {
        bCacheSaveInFlight = false;
        WriteLogAsync(FString::Printf(
            TEXT("Model SCZ save worker could not be queued. Path=%s"),
            *LocalCachePath));
        FinalizeCompletion();
    }
}

void ULoadAsyncAction::FinalizeCompletion()
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::FinalizeCompletion")))
    {
        return;
    }

    if (bCancelled)
    {
        ReleaseActionReferences();
        SetReadyToDestroy();
        return;
    }

    BroadcastProgressValue(1.0f);
    FLoadAsyncWrapper Wrapper = MoveTemp(PendingCompletionWrapper);
    Completed.Broadcast(Wrapper);
    ReleaseActionReferences();
    SetReadyToDestroy();
}

void ULoadAsyncAction::CalculateSize()
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::CalculateSize")))
    {
        return;
    }

    if (bCancelled || !IsValid(Asset))
    {
        ReleaseActionReferences();
        SetReadyToDestroy();
        return;
    }

    if (CurrentNode.Name.TrimStartAndEnd().IsEmpty() || !IsFiniteModelLoadTransform(CurrentNode.Transform))
    {
        UpdateNext();
        return;
    }

    const FString Prefix = FStringHelper::GetTextBeforeChar(CurrentNode.Name, ';').TrimStartAndEnd();
    const FString Suffix = FStringHelper::GetTextAfterChar(CurrentNode.Name, ';');
    const FString SuffixUpper = Suffix.ToUpper();

    // A glTF node named "Something;WATER" is a streaming water-volume marker.
    // It owns an AWaterActor at runtime and intentionally does not create mesh data.
    if (SuffixUpper.Contains(TEXT("WATER")))
    {
        UpdateWaterNodeData();
        return;
    }

    if (Prefix.IsEmpty() || CurrentNode.MeshIndex < 0 || CurrentNode.MeshIndex >= Asset->GetNumMeshes())
    {
        UpdateNext();
        return;
    }

    CurrentMeshName = FName(*Prefix);
    if (CurrentMeshName.IsNone())
    {
        UpdateNext();
        return;
    }
    FModelMeshData &Info = MeshMap.FindOrAdd(CurrentMeshName);

    if (SuffixUpper.Contains(TEXT("NCOL")))
    {
        Info.Data.bComplexCollision = false;
        Info.Data.bSimpleCollision = false;
    }

    if (SuffixUpper.Contains(TEXT("INST")))
    {
        UpdateModelNodeData();
    }
    else if (SuffixUpper.Contains(TEXT("LOD1")))
    {
        Info.LOD1 = CurrentNode.MeshIndex;
        UpdateNext();
    }
    else if (SuffixUpper.Contains(TEXT("LOD2")))
    {
        Info.LOD2 = CurrentNode.MeshIndex;
        UpdateNext();
    }
    else if (SuffixUpper.Contains(TEXT("LOD3")))
    {
        Info.LOD3 = CurrentNode.MeshIndex;
        UpdateNext();
    }
    else
    {
        if (bUseCachedMeshExtents)
        {
            if (const FVector* CachedExtent = LoadedModelCache.MeshExtents.Find(CurrentMeshName))
            {
                if (!CachedExtent->ContainsNaN() && CachedExtent->GetMin() >= 0.0)
                {
                    Info.LOD0 = CurrentNode.MeshIndex;
                    Info.Extent = CachedExtent->GetAbs();
                    Info.Size = Info.Extent * 2.0f;
                    UpdateModelNodeData();
                    return;
                }
            }
        }

        // Multiple nodes can reference the same base mesh. Once its local bounds were calculated in
        // this run, reuse them instead of scheduling another temporary UStaticMesh build.
        if (Info.LOD0 == CurrentNode.MeshIndex && !Info.Extent.IsNearlyZero(0.001f) &&
            !Info.Extent.ContainsNaN())
        {
            UpdateModelNodeData();
            return;
        }

        // A missing, stale, or invalid SCZ loads each unique base mesh once and calculates its
        // unscaled local extent from UStaticMesh::GetBoundingBox(). The generated cache is committed
        // only after every parsed node (including skipped nodes) has advanced the loading progress.
        bModelCacheDirty = !CurrentModelHash.IsEmpty();
        const int32 RequestedMeshIndex = CurrentNode.MeshIndex;
        const FglTFRuntimeStaticMeshConfig RequestedConfig = StaticMeshConfig;
        TWeakObjectPtr<ULoadAsyncAction> WeakThis(this);
        bStaticMeshLoadInFlight = true;
        const uint64 SubmittedTicket = FglTFRuntimeSafety::EnqueueOperation(
            this,
            Asset,
            FString::Printf(TEXT("Model metadata mesh %d"), RequestedMeshIndex),
            [WeakThis, RequestedMeshIndex, RequestedConfig](const uint64 Ticket)
            {
                ULoadAsyncAction* StrongThis = WeakThis.Get();
                if (!IsValid(StrongThis) || StrongThis->bCancelled || !IsValid(StrongThis->Asset))
                {
                    if (IsValid(StrongThis))
                    {
                        StrongThis->GlTFRuntimeOperationTicket = 0;
                        StrongThis->bStaticMeshLoadInFlight = false;
                        StrongThis->ReleaseActionReferences();
                        StrongThis->SetReadyToDestroy();
                    }
                    // Keep the ticket active until release has been requested. Completing first
                    // could allow another queued operation to reuse this parser before teardown.
                    FglTFRuntimeSafety::CompleteOperation(Ticket);
                    return;
                }

                StrongThis->GlTFRuntimeOperationTicket = Ticket;
                FglTFRuntimeStaticMeshAsync Callback;
                Callback.BindDynamic(StrongThis, &ULoadAsyncAction::GetStaticMesh);
                StrongThis->Asset->LoadStaticMeshAsync(RequestedMeshIndex, Callback, RequestedConfig);
            },
            [WeakThis](const FString& Reason)
            {
                ULoadAsyncAction* StrongThis = WeakThis.Get();
                if (!IsValid(StrongThis))
                {
                    return;
                }

                StrongThis->GlTFRuntimeOperationTicket = 0;
                StrongThis->bStaticMeshLoadInFlight = false;
                if (StrongThis->bCancelled)
                {
                    StrongThis->ReleaseActionReferences();
                    StrongThis->SetReadyToDestroy();
                    return;
                }

                StrongThis->WriteLogAsync(FString::Printf(
                    TEXT("Static mesh request was rejected by the glTFRuntime safety queue: %s"),
                    *Reason));
                StrongThis->MeshMap.Remove(StrongThis->CurrentMeshName);
                StrongThis->UpdateNext();
            });

        // EnqueueOperation may start immediately. Do not overwrite state after a synchronous cache-hit
        // callback has already completed and reset the action.
        if (bStaticMeshLoadInFlight && GlTFRuntimeOperationTicket == 0)
        {
            GlTFRuntimeOperationTicket = SubmittedTicket;
        }
    }
}


void ULoadAsyncAction::UpdateWaterNodeData()
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::UpdateWaterNodeData")))
    {
        return;
    }

    const FName NodeName(*CurrentNode.Name.TrimStartAndEnd());
    if (NodeName.IsNone() || !IsFiniteModelLoadTransform(CurrentNode.Transform))
    {
        UpdateNext();
        return;
    }
    if (!WaterNodeMap.Contains(NodeName))
    {
        FWaterStreamNodeData WaterInfo;
        WaterInfo.Transform = CurrentNode.Transform;
        // Unreal scale should be multiplied by 100
        WaterInfo.Transform.SetScale3D(CurrentNode.Transform.GetScale3D() * 100.0f);
        const FVector AbsScale = CurrentNode.Transform.GetScale3D().GetAbs();
        WaterInfo.StreamRadius = FMath::Max(2048.0f, FMath::Max3(AbsScale.X, AbsScale.Y, AbsScale.Z) * 65536.0f);
        WaterNodeMap.Add(NodeName, WaterInfo);
    }
    UpdateNext();
}

void ULoadAsyncAction::UpdateModelNodeData()
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::UpdateModelNodeData")))
    {
        return;
    }

    const FName NodeName(*CurrentNode.Name.TrimStartAndEnd());
    if (NodeName.IsNone() || CurrentMeshName.IsNone() || !MeshMap.Contains(CurrentMeshName) ||
        !IsFiniteModelLoadTransform(CurrentNode.Transform))
    {
        UpdateNext();
        return;
    }
    if (!NodeMap.Contains(NodeName))
    {
        FModelNodeData NodeInfo;
        NodeInfo.MeshName = CurrentMeshName;
        NodeInfo.Transform = CurrentNode.Transform;
        NodeMap.Add(NodeName, NodeInfo);
    }
    UpdateNext();
}

void ULoadAsyncAction::GetStaticMesh(UStaticMesh *StaticMesh)
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::GetStaticMesh")))
    {
        return;
    }

    // Keep the coordinator ticket active until all callback-side UObject work is complete. If
    // cancellation requested cache release, the cache barrier is finalized by this scope exit.
    const uint64 CompletedTicket = GlTFRuntimeOperationTicket;
    GlTFRuntimeOperationTicket = 0;
    bStaticMeshLoadInFlight = false;
    ON_SCOPE_EXIT
    {
        FglTFRuntimeSafety::CompleteOperation(CompletedTicket);
    };
    if (bCancelled)
    {
        if (IsValid(StaticMesh) && !StaticMesh->IsAsset())
        {
            StaticMesh->ClearFlags(RF_Public | RF_Standalone);
        }
        ReleaseActionReferences();
        SetReadyToDestroy();
        return;
    }

    const bool bMeshValid = IsValid(StaticMesh) && IsValid(Asset) && CurrentNode.MeshIndex >= 0 &&
        CurrentNode.MeshIndex < Asset->GetNumMeshes();
    if (bMeshValid)
    {
        if (FModelMeshData *Info = MeshMap.Find(CurrentMeshName))
        {
            Info->LOD0 = CurrentNode.MeshIndex;
            Info->Extent = StaticMesh->GetBoundingBox().GetExtent().GetAbs();
            Info->Size = Info->Extent * 2.0f;
            bModelCacheDirty = !CurrentModelHash.IsEmpty();
        }
    }
    else
    {
        MeshMap.Remove(CurrentMeshName);
    }

    if (IsValid(StaticMesh) && !StaticMesh->IsAsset())
    {
        StaticMesh->ClearFlags(RF_Public | RF_Standalone);
    }

    if (bMeshValid)
    {
        UpdateModelNodeData();
    }
    else
    {
        UpdateNext();
    }
}

void ULoadAsyncAction::UpdateNext()
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::UpdateNext")))
    {
        return;
    }

    if (bCancelled || !IsValid(WorldContextObject.Get()))
    {
        ReleaseActionReferences();
        SetReadyToDestroy();
        return;
    }

    ++CurrentIndex;
    CurrentMeshName = NAME_None;

    BroadcastNodeProgress();

    if (CurrentIndex % ChunkSize == 0)
    {
        UWorld *World = IsValid(WorldContextObject.Get()) ? WorldContextObject->GetWorld() : nullptr;
        if (IsValid(World))
        {
            ProcessTimerHandle = World->GetTimerManager().SetTimerForNextTick(
                FTimerDelegate::CreateUObject(this, &ULoadAsyncAction::ProcessChunk));
        }
        else
        {
            ReleaseActionReferences();
            SetReadyToDestroy();
        }
    }
    else
    {
        ProcessChunk();
    }
}

void ULoadAsyncAction::BroadcastProgressValue(float Value)
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::BroadcastProgressValue")))
    {
        return;
    }

    const float SafeValue = FMath::IsFinite(Value) ? FMath::Clamp(Value, 0.0f, 1.0f) : LastProgressValue;
    LastProgressValue = FMath::Max(LastProgressValue, SafeValue);
    Progress.Broadcast(LastProgressValue);
}

void ULoadAsyncAction::BroadcastNodeProgress()
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::BroadcastNodeProgress")))
    {
        return;
    }

    const float NodeFraction = MaxCount > 0
        ? FMath::Clamp(static_cast<float>(CurrentIndex) / static_cast<float>(MaxCount), 0.0f, 1.0f)
        : 1.0f;
    BroadcastProgressValue(FMath::Lerp(
        MODEL_PROGRESS_NODE_SCAN_STARTED,
        MODEL_PROGRESS_NODE_SCAN_FINISHED,
        NodeFraction));
}

void ULoadAsyncAction::WriteLogAsync(const FString& Message) const
{
    UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("LoadAsyncAction"), Message);
}
