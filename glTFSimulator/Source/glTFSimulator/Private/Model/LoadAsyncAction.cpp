// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "Model/LoadAsyncAction.h"

#include "Async/Async.h"
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
#include "System/MacroLibrary.h"
#include "System/StringHelper.h"
#include "TimerManager.h"

namespace
{
    static bool IsValidModelBounds(const FModelData& ModelData)
    {
        return !ModelData.Size.IsNearlyZero(0.001f);
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

    if (!IsValid(WorldContextObject) || !IsValid(Asset))
    {
        UE_LOG(LogTemp, Error, TEXT("Activate - WorldContextObject or Asset is not valid"));
        WriteLogAsync(TEXT("LoadAsyncAction Activate failed: WorldContextObject or Asset is invalid"));
        ReleaseActionReferences();
        SetReadyToDestroy();
        return;
    }

    Nodes = Asset->GetNodes();
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

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakThis, LocalJsonPath]()
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

        if (FFileHelper::LoadFileToString(JsonString, *LocalJsonPath))
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
            WriteLoadLog(FString::Printf(TEXT("Failed to read JSON: %s"), *LocalJsonPath));
        }

        AsyncTask(ENamedThreads::GameThread, [WeakThis, TemporaryModelData]()
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
        });
    });
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
        MergeJsonDataToMeshMap();
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
    ReleaseActionReferences();
    SetReadyToDestroy();
}

void ULoadAsyncAction::ReleaseActionReferences()
{
    bCancelled = true;

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
        Asset->MarkAsGarbage();
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

void ULoadAsyncAction::MergeJsonDataToMeshMap()
{
    if (LoadedJsonModelData.MeshData.Num() == 0)
    {
        return;
    }

    for (auto& Pair : MeshMap)
    {
        FName MeshName = Pair.Key;
        FModelMeshData& MeshData = Pair.Value;

        if (FMeshData* FoundJsonData = LoadedJsonModelData.MeshData.Find(MeshName))
        {
            MeshData.Data = *FoundJsonData;
        }
    }
}

void ULoadAsyncAction::RefreshGeneratedModelData()
{
    GeneratedModelData = LoadedJsonModelData;
    GeneratedModelData.MeshData.Empty();

    for (const TPair<FName, FModelMeshData>& Pair : MeshMap)
    {
        GeneratedModelData.MeshData.Add(Pair.Key, Pair.Value.Data);
    }

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

    const FString Prefix = FStringHelper::GetTextBeforeChar(CurrentNode.Name, ';');
    const FString Suffix = FStringHelper::GetTextAfterChar(CurrentNode.Name, ';');
    const FString SuffixUpper = Suffix.ToUpper();

    // A glTF node named "Something;WATER" is a streaming water-volume marker.
    // It owns an AWaterActor at runtime and intentionally does not create mesh data.
    if (SuffixUpper.Contains(TEXT("WATER")))
    {
        UpdateWaterNodeData();
        return;
    }

    CurrentMeshName = FName(Prefix);
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
        FglTFRuntimeStaticMeshAsync Callback;
        Callback.BindDynamic(this, &ULoadAsyncAction::GetStaticMesh);
        Asset->LoadStaticMeshAsync(CurrentNode.MeshIndex, Callback, StaticMeshConfig);
    }
}


void ULoadAsyncAction::UpdateWaterNodeData()
{
    const FName NodeName(CurrentNode.Name);
    if (!WaterNodeMap.Contains(NodeName))
    {
        FWaterStreamNodeData WaterInfo;
        WaterInfo.Transform = CurrentNode.Transform;
        const FVector AbsScale = CurrentNode.Transform.GetScale3D().GetAbs();
        WaterInfo.StreamRadius = FMath::Max(2048.0f, FMath::Max3(AbsScale.X, AbsScale.Y, AbsScale.Z) * 65536.0f);
        WaterNodeMap.Add(NodeName, WaterInfo);
    }
    UpdateNext();
}

void ULoadAsyncAction::UpdateModelNodeData()
{
    FName NodeName = FName(CurrentNode.Name);
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
    if (bCancelled)
    {
        if (IsValid(StaticMesh) && !StaticMesh->IsAsset())
        {
            StaticMesh->MarkAsGarbage();
        }
        ReleaseActionReferences();
        SetReadyToDestroy();
        return;
    }

    if (FModelMeshData *Info = MeshMap.Find(CurrentMeshName))
    {
        Info->LOD0 = CurrentNode.MeshIndex;
        if (StaticMesh)
        {
            const FVector NodeScale = CurrentNode.Transform.GetScale3D().GetAbs();
            Info->Size = StaticMesh->GetBoundingBox().GetSize() * NodeScale;
        }
    }

    if (IsValid(StaticMesh) && !StaticMesh->IsAsset())
    {
        StaticMesh->MarkAsGarbage();
        StaticMesh = nullptr;
    }

    UpdateModelNodeData();
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
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [ImagePath, WeakThis, TextureDimensionLimit]()
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

                AsyncTask(ENamedThreads::GameThread, [WeakThis, Width, Height, PixelData = MoveTemp(UncompressedRGBA)]() mutable
                {
                    if (ULoadAsyncAction* StrongThis = WeakThis.Get())
                    {
                        if (StrongThis->bCancelled)
                        {
                            return;
                        }
                    }

                    UTexture2D* NewTexture = UTexture2D::CreateTransient(Width, Height, PF_R8G8B8A8);
                    if (NewTexture && PixelData.Num() == Width * Height * 4)
                    {
                        void* TextureData = NewTexture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
                        FMemory::Memcpy(TextureData, PixelData.GetData(), PixelData.Num());
                        NewTexture->GetPlatformData()->Mips[0].BulkData.Unlock();
                        NewTexture->UpdateResource();
                    }
                });
            }
        }
    });
}

void ULoadAsyncAction::WriteLogAsync(const FString& Message) const
{
    UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("LoadAsyncAction"), Message);
}
