// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Model/glTFStreamActor.h"
#include "Setting/GameSettings.h"

#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/ShapeComponent.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeParser.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CoreMisc.h"
#include "Misc/Paths.h"
#include "Model/LoadAsyncAction.h"
#include "Model/glTFMaterialOverrideUtils.h"
#include "Model/StreamAsyncAction.h"
#include "System/FileFunctionLibrary.h"
#include "System/GameManagerSubSystem.h"
#include "System/GameUpdateSubSystem.h"
#include "System/GlbValidation.h"
#include "System/SafeFileIO.h"
#include "System/glTFRuntimeSafety.h"
#include "TimerManager.h"
#include "World/WaterActor.h"

namespace
{
    /** Actor state, UObject creation, delegates, timers, and component teardown are game-thread-only. */
    bool EnsureStreamActorGameThread(const TCHAR* FunctionName)
    {
        return ensureMsgf(IsInGameThread(), TEXT("%s must run on the game thread"), FunctionName);
    }
}

void AglTFStreamActor::Init(const FString& Path)
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::Init")))
    {
        return;
    }

    FilePath = Path;
}

void AglTFStreamActor::SetRenderOnlyStreaming(bool bRenderOnly)
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::SetRenderOnlyStreaming")))
    {
        return;
    }

    bRenderOnlyStreaming = bRenderOnly;
}

void AglTFStreamActor::BeginPlay()
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::BeginPlay")))
    {
        return;
    }

    Super::BeginPlay();

    bIsLoaded = false;
    bIsDestroyed = false;
    bAsyncLoading = false;
    LoadingStatus = 0.0f;
    GameUpdateTickHandle = INDEX_NONE;
    AssetLoadPhase = EGLTFStreamAssetPhase::None;
    ActiveSizeScanAction = nullptr;
    ActiveStreamAction = nullptr;
    CancelActiveAssetLoad();
    AssetLoadRequestSerial = 0;

    AllNodeMap.Empty();
    AllMeshMap.Empty();
    WaterNodeMap.Empty();
    LoadedNodes.Empty();
    LoadedWaterNodes.Empty();
    InstanceMap.Empty();
    UnloadBoxMap.Empty();
    DynamicComponentMap.Empty();
    WaterActorMap.Empty();
    ModelMetadata = FModelData();
    bHasModelMetadata = false;

    // Normalize only on the game thread. The expensive JSON/accessor/mesh preflight runs inside
    // LoadAssetAsync's worker task so a large or malformed external GLB cannot freeze the viewport.
    FilePath = GlbValidation::NormalizePath(FilePath);
    LoadAssetAsync(EGLTFStreamAssetPhase::SizeScan);
}

void AglTFStreamActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::EndPlay")))
    {
        return;
    }

    ReleaseRuntimeResourcesForWorldExit();
    Super::EndPlay(EndPlayReason);
}

void AglTFStreamActor::Destroyed()
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::Destroyed")))
    {
        return;
    }

    ReleaseRuntimeResourcesForWorldExit();
    Super::Destroyed();
}

void AglTFStreamActor::ReleaseRuntimeResourcesForWorldExit()
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::ReleaseRuntimeResourcesForWorldExit")))
    {
        return;
    }

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearAllTimersForObject(this);
    }

    UnregisterGameUpdate();
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
    WaterNodeMap.Empty();
    LoadedNodes.Empty();
    LoadedWaterNodes.Empty();
    InstanceMap.Empty();
    UnloadBoxMap.Empty();
    DynamicComponentMap.Empty();
    WaterActorMap.Empty();
    ModelMetadata = FModelData();
    bHasModelMetadata = false;
    FilePath.Reset();
    ActiveSizeScanAction = nullptr;
    ActiveStreamAction = nullptr;
    GameUpdateTickHandle = INDEX_NONE;
    AssetLoadPhase = EGLTFStreamAssetPhase::None;
}

void AglTFStreamActor::LoadAssetAsync(EGLTFStreamAssetPhase Phase)
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::LoadAssetAsync")))
    {
        return;
    }

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
    // teardown that raw UObject can be collected. Project code parses on a background thread, then
    // creates/touches UObjects only after the actor, request serial, path, phase and cancel token
    // are still current on the game thread. glTFRuntime itself may marshal internal engine-object
    // setup to the game thread while its parser constructor runs.
    const int32 RequestId = AssetLoadRequestSerial;
    const FString RequestedFilePath = FilePath;

    FglTFRuntimeConfig Config;
    Config.bAllowExternalFiles = true;

    TWeakObjectPtr<AglTFStreamActor> WeakThis(this);
    TSharedPtr<FThreadSafeCounter, ESPMode::ThreadSafe> CancelToken = MakeShared<FThreadSafeCounter, ESPMode::ThreadSafe>(0);
    ActiveAssetLoadCancelToken = CancelToken;

    const bool bWorkerQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, RequestedFilePath, Phase, RequestId, Config, CancelToken]()
    {
        if (!CancelToken.IsValid() || CancelToken->GetValue() != 0)
        {
            return;
        }

        FString FailureReason;
        TSharedPtr<FglTFRuntimeParser> Parser;

        // Validate every untrusted allocation-driving field before entering glTFRuntime. This check
        // rejects accessor, primitive, vertex, and index totals that could overflow an int32 or ask
        // TArray to allocate multiple gigabytes, which is the exact class of failure seen in
        // FglTFRuntimeParser::LoadStaticMesh_Internal.
        if (!GlbValidation::ValidateRuntimeMeshFile(RequestedFilePath, FailureReason))
        {
            FailureReason = FString::Printf(TEXT("runtime mesh preflight failed: %s"), *FailureReason);
        }
        else
        {
            // Each GLB receives an independent parser. Construction is bounded rather than globally
            // serialized, allowing several files to preflight/parse concurrently without unbounded peaks.
            Parser = FglTFRuntimeSafety::CreateParserSafely(RequestedFilePath, Config);
            if (!Parser.IsValid())
            {
                FailureReason = TEXT("glTFRuntime parser creation was rejected or failed");
            }
        }

        if (!CancelToken.IsValid() || CancelToken->GetValue() != 0)
        {
            return;
        }

        if (!FSafeFileIO::DispatchTrackedGameThread(
            [WeakThis, RequestedFilePath, Phase, RequestId, Config, Parser, FailureReason, CancelToken]()
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
                LoadedAsset = NewObject<UglTFRuntimeAsset>(GetTransientPackage(), NAME_None, RF_Transient);
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
                        FglTFRuntimeSafety::RequestAssetRelease(LoadedAsset);
                    }
                }
                return;
            }

            StrongThis->ActiveAssetLoadCancelToken.Reset();
            if (!LoadedAsset && !FailureReason.IsEmpty())
            {
                // Isolate this external file without terminating the process. A later streaming pass
                // may retry only after the source file changes or the actor is recreated.
                FglTFRuntimeSafety::ReportRecoverableFailure(RequestedFilePath, FailureReason);
                StrongThis->WriteLogAsync(FString::Printf(
                    TEXT("Model GLB was isolated before static-mesh construction. Path=%s Reason=%s"),
                    *RequestedFilePath,
                    *FailureReason));
            }
            StrongThis->OnAssetLoaded(LoadedAsset);
        }))
        {
            // Shutdown rejected the continuation; cancel only the thread-safe request token.
            CancelToken->Set(1);
        }
    });

    if (!bWorkerQueued)
    {
        // The request never left the game thread, so its local actor state can be unwound safely.
        CancelToken->Set(1);
        if (ActiveAssetLoadCancelToken == CancelToken)
        {
            ActiveAssetLoadCancelToken.Reset();
        }
        bAsyncLoading = false;
        AssetLoadPhase = EGLTFStreamAssetPhase::None;
    }
}

void AglTFStreamActor::OnAssetLoaded(UglTFRuntimeAsset* Asset)
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::OnAssetLoaded")))
    {
        return;
    }

    if (bIsDestroyed)
    {
        ReleaseAsset(Asset);
        return;
    }

    if (!IsValid(Asset))
    {
        UE_LOG(LogTemp, Warning, TEXT("AglTFStreamActor: failed to load asset: %s"), *FilePath);
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        WriteLogAsync(FString::Printf(TEXT("Model GLB parser failed; the file was isolated: %s"), *FilePath));
        return;
    }

    switch (AssetLoadPhase)
    {
        case EGLTFStreamAssetPhase::SizeScan:
            StartSizeScan(Asset);
            return;
        case EGLTFStreamAssetPhase::Streaming:
            glTFAsset = Asset;
            LoadingStatus = (AllNodeMap.Num() + WaterNodeMap.Num()) == 0 ? 1.0f : 0.5f;
            StartStreaming();
            return;
        default:
            break;
    }

    ReleaseAsset(Asset);
}

void AglTFStreamActor::StartSizeScan(UglTFRuntimeAsset* Asset)
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::StartSizeScan")))
    {
        return;
    }

    FglTFRuntimeStaticMeshConfig Config;
    Config.Outer = this;
    // The probe deliberately uses None: it skips materials/normals/tangents and must not poison
    // the later full-quality ReadWrite cache with a differently configured temporary mesh.
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
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::OnChunksLoaded")))
    {
        return;
    }

    ActiveSizeScanAction = nullptr;

    if (bIsDestroyed)
    {
        return;
    }

#if WITH_EDITOR
    UE_LOG(LogTemp, Verbose, TEXT("AglTFStreamActor::OnChunksLoaded completed."));
#endif

    AllNodeMap = MapWrapper.NodeMap;
    WaterNodeMap = MapWrapper.WaterNodeMap;
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
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::CancelActiveAssetLoad")))
    {
        return;
    }

    if (ActiveAssetLoadCancelToken.IsValid())
    {
        ActiveAssetLoadCancelToken->Set(1);
        ActiveAssetLoadCancelToken.Reset();
    }

    ++AssetLoadRequestSerial;
}

void AglTFStreamActor::CancelActiveAsyncActions()
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::CancelActiveAsyncActions")))
    {
        return;
    }

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
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::ReleaseAsset")))
    {
        return;
    }

    if (IsValid(Asset))
    {
        // A final release is deferred while this parser still has native work in flight. The
        // coordinator holds a strong reference, clears ReadWrite caches on the game thread, then
        // removes any legacy root/standalone flags exactly once.
        FglTFRuntimeSafety::RequestAssetRelease(Asset);
    }
}

void AglTFStreamActor::ReleaseStreamingResources()
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::ReleaseStreamingResources")))
    {
        return;
    }

    for (TPair<FName, TObjectPtr<UInstancedStaticMeshComponent>>& Pair : InstanceMap)
    {
        UInstancedStaticMeshComponent* ISMC = Pair.Value.Get();
        if (!IsValid(ISMC))
        {
            continue;
        }

        // Components release their references; reusable meshes remain owned solely by the
        // glTFRuntimeAsset ReadWrite cache until ReleaseAsset reaches its cache barrier.
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

    for (TPair<FName, TObjectPtr<AWaterActor>>& Pair : WaterActorMap)
    {
        if (IsValid(Pair.Value.Get()))
        {
            Pair.Value->Destroy();
        }
    }
    WaterActorMap.Empty();
    LoadedWaterNodes.Empty();
}


void AglTFStreamActor::OnSizeScanProgress(float Progress)
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::OnSizeScanProgress")))
    {
        return;
    }

    LoadingStatus = FMath::Clamp(Progress * 0.5f, 0.0f, 0.5f);
}

void AglTFStreamActor::OnStreamAsyncProgress(float Progress)
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::OnStreamAsyncProgress")))
    {
        return;
    }

    LoadingStatus = FMath::Clamp(0.5f + Progress * 0.5f, 0.5f, 1.0f);
}

bool AglTFStreamActor::IsPlayerInsideModelRange() const
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::IsPlayerInsideModelRange")))
    {
        return false;
    }

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
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::StartStreaming")))
    {
        return;
    }

    if (AllNodeMap.Num() == 0 && WaterNodeMap.Num() == 0)
    {
        bIsLoaded = true;
        bAsyncLoading = false;
        LoadingStatus = 1.0f;
        return;
    }

    RegisterGameUpdate();
    StartStreamingStep();
}

void AglTFStreamActor::RegisterGameUpdate()
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::RegisterGameUpdate")))
    {
        return;
    }

    if (GameUpdateTickHandle != INDEX_NONE)
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [this](const float DeltaSeconds)
            {
                UpdateStreamingFromGameUpdate(DeltaSeconds);
            },
            15);
    }
}

void AglTFStreamActor::UnregisterGameUpdate()
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::UnregisterGameUpdate")))
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
    }
    GameUpdateTickHandle = INDEX_NONE;
}

void AglTFStreamActor::UpdateStreamingFromGameUpdate(float DeltaSeconds)
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::UpdateStreamingFromGameUpdate")))
    {
        return;
    }

    StartStreamingStep();
}

FglTFRuntimeStaticMeshConfig AglTFStreamActor::BuildStreamingStaticMeshConfig()
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::BuildStreamingStaticMeshConfig")))
    {
        return FglTFRuntimeStaticMeshConfig();
    }

    FglTFRuntimeStaticMeshConfig Config;
    // One UglTFRuntimeAsset now owns the authoritative mesh/material cache for this GLB.
    Config.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    Config.CollisionComplexity = ECollisionTraceFlag::CTF_UseComplexAsSimple;

    // One canonical editor-assigned material set. glTF-internal material names remain valid
    // runtime metadata through Materials.ByMaterialName. No Unreal asset is found by name/path.
    const FglTFMaterialAssetReferences& MaterialReferences = Default.Materials;
    const TMap<EglTFRuntimeMaterialType, UMaterialInterface*> MaterialOverrides =
        glTFMaterialOverrideUtils::BuildOverrideMap(MaterialReferences);
    Config.MaterialsConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    if (MaterialOverrides.Num() > 0)
    {
        Config.MaterialsConfig.UberMaterialsOverrideMap = MaterialOverrides;
        Config.MaterialsConfig.UnlitOverrideMap = MaterialOverrides;
    }

    // Named overrides are keyed only by names stored inside the imported glTF document.
    // Parameter injection preserves base-color/normal/ORM/emissive textures on the selected base material.
    glTFMaterialOverrideUtils::ApplyNamedOverrides(MaterialReferences, Config.MaterialsConfig);

    Config.MaterialsConfig.bGeneratesMipMaps = true;
    Config.MaterialsConfig.SpecularFactor = 0.0f;
    const int32 TextureDimensionLimit = UGameSettings::ResolveMaxTextureResolution(this);
    Config.MaterialsConfig.ImagesConfig.MaxWidth = TextureDimensionLimit;
    Config.MaterialsConfig.ImagesConfig.MaxHeight = TextureDimensionLimit;
    Config.MaterialsConfig.ImagesConfig.bCompressMips = true;
    Config.MaterialsConfig.ImagesConfig.bStreaming = true;
    Config.MaterialsConfig.bLoadMipMaps = true;
    Config.Outer = this;
    Config.bAllowCPUAccess = true;
    // Single-player and listen-server worlds need runtime lighting cards and nav collision.
    // Client render-only streaming skips them because authority/collision lives on the server.
    Config.bBuildLumenCards = true;
    Config.bBuildNavCollision = !bRenderOnlyStreaming;
    if (bRenderOnlyStreaming)
    {
        Config.CollisionComplexity = ECollisionTraceFlag::CTF_UseDefault;
        Config.bBuildComplexCollision = false;
        Config.bBuildSimpleCollision = false;
    }
    return Config;
}

void AglTFStreamActor::StartStreamingStep()
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::StartStreamingStep")))
    {
        return;
    }

    if (bIsDestroyed)
    {
        bAsyncLoading = false;
        UnregisterGameUpdate();
        return;
    }

    if (!IsValid(glTFAsset))
    {
        bAsyncLoading = false;
        UnregisterGameUpdate();
        return;
    }

    if (AllNodeMap.Num() == 0 && WaterNodeMap.Num() == 0)
    {
        bIsLoaded = true;
        LoadingStatus = 1.0f;
        bAsyncLoading = false;
        UnregisterGameUpdate();
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
        Size,
        bRenderOnlyStreaming);

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
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::UpdateProperties")))
    {
        return;
    }

    AllNodeMap = Collection.NodeMap;
    WaterNodeMap = Collection.WaterNodeMap;
    LoadedNodes = Collection.LoadedNodes;
    LoadedWaterNodes = Collection.LoadedWaterNodes;
    InstanceMap = Collection.InstanceMap;
    UnloadBoxMap = Collection.UnloadBoxMap;
    DynamicComponentMap = Collection.DynamicComponentMap;
    WaterActorMap = Collection.WaterActorMap;
}

void AglTFStreamActor::OnStreamAsyncCompleted(const FStreamAsyncWrapper& MapWrapper)
{
    if (!EnsureStreamActorGameThread(TEXT("AglTFStreamActor::OnStreamAsyncCompleted")))
    {
        return;
    }

    ActiveStreamAction = nullptr;

    if (bIsDestroyed)
    {
        return;
    }

    UpdateProperties(MapWrapper);
    bIsLoaded = true;
    bAsyncLoading = false;
    LoadingStatus = 1.0f;

}
