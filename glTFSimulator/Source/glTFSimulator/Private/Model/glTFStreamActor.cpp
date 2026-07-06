// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Model/glTFStreamActor.h"

#include "Async/Async.h"
#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/ShapeComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture.h"
#include "glTFRuntimeFunctionLibrary.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeParser.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CoreMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Model/LoadAsyncAction.h"
#include "Model/StreamAsyncAction.h"
#include "System/AssetManageSubSystem.h"
#include "Modules/ModuleManager.h"
#include "System/FileFunctionLibrary.h"
#include "System/GameManagerSubSystem.h"
#include "System/MacroLibrary.h"
#include "TimerManager.h"

static constexpr int32 GltfStreamTextureDimensionLimit = 1024;

void AglTFStreamActor::Init(const FString& Path)
{
    FilePath = Path;
}

void AglTFStreamActor::BeginPlay()
{
    Super::BeginPlay();

    bIsLoaded = false;
    bIsDestroyed = false;
    bAsyncLoading = false;
    LoadingStatus = 0.0f;
    AssetLoadPhase = EGLTFStreamAssetPhase::None;
    ActiveSizeScanAction = nullptr;
    ActiveStreamAction = nullptr;
    CancelActiveAssetLoad();
    AssetLoadRequestSerial = 0;

    AllNodeMap.Empty();
    AllMeshMap.Empty();
    LoadedNodes.Empty();
    InstanceMap.Empty();
    UnloadBoxMap.Empty();
    DynamicComponentMap.Empty();
    ModelMetadata = FModelData();
    bHasModelMetadata = false;

    LoadAssetAsync(EGLTFStreamAssetPhase::SizeScan);
}

void AglTFStreamActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ReleaseRuntimeResourcesForWorldExit();
    Super::EndPlay(EndPlayReason);
}

void AglTFStreamActor::Destroyed()
{
    ReleaseRuntimeResourcesForWorldExit();
    Super::Destroyed();
}

void AglTFStreamActor::ReleaseRuntimeResourcesForWorldExit()
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearAllTimersForObject(this);
    }

    CancelActiveAssetLoad();
    CancelActiveAsyncActions();

    bIsDestroyed = true;
    bAsyncLoading = false;
    bIsLoaded = false;
    LoadingStatus = 1.0f;

    ReleaseStreamingResources();
    ReleaseAsset(glTFAsset.Get());
    glTFAsset = nullptr;

    AllNodeMap.Empty();
    AllMeshMap.Empty();
    LoadedNodes.Empty();
    InstanceMap.Empty();
    UnloadBoxMap.Empty();
    DynamicComponentMap.Empty();
    ModelMetadata = FModelData();
    bHasModelMetadata = false;
    FilePath.Reset();
    ActiveSizeScanAction = nullptr;
    ActiveStreamAction = nullptr;
    AssetLoadPhase = EGLTFStreamAssetPhase::None;
}

void AglTFStreamActor::LoadAssetAsync(EGLTFStreamAssetPhase Phase)
{
    CancelActiveAssetLoad();
    AssetLoadPhase = Phase;

    if (bIsDestroyed || FilePath.IsEmpty())
    {
        bAsyncLoading = false;
        return;
    }

    // Keep glTF parsing asynchronous while avoiding glTFRuntime's built-in filename async
    // helper. The plugin helper creates an internal unreferenced UglTFRuntimeAsset before
    // parsing completes, then calls SetParser() later on the game thread. During world
    // teardown that raw UObject can be collected. This path parses on a background thread and
    // creates/touches UObjects only after the actor, request serial, path, phase and cancel
    // token are still current on the game thread.
    const int32 RequestId = AssetLoadRequestSerial;
    const FString RequestedFilePath = FilePath;

    FglTFRuntimeConfig Config;
    Config.bAllowExternalFiles = true;

    TWeakObjectPtr<AglTFStreamActor> WeakThis(this);
    TSharedPtr<FThreadSafeCounter, ESPMode::ThreadSafe> CancelToken = MakeShared<FThreadSafeCounter, ESPMode::ThreadSafe>(0);
    ActiveAssetLoadCancelToken = CancelToken;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakThis, RequestedFilePath, Phase, RequestId, Config, CancelToken]()
    {
        if (!CancelToken.IsValid() || CancelToken->GetValue() != 0)
        {
            return;
        }

        TSharedPtr<FglTFRuntimeParser> Parser = FglTFRuntimeParser::FromFilename(RequestedFilePath, Config);

        if (!CancelToken.IsValid() || CancelToken->GetValue() != 0)
        {
            return;
        }

        AsyncTask(ENamedThreads::GameThread, [WeakThis, RequestedFilePath, Phase, RequestId, Config, Parser, CancelToken]()
        {
            AglTFStreamActor* StrongThis = WeakThis.Get();
            const bool bRequestStillCurrent =
                CancelToken.IsValid() &&
                CancelToken->GetValue() == 0 &&
                IsValid(StrongThis) &&
                !StrongThis->bIsDestroyed &&
                StrongThis->ActiveAssetLoadCancelToken == CancelToken &&
                StrongThis->AssetLoadRequestSerial == RequestId &&
                StrongThis->AssetLoadPhase == Phase &&
                StrongThis->FilePath == RequestedFilePath &&
                !IsGarbageCollecting();

            if (!bRequestStillCurrent)
            {
                return;
            }

            UglTFRuntimeAsset* LoadedAsset = nullptr;
            if (Parser.IsValid())
            {
                LoadedAsset = NewObject<UglTFRuntimeAsset>(StrongThis);
                if (LoadedAsset)
                {
                    LoadedAsset->RuntimeContextObject = Config.RuntimeContextObject;
                    LoadedAsset->RuntimeContextString = Config.RuntimeContextString;
                    if (!LoadedAsset->SetParser(Parser.ToSharedRef()))
                    {
                        StrongThis->ReleaseAsset(LoadedAsset);
                        LoadedAsset = nullptr;
                    }
                }
            }

            StrongThis = WeakThis.Get();
            if (!IsValid(StrongThis) || StrongThis->bIsDestroyed || StrongThis->ActiveAssetLoadCancelToken != CancelToken ||
                StrongThis->AssetLoadRequestSerial != RequestId || CancelToken->GetValue() != 0 ||
                StrongThis->AssetLoadPhase != Phase || StrongThis->FilePath != RequestedFilePath)
            {
                if (LoadedAsset)
                {
                    if (IsValid(StrongThis))
                    {
                        StrongThis->ReleaseAsset(LoadedAsset);
                    }
                    else
                    {
                        LoadedAsset->ClearCache();
                        LoadedAsset->MarkAsGarbage();
                    }
                }
                return;
            }

            StrongThis->ActiveAssetLoadCancelToken.Reset();
            StrongThis->OnAssetLoaded(LoadedAsset);
        });
    });
}

void AglTFStreamActor::OnAssetLoaded(UglTFRuntimeAsset* Asset)
{
    if (bIsDestroyed)
    {
        ReleaseAsset(Asset);
        return;
    }

    if (!IsValid(Asset))
    {
        UE_LOG(LogTemp, Warning, TEXT("AglTFStreamActor: failed to load asset: %s"), *FilePath);
        bAsyncLoading = false;
        return;
    }

    switch (AssetLoadPhase)
    {
        case EGLTFStreamAssetPhase::SizeScan:
            StartSizeScan(Asset);
            return;
        case EGLTFStreamAssetPhase::Streaming:
            glTFAsset = Asset;
            LoadingStatus = AllNodeMap.Num() == 0 ? 1.0f:
            0.5f;
            StartStreaming();
            return;
        default:
            break;
    }

    ReleaseAsset(Asset);
}

void AglTFStreamActor::StartSizeScan(UglTFRuntimeAsset* Asset)
{
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

    const int32 SizeScanChunkSize = GetSizeScanChunkSize(Asset->GetNodes().Num());
    const FString JsonPath = FPaths::ChangeExtension(FilePath, TEXT("json"));

    ULoadAsyncAction* AsyncAction = ULoadAsyncAction::LoadAsync(this, Asset, Config, SizeScanChunkSize, JsonPath);
    ActiveSizeScanAction = AsyncAction;
    if (AsyncAction)
    {
        AsyncAction->Completed.AddDynamic(this, &AglTFStreamActor::OnChunksLoaded);
        AsyncAction->Progress.AddDynamic(this, &AglTFStreamActor::OnSizeScanProgress);
        AsyncAction->Activate();
    }
    else
    {
        ActiveSizeScanAction = nullptr;
        ReleaseAsset(Asset);
    }
}

int32 AglTFStreamActor::GetSizeScanChunkSize(int32 TotalNodeCount) const
{
    if (TotalNodeCount <= 0)
    {
        return 1;
    }
    return FMath::Max(1, ChunkSize);
}

void AglTFStreamActor::OnChunksLoaded(const FLoadAsyncWrapper& MapWrapper)
{
    ActiveSizeScanAction = nullptr;

    if (bIsDestroyed)
    {
        return;
    }

#if WITH_EDITOR
    UE_LOG(LogTemp, Warning, TEXT("AglTFStreamActor::OnChunksLoaded Executed."));
#endif

    AllNodeMap = MapWrapper.NodeMap;
    AllMeshMap = MapWrapper.MeshMap;
    ModelMetadata = MapWrapper.ModelData;
    bHasModelMetadata = !ModelMetadata.Size.IsNearlyZero(0.001f);
    LoadingStatus = 0.5f;

    WriteLogAsync(FString::Printf(TEXT("Size scan completed. File=%s Center=%s Size=%s"),
        *FilePath,
        *ModelMetadata.Center.ToCompactString(),
        *ModelMetadata.Size.ToCompactString()));

    // The size-scan asset and its temporary meshes are no longer needed here.
    // ULoadAsyncAction already clears its local asset pointer after calculating bounds.
    if (IsPlayerInsideModelRange())
    {
        LoadAssetAsync(EGLTFStreamAssetPhase::Streaming);
    }
    else
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        WriteLogAsync(FString::Printf(TEXT("Streaming GLB load skipped because player is outside model range: %s"), *FilePath));
    }
}

void AglTFStreamActor::CancelActiveAssetLoad()
{
    if (ActiveAssetLoadCancelToken.IsValid())
    {
        ActiveAssetLoadCancelToken->Set(1);
        ActiveAssetLoadCancelToken.Reset();
    }

    ++AssetLoadRequestSerial;
}

void AglTFStreamActor::CancelActiveAsyncActions()
{
    if (IsValid(ActiveSizeScanAction.Get()))
    {
        ActiveSizeScanAction->CancelAndRelease();
    }
    ActiveSizeScanAction = nullptr;

    if (IsValid(ActiveStreamAction.Get()))
    {
        ActiveStreamAction->CancelAndRelease();
    }
    ActiveStreamAction = nullptr;
}

void AglTFStreamActor::ReleaseAsset(UglTFRuntimeAsset* Asset)
{
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
}

void AglTFStreamActor::ReleaseStreamingResources()
{
    UAssetManageSubSystem* AssetManager = UAssetManageSubSystem::Get(this);
    for (TPair<FName, TObjectPtr<UInstancedStaticMeshComponent>>& Pair : InstanceMap)
    {
        UInstancedStaticMeshComponent* ISMC = Pair.Value.Get();
        if (!IsValid(ISMC))
        {
            continue;
        }

        if (AssetManager)
        {
            AssetManager->ReleaseStaticMesh(this, ISMC->GetStaticMesh());
        }
        ISMC->ClearInstances();
        const int32 MaterialCount = ISMC->GetNumMaterials();
        for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
        {
            ISMC->SetMaterial(MaterialIndex, nullptr);
        }
        ISMC->SetStaticMesh(nullptr);
        ISMC->UnregisterComponent();
        ISMC->DestroyComponent();
    }
    InstanceMap.Empty();
    LoadedNodes.Empty();

    for (TPair<FName, TObjectPtr<UBoxComponent>>& Pair : UnloadBoxMap)
    {
        if (IsValid(Pair.Value))
        {
            Pair.Value->DestroyComponent();
        }
    }
    UnloadBoxMap.Empty();

    for (TPair<FName, FComponentGroup>& Pair : DynamicComponentMap)
    {
        for (UShapeComponent* Collider : Pair.Value.Colliders)
        {
            if (IsValid(Collider))
            {
                Collider->DestroyComponent();
            }
        }
        for (ULightComponent* Light : Pair.Value.Lights)
        {
            if (IsValid(Light))
            {
                Light->DestroyComponent();
            }
        }
    }
    DynamicComponentMap.Empty();
}

void AglTFStreamActor::OnSizeScanProgress(float Progress)
{
    LoadingStatus = FMath::Clamp(Progress * 0.5f, 0.0f, 0.5f);
}

void AglTFStreamActor::OnStreamAsyncProgress(float Progress)
{
    LoadingStatus = FMath::Clamp(0.5f + Progress * 0.5f, 0.5f, 1.0f);
}

bool AglTFStreamActor::IsPlayerInsideModelRange() const
{
    if (!bHasModelMetadata || ModelMetadata.Size.IsNearlyZero(0.001f))
    {
        return true;
    }

    FVector PlayerLocation = FVector::ZeroVector;
    if (UGameManagerSubSystem* GameSys = UGameManagerSubSystem::GetSubSystem(const_cast<AglTFStreamActor*>(this)))
    {
        PlayerLocation = GameSys->GetPlayerLocation();
    }

    constexpr float DistanceScale = 64.0f;
    const float Radius = FMath::Max3(ModelMetadata.Size.X, ModelMetadata.Size.Y, ModelMetadata.Size.Z) * DistanceScale;
    return FVector::DistSquared(PlayerLocation, ModelMetadata.Center) <= FMath::Square(FMath::Max(1.0f, Radius));
}

void AglTFStreamActor::WriteLogAsync(const FString& Message) const
{
    UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("glTFStreamActor"), Message);
}

void AglTFStreamActor::StartStreaming()
{
    if (AllNodeMap.Num() == 0)
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        return;
    }

    AsyncTick();
}

FglTFRuntimeStaticMeshConfig AglTFStreamActor::BuildStreamingStaticMeshConfig()
{
    FglTFRuntimeStaticMeshConfig Config;
    Config.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    Config.CollisionComplexity = ECollisionTraceFlag::CTF_UseComplexAsSimple;

    // Never inject null override materials. A null override can replace a valid glTF material and make textures disappear.
    TMap<EglTFRuntimeMaterialType, UMaterialInterface*> UberMaterialsOverrideMap;
    auto AddUberOverrideIfValid = [&UberMaterialsOverrideMap](EglTFRuntimeMaterialType Type, UMaterialInterface* Material)
    {
        if (IsValid(Material))
        {
            UberMaterialsOverrideMap.Add(Type, Material);
        }
    };

    AddUberOverrideIfValid(EglTFRuntimeMaterialType::Opaque, Default.Opaque.Get());
    AddUberOverrideIfValid(EglTFRuntimeMaterialType::Translucent, Default.Translucent.Get());
    AddUberOverrideIfValid(EglTFRuntimeMaterialType::TwoSided, Default.TwoSided.Get());
    AddUberOverrideIfValid(EglTFRuntimeMaterialType::TwoSidedTranslucent, Default.TranslucentTwoSided.Get());
    AddUberOverrideIfValid(EglTFRuntimeMaterialType::Masked, Default.Opaque.Get());
    AddUberOverrideIfValid(EglTFRuntimeMaterialType::TwoSidedMasked, Default.TwoSided.Get());

    Config.MaterialsConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    if (UberMaterialsOverrideMap.Num() > 0)
    {
        Config.MaterialsConfig.UberMaterialsOverrideMap = UberMaterialsOverrideMap;
        Config.MaterialsConfig.UnlitOverrideMap = UberMaterialsOverrideMap;
    }

    TMap<FString, UMaterialInterface*> MaterialsOverrideByNameMap;
    if (IsValid(Default.Glass.Get()))
    {
        MaterialsOverrideByNameMap.Add(TEXT("glass"), Default.Glass.Get());
    }
    if (IsValid(Default.TintedGlass.Get()))
    {
        MaterialsOverrideByNameMap.Add(TEXT("tinted_glass"), Default.TintedGlass.Get());
    }
    if (IsValid(Default.Terrain.Get()))
    {
        MaterialsOverrideByNameMap.Add(TEXT("terrain"), Default.Terrain.Get());
        MaterialsOverrideByNameMap.Add(TEXT("Terrain"), Default.Terrain.Get());
    }
    if (MaterialsOverrideByNameMap.Num() > 0)
    {
        Config.MaterialsConfig.MaterialsOverrideByNameMap = MaterialsOverrideByNameMap;
        Config.MaterialsConfig.bMaterialsOverrideMapInjectParams = true;
    }

    Config.MaterialsConfig.bGeneratesMipMaps = false;
    Config.MaterialsConfig.SpecularFactor = 0.0f;
    Config.MaterialsConfig.ImagesConfig.MaxWidth = GltfStreamTextureDimensionLimit;
    Config.MaterialsConfig.ImagesConfig.MaxHeight = GltfStreamTextureDimensionLimit;
    Config.MaterialsConfig.ImagesConfig.bCompressMips = false;
    Config.MaterialsConfig.ImagesConfig.bStreaming = false;
    Config.MaterialsConfig.bLoadMipMaps = false;
    Config.Outer = this;
    Config.bAllowCPUAccess = true;
    Config.bBuildLumenCards = false;
    Config.bBuildNavCollision = true;
    return Config;
}

void AglTFStreamActor::AsyncTick()
{
    if (bIsDestroyed)
    {
        bAsyncLoading = false;
        return;
    }

    if (!IsValid(glTFAsset))
    {
        bAsyncLoading = false;
        return;
    }

    if (AllNodeMap.Num() == 0)
    {
        bIsLoaded = true;
        LoadingStatus = 1.0f;
        bAsyncLoading = false;
        return;
    }

    if (bAsyncLoading)
    {
        return;
    }
    bAsyncLoading = true;

    FVector PlayerLoc = FVector::ZeroVector;
    if (UGameManagerSubSystem* GameSys = UGameManagerSubSystem::GetSubSystem(this))
    {
        PlayerLoc = GameSys->GetPlayerLocation();
    }

    const int32 Size = FMath::Max(1, ChunkSize);
    UStreamAsyncAction* AsyncAction = UStreamAsyncAction::StreamAsync(
        this,
        this,
        PlayerLoc,
        BuildStreamingStaticMeshConfig(),
        StreamDistance,
        Size);

    ActiveStreamAction = AsyncAction;
    if (AsyncAction)
    {
        AsyncAction->Completed.AddDynamic(this, &AglTFStreamActor::OnStreamAsyncCompleted);
        AsyncAction->Progress.AddDynamic(this, &AglTFStreamActor::OnStreamAsyncProgress);
        AsyncAction->Activate();
    }
    else
    {
        ActiveStreamAction = nullptr;
        bAsyncLoading = false;
    }
}

void AglTFStreamActor::UpdateProperties(const FStreamAsyncWrapper& Collection)
{
    AllNodeMap = Collection.NodeMap;
    LoadedNodes = Collection.LoadedNodes;
    InstanceMap = Collection.InstanceMap;
    UnloadBoxMap = Collection.UnloadBoxMap;
    DynamicComponentMap = Collection.DynamicComponentMap;
}

void AglTFStreamActor::OnStreamAsyncCompleted(const FStreamAsyncWrapper& MapWrapper)
{
    ActiveStreamAction = nullptr;

    if (bIsDestroyed)
    {
        return;
    }

    UpdateProperties(MapWrapper);
    bIsLoaded = true;
    bAsyncLoading = false;
    LoadingStatus = 1.0f;

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().SetTimerForNextTick(this, &AglTFStreamActor::AsyncTick);
    }
}
