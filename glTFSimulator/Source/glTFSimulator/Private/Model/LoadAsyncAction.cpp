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

    static bool IsFiniteVector(const FVector& Vector)
    {
        return FMath::IsFinite(Vector.X) &&
            FMath::IsFinite(Vector.Y) &&
            FMath::IsFinite(Vector.Z);
    }

    static bool IsFiniteQuat(const FQuat& Rotation)
    {
        return FMath::IsFinite(Rotation.X) &&
            FMath::IsFinite(Rotation.Y) &&
            FMath::IsFinite(Rotation.Z) &&
            FMath::IsFinite(Rotation.W);
    }

    static bool IsFiniteTransform(const FTransform& Transform)
    {
        const FQuat Rotation = Transform.GetRotation();
        return !Transform.ContainsNaN() &&
            IsFiniteVector(Transform.GetLocation()) &&
            IsFiniteVector(Transform.GetScale3D()) &&
            IsFiniteQuat(Rotation) &&
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

    /** Pure-data payload transferred from the JSON worker back to the game thread. */
    struct FModelJsonWorkerResult
    {
        FModelData ModelData;
        TArray<FString> Warnings;
    };
}


ULoadAsyncAction *ULoadAsyncAction::LoadAsync(
    UObject *WorldContextObject,
    UglTFRuntimeAsset *Asset,
    const FglTFRuntimeStaticMeshConfig &StaticMeshConfig,
    const int32 ChunkSize,
    const FString& InJsonFilePath)
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
    Action->JsonFilePath = InJsonFilePath;
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
        FLoadAsyncWrapper Wrapper;
        Completed.Broadcast(Wrapper);
        ReleaseActionReferences();
        SetReadyToDestroy();
        return;
    }

    Nodes.Reset();
    Nodes.Reserve(ParsedNodes.Num());
    TSet<FName> SeenNodeNames;
    for (const FglTFRuntimeNode& Node : ParsedNodes)
    {
        const FString TrimmedName = Node.Name.TrimStartAndEnd();
        const FName NodeName(*TrimmedName);
        const bool bWaterNode = IsWaterNodeName(TrimmedName);
        const bool bHasValidMesh = Node.MeshIndex >= 0 && Node.MeshIndex < MeshCount;
        if (TrimmedName.IsEmpty() || NodeName.IsNone() || !IsFiniteTransform(Node.Transform) ||
            (!bWaterNode && !bHasValidMesh) || SeenNodeNames.Contains(NodeName))
        {
            continue;
        }

        FglTFRuntimeNode SafeNode = Node;
        SafeNode.Name = TrimmedName;
        Nodes.Add(MoveTemp(SafeNode));
        SeenNodeNames.Add(NodeName);
    }

    MaxCount = Nodes.Num();
    CurrentIndex = 0;
    NodeMap.Reserve(MaxCount);
    WaterNodeMap.Reserve(FMath::Max(1, MaxCount / 16));
    Progress.Broadcast(0.0f);

    LoadJsonAsync();
}

void ULoadAsyncAction::LoadJsonAsync()
{
    if (!EnsureLoadActionGameThread(TEXT("ULoadAsyncAction::LoadJsonAsync")))
    {
        return;
    }

    const FString LocalJsonPath = JsonFilePath;
    TWeakObjectPtr<ULoadAsyncAction> WeakThis(this);

    const bool bWorkerQueued = FSafeFileIO::RunTrackedWorker([WeakThis, LocalJsonPath]()
    {
        // Worker code is intentionally limited to POD/container data and the non-UObject file
        // service. Calling a UBlueprintFunctionLibrary from this thread would make its future
        // implementation changes an accidental game-thread violation.
        FModelJsonWorkerResult WorkerResult;

        if (!FPaths::FileExists(LocalJsonPath))
        {
            WorkerResult.Warnings.Add(FString::Printf(
                TEXT("JSON file not found. Creating default JSON: %s"),
                *LocalJsonPath));

            FModelData EmptyData;
            const FSafeFileWriteResult SaveResult = FSafeFileIO::SaveJsonBlocking(
                EmptyData.Serialization(),
                LocalJsonPath,
                MAX_MODEL_JSON_BYTES);
            if (!SaveResult.IsSuccess())
            {
                WorkerResult.Warnings.Add(FString::Printf(
                    TEXT("Failed to create default JSON. Path=%s Reason=%s"),
                    *LocalJsonPath,
                    *SaveResult.Error));
            }
        }

        FSafeJsonLimits JsonLimits;
        JsonLimits.MaxFileBytes = MAX_MODEL_JSON_BYTES;
        JsonLimits.MaxContainerEntries = MAX_MODEL_NODE_COUNT;
        JsonLimits.bAllowBackupRecovery = true;
        const FSafeJsonLoadResult LoadResult = FSafeFileIO::LoadJsonBlocking(LocalJsonPath, JsonLimits);
        if (LoadResult.IsSuccess())
        {
            const TSharedPtr<FJsonObject>& JsonObject = LoadResult.JsonObject;
            WorkerResult.ModelData.Deserialization(JsonObject);

            if (WorkerResult.ModelData.MeshData.Num() == 0)
            {
                // Backward compatibility: also read the old MeshData array format. This remains
                // bounded by FSafeFileIO's depth/value/container limits before this loop is entered.
                const TArray<TSharedPtr<FJsonValue>>* JsonArrayPtr = nullptr;
                if (JsonObject->TryGetArrayField(TEXT("MeshData"), JsonArrayPtr) && JsonArrayPtr)
                {
                    for (const TSharedPtr<FJsonValue>& Value : *JsonArrayPtr)
                    {
                        if (Value.IsValid() && Value->Type == EJson::Object)
                        {
                            const TSharedPtr<FJsonObject> MeshObj = Value->AsObject();
                            FMeshData MeshData;
                            if (MeshData.Deserialization(MeshObj))
                            {
                                WorkerResult.ModelData.MeshData.Add(NAME_None, MeshData);
                            }
                        }
                    }
                }
            }

            if (LoadResult.bRecoveredFromBackup)
            {
                WorkerResult.Warnings.Add(FString::Printf(
                    TEXT("Recovered model metadata from the backup copy: %s"),
                    *LocalJsonPath));
            }
        }
        else
        {
            WorkerResult.Warnings.Add(FString::Printf(
                TEXT("Failed to read bounded model JSON. Path=%s Reason=%s"),
                *LocalJsonPath,
                *LoadResult.Error));
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
            StrongThis->ProcessChunk();
        }))
        {
            // Shutdown rejected the continuation; UObject cleanup remains on the shutdown path.
            return;
        }
    });

    if (!bWorkerQueued)
    {
        // LoadJsonAsync originates on the game thread, so a rejected worker can be released here.
        bCancelled = true;
        ReleaseActionReferences();
        SetReadyToDestroy();
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
        CalculateSize();
    }
    else
    {
        SanitizeParsedData();
        RefreshGeneratedModelData();
        SaveGeneratedJsonAsync();

        FLoadAsyncWrapper Wrapper;
        Wrapper.NodeMap = MoveTemp(NodeMap);
        Wrapper.WaterNodeMap = MoveTemp(WaterNodeMap);
        Wrapper.MeshMap = MoveTemp(MeshMap);
        Wrapper.ModelData = GeneratedModelData;
        Progress.Broadcast(1.0f);
        Completed.Broadcast(Wrapper);
        ReleaseActionReferences();
        SetReadyToDestroy();
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
    if (!bStaticMeshLoadInFlight)
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
    if (bStaticMeshLoadInFlight && GlTFRuntimeOperationTicket != 0)
    {
        FglTFRuntimeSafety::CancelQueuedOperations(this);
        return;
    }

    bStaticMeshLoadInFlight = false;
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
    MeshMap.Empty();
    NodeMap.Empty();
    WaterNodeMap.Empty();
    LoadedJsonModelData = FModelData();
    GeneratedModelData = FModelData();
    CurrentMeshName = NAME_None;
    CurrentIndex = 0;
    MaxCount = 0;
    JsonFilePath.Reset();
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
        if (RuntimeMeshData.LOD0 == INDEX_NONE || RuntimeMeshData.Size.ContainsNaN())
        {
            It.RemoveCurrent();
            continue;
        }

        if (const FMeshData* ParsedMeshData = LoadedJsonModelData.MeshData.Find(It.Key()))
        {
            RuntimeMeshData.Data = *ParsedMeshData;
        }
    }

    for (auto It = NodeMap.CreateIterator(); It; ++It)
    {
        const FModelNodeData& NodeData = It.Value();
        if (It.Key().IsNone() || NodeData.MeshName.IsNone() ||
            !IsFiniteTransform(NodeData.Transform) || !MeshMap.Contains(NodeData.MeshName))
        {
            It.RemoveCurrent();
        }
    }

    for (auto It = WaterNodeMap.CreateIterator(); It; ++It)
    {
        if (It.Key().IsNone() || !IsFiniteTransform(It.Value().Transform) ||
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
    // Mesh customization is optional user-authored data. Never auto-generate one entry per glTF node/mesh.
    GeneratedModelData.MeshData.Empty();

    FBox Bounds(ForceInit);
    for (const TPair<FName, FModelNodeData>& Pair : NodeMap)
    {
        const FModelNodeData& NodeData = Pair.Value;
        const FModelMeshData* MeshData = MeshMap.Find(NodeData.MeshName);
        if (!MeshData || MeshData->Size.IsNearlyZero(0.001f))
        {
            continue;
        }

        const FVector Extent = MeshData->Size.GetAbs() * 0.5f;
        Bounds += FBox::BuildAABB(NodeData.Transform.GetLocation(), Extent);
    }

    for (const TPair<FName, FWaterStreamNodeData>& Pair : WaterNodeMap)
    {
        const FVector WaterExtent = Pair.Value.Transform.GetScale3D().GetAbs() * 50.0f;
        Bounds += FBox::BuildAABB(Pair.Value.Transform.GetLocation(), FVector(FMath::Max(WaterExtent.X, 100.0f), FMath::Max(WaterExtent.Y, 100.0f), FMath::Max(WaterExtent.Z, 100.0f)));
    }

    if (Bounds.IsValid)
    {
        GeneratedModelData.Center = Bounds.GetCenter();
        GeneratedModelData.Size = Bounds.GetSize();
    }
    else if (!IsValidModelBounds(GeneratedModelData))
    {
        GeneratedModelData.Center = FVector::ZeroVector;
        GeneratedModelData.Size = FVector::ZeroVector;
    }

    WriteLogAsync(FString::Printf(TEXT("Model bounds refreshed. JSON=%s Center=%s Size=%s"),
        *JsonFilePath,
        *GeneratedModelData.Center.ToCompactString(),
        *GeneratedModelData.Size.ToCompactString()));
}

void ULoadAsyncAction::SaveGeneratedJsonAsync() const
{
    if (JsonFilePath.IsEmpty())
    {
        return;
    }

    const TSharedRef<FJsonObject> Json = GeneratedModelData.Serialization();
    UFileFunctionLibrary::ToJsonAsync(Json, JsonFilePath);
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

    if (CurrentNode.Name.TrimStartAndEnd().IsEmpty() || !IsFiniteTransform(CurrentNode.Transform))
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
        // The coordinator serializes this parser's mutable cache, while allowing other GLB
        // assets to calculate bounds concurrently under the global memory cap.
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
    if (NodeName.IsNone() || !IsFiniteTransform(CurrentNode.Transform))
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
        !IsFiniteTransform(CurrentNode.Transform))
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
            const FVector NodeScale = CurrentNode.Transform.GetScale3D().GetAbs();
            Info->Size = StaticMesh->GetBoundingBox().GetSize() * NodeScale;
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

    if (MaxCount > 0)
    {
        Progress.Broadcast(FMath::Clamp(static_cast<float>(CurrentIndex) / static_cast<float>(MaxCount), 0.0f, 1.0f));
    }

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

void ULoadAsyncAction::WriteLogAsync(const FString& Message) const
{
    UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("LoadAsyncAction"), Message);
}
