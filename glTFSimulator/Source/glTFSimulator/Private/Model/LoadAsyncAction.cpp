// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "Model/LoadAsyncAction.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
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
    Action->JsonFilePath = InJsonFilePath;
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}

void ULoadAsyncAction::Activate()
{
    if (!IsValid(WorldContextObject) || !IsValid(Asset))
    {
        UE_LOG(LogTemp, Error, TEXT("Activate - WorldContextObject or Asset is not valid"));
        WriteLogAsync(TEXT("LoadAsyncAction Activate failed: WorldContextObject or Asset is invalid"));
        SetReadyToDestroy();
        return;
    }

    Nodes = Asset->GetNodes();
    MaxCount = Nodes.Num();
    CurrentIndex = 0;
    NodeMap.Reserve(MaxCount);
    Progress.Broadcast(0.0f);

    LoadJsonAsync();
}

void ULoadAsyncAction::LoadJsonAsync()
{
    const FString LocalJsonPath = JsonFilePath;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, LocalJsonPath]()
    {
        if (!FPaths::FileExists(LocalJsonPath))
        {
            WriteLogAsync(FString::Printf(TEXT("JSON file not found. Creating default JSON: %s"), *LocalJsonPath));
            if (!CreateDefaultJsonFile(LocalJsonPath))
            {
                WriteLogAsync(FString::Printf(TEXT("Failed to create default JSON: %s"), *LocalJsonPath));
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
                WriteLogAsync(FString::Printf(TEXT("JSON is damaged or unreadable: %s"), *LocalJsonPath));
            }
        }
        else
        {
            WriteLogAsync(FString::Printf(TEXT("Failed to read JSON: %s"), *LocalJsonPath));
        }

        AsyncTask(ENamedThreads::GameThread, [this, TemporaryModelData]()
        {
            LoadedJsonModelData = TemporaryModelData;
            ProcessChunk();
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
    if (!IsValid(WorldContextObject))
    {
        FLoadAsyncWrapper Wrapper;
        Completed.Broadcast(Wrapper);
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

        if (IsValid(Asset))
        {
            Asset->ClearCache();
            Asset->MarkAsGarbage();
            Asset = nullptr;
        }

        UWorld *World = WorldContextObject->GetWorld();
        if (IsValid(World))
        {
            World->GetTimerManager().ClearTimer(ProcessTimerHandle);
        }

        FLoadAsyncWrapper Wrapper;
        Wrapper.NodeMap = MoveTemp(NodeMap);
        Wrapper.MeshMap = MoveTemp(MeshMap);
        Wrapper.ModelData = GeneratedModelData;
        Progress.Broadcast(1.0f);
        Completed.Broadcast(Wrapper);
        SetReadyToDestroy();
    }
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
    FString Prefix = FStringHelper::GetTextBeforeChar(CurrentNode.Name, ';');
    FString Suffix = FStringHelper::GetTextAfterChar(CurrentNode.Name, ';');
    CurrentMeshName = FName(Prefix);
    FModelMeshData &Info = MeshMap.FindOrAdd(CurrentMeshName);

    if (Suffix.Contains(TEXT("NCOL")))
    {
        Info.Data.bComplexCollision = false;
        Info.Data.bSimpleCollision = false;
    }

    if (Suffix.Contains(TEXT("INST")))
    {
        UpdateModelNodeData();
    }
    else if (Suffix.Contains(TEXT("LOD1")))
    {
        Info.LOD1 = CurrentNode.MeshIndex;
        UpdateNext();
    }
    else if (Suffix.Contains(TEXT("LOD2")))
    {
        Info.LOD2 = CurrentNode.MeshIndex;
        UpdateNext();
    }
    else if (Suffix.Contains(TEXT("LOD3")))
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
    if (FModelMeshData *Info = MeshMap.Find(CurrentMeshName))
    {
        Info->LOD0 = CurrentNode.MeshIndex;
        if (StaticMesh)
        {
            const FVector NodeScale = CurrentNode.Transform.GetScale3D().GetAbs();
            Info->Size = StaticMesh->GetBoundingBox().GetSize() * NodeScale;
        }
    }

    if (StaticMesh)
    {
        StaticMesh->MarkAsGarbage();
        StaticMesh = nullptr;
    }

    UpdateModelNodeData();
}

void ULoadAsyncAction::UpdateNext()
{
    ++CurrentIndex;
    CurrentMeshName = NAME_None;

    if (MaxCount > 0)
    {
        Progress.Broadcast(FMath::Clamp(static_cast<float>(CurrentIndex) / static_cast<float>(MaxCount), 0.0f, 1.0f));
    }

    if (CurrentIndex % ChunkSize == 0)
    {
        UWorld *World = WorldContextObject->GetWorld();
        if (IsValid(World))
        {
            ProcessTimerHandle = World->GetTimerManager().SetTimerForNextTick(
                FTimerDelegate::CreateUObject(this, &ULoadAsyncAction::ProcessChunk));
        }
    }
    else
    {
        ProcessChunk();
    }
}

void ULoadAsyncAction::LoadTextureAsync(FString ImagePath)
{
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [ImagePath, this]()
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

                AsyncTask(ENamedThreads::GameThread, [=]()
                {
                    UTexture2D* NewTexture = UTexture2D::CreateTransient(Width, Height, PF_R8G8B8A8);
                    if (NewTexture)
                    {
                        void* TextureData = NewTexture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
                        FMemory::Memcpy(TextureData, UncompressedRGBA.GetData(), UncompressedRGBA.Num());
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
