// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "Model/LoadAsyncAction.h"
#include "System/StringHelper.h"
#include "System/MacroLibrary.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Misc/CoreMisc.h" // FCString
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Async/Async.h"
#include "Engine/Texture2D.h"

// 파일 입출력 및 JSON 직렬화를 위한 파일 포함
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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
    Action->ChunkSize = ChunkSize;
    Action->JsonFilePath = InJsonFilePath; // 내부 경로 보관
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}

void ULoadAsyncAction::Activate()
{
    if (!IsValid(WorldContextObject) || !IsValid(Asset))
    {
        UE_LOG(LogTemp, Error, TEXT("Activate - WorldContextObject or Asset is not valid"));
        SetReadyToDestroy();
        return;
    }
    Nodes = Asset->GetNodes();
    MaxCount = Nodes.Num();
    
    // [최적화] 맵 크기 미리 예약하여 성능 향상
    NodeMap.Reserve(MaxCount);
    
    // [변경] 순서를 보장하기 위해 JSON 파일 비동기 처리를 먼저 수행합니다.
    LoadJsonAsync();
}

void ULoadAsyncAction::LoadJsonAsync()
{
    // 1. 백그라운드 스레드(워커 스레드)에서 파일 검사, 필요 시 생성, 파싱 연산 처리
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this]()
    {
        // [추가] 대상 JSON 파일이 없다면 디폴트 데이터를 생성하여 하드디스크에 먼저 파일로 기록합니다.
        if (!FPaths::FileExists(JsonFilePath))
        {
            UE_LOG(LogTemp, Warning, TEXT("LoadJsonAsync - JSON file not found. Creating default JSON at: %s"), *JsonFilePath);
            if (!CreateDefaultJsonFile(JsonFilePath))
            {
                UE_LOG(LogTemp, Error, TEXT("LoadJsonAsync - Failed to create default JSON file. Proceeding with empty data."));
            }
        }

        FString JsonString;
        FRuntimeModelData TemporaryModelData;

        if (FFileHelper::LoadFileToString(JsonString, *JsonFilePath))
        {
            TSharedPtr<FJsonObject> JsonObject;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

            if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
            {
                // 기본 구조체 센터(Center) 데이터 복구
                JsonObject->TryGetNumberField(TEXT("X"), TemporaryModelData.Center.X);
                JsonObject->TryGetNumberField(TEXT("Y"), TemporaryModelData.Center.Y);
                JsonObject->TryGetNumberField(TEXT("Z"), TemporaryModelData.Center.Z);

                // JSON 맵 구조 파싱 처리 (Key: MeshName 오브젝트 매핑 방식 지원)
                const TSharedPtr<FJsonObject>* MeshDataJsonPtr = nullptr;
                if (JsonObject->TryGetObjectField(TEXT("MeshData"), MeshDataJsonPtr) && MeshDataJsonPtr->IsValid())
                {
                    for (const auto& KeyValue : (*MeshDataJsonPtr)->Values)
                    {
                        FName MeshKey = FName(*KeyValue.Key);
                        TSharedPtr<FJsonObject> MeshObj = KeyValue.Value->AsObject();
                        
                        if (MeshObj.IsValid())
                        {
                            FRuntimeMeshData MeshData;
                            if (MeshData.Deserialization(MeshObj))
                            {
                                TemporaryModelData.MeshData.Add(MeshKey, MeshData);
                            }
                        }
                    }
                }
                // 만약 JSON 구조가 오브젝트 맵이 아니라 배열([]) 형태 형태일 경우 하위 호환 처리
                else 
                {
                    const TArray<TSharedPtr<FJsonValue>>* JsonArrayPtr = nullptr;
                    if (JsonObject->TryGetArrayField(TEXT("MeshData"), JsonArrayPtr) && JsonArrayPtr)
                    {
                        for (const TSharedPtr<FJsonValue>& Value : *JsonArrayPtr)
                        {
                            if (Value.IsValid() && Value->Type == EJson::Object)
                            {
                                TSharedPtr<FJsonObject> MeshObj = Value->AsObject();
                                FRuntimeMeshData MeshData;
                                if (MeshData.Deserialization(MeshObj))
                                {
                                    TemporaryModelData.MeshData.Add(NAME_None, MeshData);
                                }
                            }
                        }
                    }
                }
            }
        }

        // 2. 파싱 연산 완료 후, 언리얼 오브젝트 데이터를 안전하게 갱신하기 위해 게임 스레드로 복귀
        AsyncTask(ENamedThreads::GameThread, [this, TemporaryModelData]()
        {
            this->LoadedJsonModelData = TemporaryModelData;
            
            // 데이터 수집 준비가 완료되었으므로 청크 루프 프로세스 개시
            ProcessChunk();
        });
    });
}

bool ULoadAsyncAction::CreateDefaultJsonFile(const FString& Path)
{
    // 최상위 빈 루트 오브젝트 선언
    TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
    
    // FRuntimeModelData 기본값 세팅 규칙 반영 (Center 및 내부 빈 MeshData 구성)
    RootObject->SetNumberField(TEXT("X"), 0.0f);
    RootObject->SetNumberField(TEXT("Y"), 0.0f);
    RootObject->SetNumberField(TEXT("Z"), 0.0f);
    
    // 빈 MeshData 오브젝트 생성 후 할당 ({ "MeshData": {} })
    TSharedRef<FJsonObject> EmptyMeshDataMap = MakeShared<FJsonObject>();
    RootObject->SetObjectField(TEXT("MeshData"), EmptyMeshDataMap);

    // 문자열 버퍼로 전환 후 직렬화 파일 쓰기
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
        return; // Early exit 추가로 함수 안정성 확보
    }

    if (CurrentIndex < MaxCount)
    {
        CurrentNode = Nodes[CurrentIndex];
        CalculateSize();
    }
    else
    {
        // 모든 청크 순회가 끝나 가비지 컬렉션 처리가 되기 전, 수집된 JSON 메타데이터와 MeshMap 최종 병합
        MergeJsonDataToMeshMap();

        Asset->ClearCache();
        Asset->MarkAsGarbage();
        Asset = nullptr;
        UWorld *World = WorldContextObject->GetWorld();
        if (IsValid(World))
        {
            World->GetTimerManager().ClearTimer(ProcessTimerHandle);
        }
        FLoadAsyncWrapper Wrapper;
        Wrapper.NodeMap = MoveTemp(NodeMap);
        Wrapper.MeshMap = MoveTemp(MeshMap);
        Completed.Broadcast(Wrapper);
        SetReadyToDestroy();
    }
}

void ULoadAsyncAction::MergeJsonDataToMeshMap()
{
    if (LoadedJsonModelData.MeshData.Num() == 0) return;

    // 수집된 내부 가상 MeshMap 리스트를 순회하며 JSON 설정 정보 덮어쓰기 병합
    for (auto& Pair : MeshMap)
    {
        FName MeshName = Pair.Key;
        FModelMeshData& MeshData = Pair.Value;

        if (FRuntimeMeshData* FoundJsonData = LoadedJsonModelData.MeshData.Find(MeshName))
        {
            // 동등 연산 및 데이터 전체 복사 복사
            MeshData.Data = *FoundJsonData;
        }
    }
}

void ULoadAsyncAction::CalculateSize()
{
    FString Prefix = FStringHelper::GetTextBeforeChar(CurrentNode.Name, ';');
    FString Suffix = FStringHelper::GetTextAfterChar(CurrentNode.Name, ';');
    CurrentMeshName = FName(Prefix);
    FModelMeshData &Info = MeshMap.FindOrAdd(CurrentMeshName); // 존재하면 반환, 없으면 디폴트 생성 후 반환
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
            Info->Size = StaticMesh->GetBoundingBox().GetSize() * CurrentNode.Transform.GetScale3D();
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
    CurrentIndex++;
    CurrentMeshName = NAME_None;
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
    // 1. 백그라운드 스레드에서 작업 시작
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [ImagePath, this]()
    {
        TArray<uint8> RawFileData;
        if (!FFileHelper::LoadFileToArray(RawFileData, *ImagePath)) return;

        // 이미지 포맷 감지 및 디코딩
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

                // 2. 텍스처 생성은 반드시 게임 스레드에서 수행
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