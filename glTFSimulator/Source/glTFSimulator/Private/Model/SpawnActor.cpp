// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Model/SpawnActor.h"
#include "Model/StreamActor.h"
#include "System/FileFunctionLibrary.h"
#include "System/ActorHelper.h"
#include "System/MacroLibrary.h"
#include "Misc/OutputDeviceDebug.h"
#include "TimerManager.h"
#include "Model/LoadAsyncAction.h"
#include "glTFRuntimeFunctionLibrary.h"

#define MODEL_RUNTIME_DATA TEXT(".json")

FORCEINLINE int32 GetSize(float Value)
{
    return (Value < 1.0f) ? 1 : (int32)Value;
}

void ASpawnActor::Init(const FString &Path)
{
    FilePath = Path;
}

void ASpawnActor::BeginPlay()
{
    Super::BeginPlay();
    bIsLoaded = false;
    bAssetLoaded = false;
    AllNodeMap.Empty(); // Initialize Map
    // Runtime 플러그인의 델리게이트는 Dynamic Delegate이므로 BindDynamic 사용
    LoadAssetAsync();
}

void ASpawnActor::Destroyed()
{
    Super::Destroyed();
    bIsDestroyed = true;
    if (IsValid(glTFRuntimeAsset))
    {
        glTFRuntimeAsset->ClearCache();
        glTFRuntimeAsset->MarkAsGarbage();
        glTFRuntimeAsset = nullptr;
    }
}

void ASpawnActor::LoadAssetAsync()
{
    FglTFRuntimeHttpResponse Delegate;
    Delegate.BindDynamic(this, &ASpawnActor::OnAssetLoaded);
    FglTFRuntimeConfig Config;
    Config.bAllowExternalFiles = true;
    // bPathRelativeToContent = false 상태로 호출
    UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilenameAsync(FilePath, false, Config, Delegate);
}

FORCEINLINE int32 GetBatchSize(const int32 Total, const int32 Count)
{
    return FMath::Max(1, Total / Count); // 항상 1 이상 보장
}

void ASpawnActor::BatchNodeMap(const FLoadAsyncWrapper &Map)
{
    AllNodeMap = Map.NodeMap;
    AllMeshMap = Map.MeshMap;
    TArray<TPair<FName, FModelNodeData>> Pairs = AllNodeMap.Array();
    if (Pairs.Num() == 0)
        return;
    const int32 BatchSize = GetBatchSize(Pairs.Num(), BatchCount);
    // 1단계: 중복 빈도 계산
    TMap<FName, int32> PrefixCountMap;
    for (const TPair<FName, FModelNodeData> &Pair : Pairs)
    {
        const FName Name = Pair.Key;
        const FModelNodeData &Info = Pair.Value;
        PrefixCountMap.FindOrAdd(Name)++;
    }
    // 2단계: 빈도순으로 정렬된 노드 인덱스 배열 생성
    TArray<int32> SortedNodeIndices;
    SortedNodeIndices.SetNum(Pairs.Num());
    for (int32 i = 0; i < Pairs.Num(); i++)
    {
        SortedNodeIndices[i] = i;
    }
    // 빈도 높은 순으로 정렬 (동일 빈도일 때는 원본 순서 유지)
    SortedNodeIndices.Sort([this, &PrefixCountMap, &Pairs](int32 A, int32 B)
                           {
                                FName PrefixA = Pairs[A].Key;
                                FName PrefixB = Pairs[B].Key;

                                int32 CountA = PrefixCountMap[PrefixA];
                                int32 CountB = PrefixCountMap[PrefixB];

                                if (CountA != CountB)
                                    return CountA > CountB; // 빈도 높은 순

                                return A < B; // 동일 빈도시 원본 순서 유지
                           });
    // 3단계: 배치 생성 (중복 MeshIndex 재사용)
    TMap<FName, FModelNodeData> Batch;
    for (int32 SortedIndex : SortedNodeIndices)
    {
        const TPair<FName, FModelNodeData> Node = Pairs[SortedIndex];
        Batch.Add(Node.Key, Node.Value);
        // 배치 크기 초과시 스폰
        if (Batch.Num() >= BatchSize)
        {
            SpawnStreamActor(Batch);
            Batch.Empty();
        }
    }
    // 남은 배치 스폰
    if (Batch.Num() > 0)
    {
        SpawnStreamActor(Batch);
    }
    CheckLoadedAsync();
}

bool ASpawnActor::CheckAllStreamActorLoaded()
{
    int32 Count = StreamActors.Num();
    bool Result = Count > 0;
    float Percent = 0.0f;
    for (const AStreamActor *StreamActor : StreamActors)
    {
        bool Status = StreamActor->bIsLoaded;
        if (Status)
        {
            Percent += 1;
        }
        if (!Status && Result)
        {
            Result = false;
        }
    }
    LoadingStatus = (Percent / (float)Count);
    return Result;
}

void ASpawnActor::LoadRuntimeData()
{
    FString Path = UFileFunctionLibrary::GetPathWithoutExtension(FilePath);
    TSharedPtr<FJsonObject> Json = UFileFunctionLibrary::FromJson(Path.Append(MODEL_RUNTIME_DATA));
}

void ASpawnActor::CheckLoadedAsync()
{
    GetWorldTimerManager().SetTimerForNextTick(this, &ASpawnActor::CheckLoadedAsyncInternal);
}

void ASpawnActor::CheckLoadedAsyncInternal()
{
    if (CheckAllStreamActorLoaded())
    {
        GetWorldTimerManager().SetTimerForNextTick([this]() { bIsLoaded = true; });
    }
    else
    {
        bIsLoaded = false;
        CheckLoadedAsync();
    }
}

void ASpawnActor::OnAssetLoaded(UglTFRuntimeAsset* Asset)
{
    if (!IsValid(Asset))
    {
        return;
    }

    if (!bAssetLoaded)
    {
        bAssetLoaded = true;
        // 로드된 에셋 보관 (두 번째 로드 요청을 방지하기 위함)
        this->glTFRuntimeAsset = Asset;
        LoadAssetAsync();
    }
    else
    {
        int32 ChunkSize = Asset->GetNodes().Num();

        FglTFRuntimeStaticMeshConfig Config;
        Config.Outer = this;
        Config.CacheMode = EglTFRuntimeCacheMode::None;
        Config.MaterialsConfig.CacheMode = EglTFRuntimeCacheMode::None;
        Config.MaterialsConfig.bSkipLoad = true;
        Config.MaterialsConfig.bLoadMipMaps = false;
        Config.bAllowCPUAccess = false;
        Config.bBuildLumenCards = false;
        Config.bBuildNavCollision = false;
        Config.bBuildSimpleCollision = false;
        Config.bBuildComplexCollision = false;
        Config.NormalsGenerationStrategy = EglTFRuntimeNormalsGenerationStrategy::Never;
        Config.TangentsGenerationStrategy = EglTFRuntimeTangentsGenerationStrategy::Never;

        FString JsonPath = FPaths::ChangeExtension(FilePath, TEXT("json"));
        // Async Action 생성 및 호출
        ULoadAsyncAction *AsyncAction = ULoadAsyncAction::LoadAsync(this, Asset, Config, ChunkSize, JsonPath);
        if (AsyncAction)
        {
            AsyncAction->Completed.AddDynamic(this, &ASpawnActor::OnChunksLoaded);
            AsyncAction->Activate(); // 비동기 태스크 시작
        }
    }
}

void ASpawnActor::OnChunksLoaded(const FLoadAsyncWrapper &MapWrapper)
{
#if WITH_EDITOR
    UE_LOG(LogTemp, Warning, TEXT("ASpawnActor::OnChunksLoaded Executed."));
#endif
    // Blueprint에서는 Asset을 다시 로드했지만, C++에서는 캐싱된 Asset을 바로 넘김
    BatchNodeMap(MapWrapper);
}

void ASpawnActor::SpawnStreamActor(const TMap<FName, FModelNodeData>& Nodes)
{
    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = this;
    SpawnParams.Instigator = GetInstigator();
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::Undefined;

    // 만약 StreamActorClass가 BP에 설정되어 있지 않다면 기본 C++ 클래스를 사용
    UClass* SpawnClass = StreamActorClass ? StreamActorClass.Get() : AStreamActor::StaticClass();

    AStreamActor *StreamActor = 
    FActorHelper::SpawnActorDeferred<AStreamActor>(
        GetWorld(),
        SpawnClass,
        GetTransform(),
        SpawnParams);

    if (IsValid(StreamActor))
    {
        StreamActor->Init(this, Nodes);
        StreamActor->FinishSpawning(GetTransform());
        StreamActors.Emplace(StreamActor);
    }
}