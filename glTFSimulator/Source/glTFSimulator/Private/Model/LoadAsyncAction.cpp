// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "Model/LoadAsyncAction.h"

#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/CoreMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Setting/GameSettings.h"
#include "System/FileFunctionLibrary.h"
#include "System/JsonHelper.h"
#include "System/SafeFileIO.h"
#include "System/glTFRuntimeSafety.h"
#include "System/MacroLibrary.h"
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

    static void ResizeRGBA8Texture(const TArray<uint8>& SourcePixels, int32 SourceWidth, int32 SourceHeight, int32 TargetWidth, int32 TargetHeight, TArray<uint8>& OutPixels)
    {
        OutPixels.Reset();
        if (SourceWidth <= 0 || SourceHeight <= 0 || TargetWidth <= 0 || TargetHeight <= 0 || SourcePixels.Num() < SourceWidth * SourceHeight * 4)
        {
            return;
        }

        OutPixels.SetNumUninitialized(TargetWidth * TargetHeight * 4);
        const float XScale = static_cast<float>(SourceWidth) / static_cast<float>(TargetWidth);
        const float YScale = static_cast<float>(SourceHeight) / static_cast<float>(TargetHeight);

        for (int32 Y = 0; Y < TargetHeight; ++Y)
        {
            const int32 SourceY = FMath::Clamp(FMath::FloorToInt((static_cast<float>(Y) + 0.5f) * YScale), 0, SourceHeight - 1);
            for (int32 X = 0; X < TargetWidth; ++X)
            {
                const int32 SourceX = FMath::Clamp(FMath::FloorToInt((static_cast<float>(X) + 0.5f) * XScale), 0, SourceWidth - 1);
                const int32 SourceIndex = (SourceY * SourceWidth + SourceX) * 4;
                const int32 TargetIndex = (Y * TargetWidth + X) * 4;
                OutPixels[TargetIndex + 0] = SourcePixels[SourceIndex + 0];
                OutPixels[TargetIndex + 1] = SourcePixels[SourceIndex + 1];
                OutPixels[TargetIndex + 2] = SourcePixels[SourceIndex + 2];
                OutPixels[TargetIndex + 3] = SourcePixels[SourceIndex + 3];
            }
        }
    }
}

ULoadAsyncAction *ULoadAsyncAction::LoadAsync(
    UObject *WorldContextObject,
    UglTFRuntimeAsset *Asset,
    const FglTFRuntimeStaticMeshConfig &StaticMeshConfig,
    const int32 ChunkSize,
    const FString& InJsonFilePath)
{
    auto *Action = NewObject<ULoadAsyncAction>();
    Action->WorldContextObject = WorldContextObject;
    Action->Asset = Asset;
    Action->StaticMeshConfig = StaticMeshConfig;
    Action->ChunkSize = FMath::Max(1, ChunkSize);
    Action->MaxTextureDimension = UGameSettings::ResolveMaxTextureResolution(WorldContextObject);
    Action->JsonFilePath = InJsonFilePath;
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}

void ULoadAsyncAction::Activate()
{
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
    const FString LocalJsonPath = JsonFilePath;
    TWeakObjectPtr<ULoadAsyncAction> WeakThis(this);

    const bool bWorkerQueued = FSafeFileIO::RunTrackedWorker([WeakThis, LocalJsonPath]()
    {
        auto WriteLoadLog = [](const FString& Message)
        {
            UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("LoadAsyncAction"), Message);
        };

        auto CreateDefaultJson = [](const FString& Path) -> bool
        {
            UFileFunctionLibrary::GenerateDirectory(Path);

            FModelData EmptyData;
            TSharedRef<FJsonObject> RootObject = EmptyData.Serialization();

            FString OutputString;
            TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
            if (FJsonSerializer::Serialize(RootObject, Writer))
            {
                return FFileHelper::SaveStringToFile(OutputString, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            }

            return false;
        };

        if (!FPaths::FileExists(LocalJsonPath))
        {
            WriteLoadLog(FString::Printf(TEXT("JSON file not found. Creating default JSON: %s"), *LocalJsonPath));
            if (!CreateDefaultJson(LocalJsonPath))
            {
                WriteLoadLog(FString::Printf(TEXT("Failed to create default JSON: %s"), *LocalJsonPath));
            }
        }

        FString JsonString;
        FModelData TemporaryModelData;
        const int64 JsonFileSize = IFileManager::Get().FileSize(*LocalJsonPath);

        if (JsonFileSize >= 0 && JsonFileSize <= MAX_MODEL_JSON_BYTES &&
            FFileHelper::LoadFileToString(JsonString, *LocalJsonPath))
        {
            TSharedPtr<FJsonObject> JsonObject;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

            if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
            {
                TemporaryModelData.Deserialization(JsonObject);

                if (TemporaryModelData.MeshData.Num() == 0)
                {
                    // Backward compatibility: also read the old MeshData array format.
                    const TArray<TSharedPtr<FJsonValue>>* JsonArrayPtr = nullptr;
                    if (JsonObject->TryGetArrayField(TEXT("MeshData"), JsonArrayPtr) && JsonArrayPtr)
                    {
                        for (const TSharedPtr<FJsonValue>& Value : *JsonArrayPtr)
                        {
                            if (Value.IsValid() && Value->Type == EJson::Object)
                            {
                                TSharedPtr<FJsonObject> MeshObj = Value->AsObject();
                                FMeshData MeshData;
                                if (MeshData.Deserialization(MeshObj))
                                {
                                    TemporaryModelData.MeshData.Add(NAME_None, MeshData);
                                }
                            }
                        }
                    }
                }
            }
            else
            {
                WriteLoadLog(FString::Printf(TEXT("JSON is damaged or unreadable: %s"), *LocalJsonPath));
            }
        }
        else
        {
            WriteLoadLog(FString::Printf(TEXT("Failed to read JSON or file exceeds %lld bytes: %s"), MAX_MODEL_JSON_BYTES, *LocalJsonPath));
        }

        if (!FSafeFileIO::DispatchTrackedGameThread([WeakThis, TemporaryModelData]()
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

            StrongThis->LoadedJsonModelData = TemporaryModelData;
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

bool ULoadAsyncAction::CreateDefaultJsonFile(const FString& Path)
{
    UFileFunctionLibrary::GenerateDirectory(Path);

    FModelData EmptyData;
    TSharedRef<FJsonObject> RootObject = EmptyData.Serialization();

    FString OutputString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
    if (FJsonSerializer::Serialize(RootObject, Writer))
    {
        return FFileHelper::SaveStringToFile(OutputString, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }

    return false;
}

void ULoadAsyncAction::ProcessChunk()
{
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
    bCancelled = true;
    Completed.Clear();
    Progress.Clear();

    // Remove a request that is waiting in the process-wide queue. Native work already running is
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
        Asset->ClearCache();
        if (Asset->IsRooted())
        {
            Asset->RemoveFromRoot();
        }
        Asset->ClearFlags(RF_Public | RF_Standalone);
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
        // Serialize allocation-heavy plugin builders. This prevents world metadata, streamed
        // meshes, prefabs, vehicles, weapons, and characters from building concurrently.
        const int32 RequestedMeshIndex = CurrentNode.MeshIndex;
        const FglTFRuntimeStaticMeshConfig RequestedConfig = StaticMeshConfig;
        TWeakObjectPtr<ULoadAsyncAction> WeakThis(this);
        bStaticMeshLoadInFlight = true;
        GlTFRuntimeOperationTicket = FglTFRuntimeSafety::EnqueueOperation(
            this,
            FString::Printf(TEXT("Model metadata mesh %d"), RequestedMeshIndex),
            [WeakThis, RequestedMeshIndex, RequestedConfig](const uint64 Ticket)
            {
                ULoadAsyncAction* StrongThis = WeakThis.Get();
                if (!IsValid(StrongThis) || StrongThis->bCancelled || !IsValid(StrongThis->Asset))
                {
                    FglTFRuntimeSafety::CompleteOperation(Ticket);
                    if (IsValid(StrongThis))
                    {
                        StrongThis->GlTFRuntimeOperationTicket = 0;
                        StrongThis->bStaticMeshLoadInFlight = false;
                        StrongThis->ReleaseActionReferences();
                        StrongThis->SetReadyToDestroy();
                    }
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
    }
}


void ULoadAsyncAction::UpdateWaterNodeData()
{
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
    // The plugin callback is the only safe point to release an active native-operation ticket.
    const uint64 CompletedTicket = GlTFRuntimeOperationTicket;
    GlTFRuntimeOperationTicket = 0;
    FglTFRuntimeSafety::CompleteOperation(CompletedTicket);
    bStaticMeshLoadInFlight = false;
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

void ULoadAsyncAction::LoadTextureAsync(FString ImagePath)
{
    TWeakObjectPtr<ULoadAsyncAction> WeakThis(this);
    const int32 TextureDimensionLimit = FMath::Clamp(MaxTextureDimension, 64, 8192);
    const bool bWorkerQueued = FSafeFileIO::RunTrackedWorker(
        [ImagePath, WeakThis, TextureDimensionLimit]()
    {
        TArray<uint8> RawFileData;
        if (!FFileHelper::LoadFileToArray(RawFileData, *ImagePath))
        {
            return;
        }

        IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
        EImageFormat Format = ImageWrapperModule.DetectImageFormat(RawFileData.GetData(), RawFileData.Num());
        TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(Format);

        if (ImageWrapper.IsValid() && ImageWrapper->SetCompressed(RawFileData.GetData(), RawFileData.Num()))
        {
            TArray<uint8> UncompressedRGBA;
            if (ImageWrapper->GetRaw(ERGBFormat::RGBA, 8, UncompressedRGBA))
            {
                int32 Width = ImageWrapper->GetWidth();
                int32 Height = ImageWrapper->GetHeight();

                const int32 LargestDimension = FMath::Max(Width, Height);
                if (LargestDimension > TextureDimensionLimit)
                {
                    const float ResizeScale = static_cast<float>(TextureDimensionLimit) / static_cast<float>(LargestDimension);
                    const int32 TargetWidth = FMath::Max(1, FMath::RoundToInt(static_cast<float>(Width) * ResizeScale));
                    const int32 TargetHeight = FMath::Max(1, FMath::RoundToInt(static_cast<float>(Height) * ResizeScale));

                    TArray<uint8> ResizedRGBA;
                    ResizeRGBA8Texture(UncompressedRGBA, Width, Height, TargetWidth, TargetHeight, ResizedRGBA);
                    if (ResizedRGBA.Num() == TargetWidth * TargetHeight * 4)
                    {
                        UncompressedRGBA = MoveTemp(ResizedRGBA);
                        Width = TargetWidth;
                        Height = TargetHeight;
                    }
                }

                if (!FSafeFileIO::DispatchTrackedGameThread(
                    [WeakThis, Width, Height, PixelData = MoveTemp(UncompressedRGBA)]() mutable
                {
                    ULoadAsyncAction* StrongThis = WeakThis.Get();
                    if (!IsValid(StrongThis) || StrongThis->bCancelled)
                    {
                        return;
                    }

                    UTexture2D* NewTexture = UTexture2D::CreateTransient(Width, Height, PF_R8G8B8A8);
                    if (NewTexture && PixelData.Num() == Width * Height * 4)
                    {
                        void* TextureData = NewTexture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
                        FMemory::Memcpy(TextureData, PixelData.GetData(), PixelData.Num());
                        NewTexture->GetPlatformData()->Mips[0].BulkData.Unlock();
                        NewTexture->UpdateResource();
                    }
                }))
                {
                    // Pixel data is released with the rejected continuation during shutdown.
                    return;
                }
            }
        }
    });

    if (!bWorkerQueued)
    {
        // Texture loading is optional; shutdown rejection requires no UObject-side completion.
        return;
    }
}

void ULoadAsyncAction::WriteLogAsync(const FString& Message) const
{
    UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("LoadAsyncAction"), Message);
}
