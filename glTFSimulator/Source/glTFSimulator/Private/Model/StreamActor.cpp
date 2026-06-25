// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Model/StreamActor.h"
#include "System/GameManagerSubSystem.h"
#include "Engine/StaticMesh.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "glTFRuntimeAsset.h"
#include "Model/SpawnActor.h"
#include "Misc/OutputDeviceDebug.h"
#include "Model/StreamAsyncAction.h" // UStreamAsyncAction 클래스 경로에 맞게 수정
#include "TimerManager.h"            // SetTimerForNextTick 사용
#include "Engine/Texture2DArray.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"


static bool IsTerrainLikeName(const FString& Name)
{
    return Name.Equals(TEXT("terrain"), ESearchCase::IgnoreCase)
        || Name.Contains(TEXT("terrain"), ESearchCase::IgnoreCase);
}

static bool LoadTerrainTextureSlice(const FString& FilePath, int32& OutWidth, int32& OutHeight, TArray<uint8>& OutPixels)
    {
        TArray<uint8> CompressedData;
        if (!FFileHelper::LoadFileToArray(CompressedData, *FilePath))
        {
            return false;
        }

        IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName(TEXT("ImageWrapper")));
        const EImageFormat DetectedFormat = ImageWrapperModule.DetectImageFormat(CompressedData.GetData(), CompressedData.Num());
        if (DetectedFormat == EImageFormat::Invalid)
        {
            UE_LOG(LogTemp, Warning, TEXT("[TerrainTextureArray] Unsupported image format: %s"), *FilePath);
            return false;
        }

        TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(DetectedFormat);
        if (!ImageWrapper.IsValid() || !ImageWrapper->SetCompressed(CompressedData.GetData(), CompressedData.Num()))
        {
            UE_LOG(LogTemp, Warning, TEXT("[TerrainTextureArray] Could not decode image: %s"), *FilePath);
            return false;
        }

        TArray<uint8> BGRA;
        if (!ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, BGRA))
        {
            UE_LOG(LogTemp, Warning, TEXT("[TerrainTextureArray] Could not unpack image pixels: %s"), *FilePath);
            return false;
        }

        OutWidth = ImageWrapper->GetWidth();
        OutHeight = ImageWrapper->GetHeight();
        OutPixels = MoveTemp(BGRA);
        return OutWidth > 0 && OutHeight > 0 && OutPixels.Num() == OutWidth * OutHeight * 4;
    }

static UTexture2DArray* BuildTerrainTextureArrayFromProjectPngs(UObject* Outer)
    {
        const FString LowercaseDirectory = FPaths::Combine(FPaths::ProjectDir(), TEXT("textures"), TEXT("terrain"));
        const FString TitlecaseDirectory = FPaths::Combine(FPaths::ProjectDir(), TEXT("Textures"), TEXT("terrain"));
        const FString TerrainDirectory = IFileManager::Get().DirectoryExists(*LowercaseDirectory) ? LowercaseDirectory : TitlecaseDirectory;

        if (!IFileManager::Get().DirectoryExists(*TerrainDirectory))
        {
            UE_LOG(LogTemp, Verbose, TEXT("[TerrainTextureArray] Folder not found: %s"), *TerrainDirectory);
            return nullptr;
        }

        TArray<TArray<uint8>> Slices;
        int32 ExpectedWidth = 0;
        int32 ExpectedHeight = 0;

        for (int32 Index = 0; Index < 512; ++Index)
        {
            const FString FilePath = FPaths::Combine(TerrainDirectory, FString::Printf(TEXT("%d.png"), Index));
            if (!FPaths::FileExists(FilePath))
            {
                if (Index == 0)
                {
                    UE_LOG(LogTemp, Verbose, TEXT("[TerrainTextureArray] No numbered PNG terrain textures found in %s"), *TerrainDirectory);
                }
                break;
            }

            int32 Width = 0;
            int32 Height = 0;
            TArray<uint8> Pixels;
            if (!LoadTerrainTextureSlice(FilePath, Width, Height, Pixels))
            {
                continue;
            }

            if (Slices.Num() == 0)
            {
                ExpectedWidth = Width;
                ExpectedHeight = Height;
            }
            else if (Width != ExpectedWidth || Height != ExpectedHeight)
            {
                UE_LOG(LogTemp, Warning, TEXT("[TerrainTextureArray] Skipped %s because all slices must share size %dx%d. Got %dx%d."), *FilePath, ExpectedWidth, ExpectedHeight, Width, Height);
                continue;
            }

            Slices.Add(MoveTemp(Pixels));
        }

        if (Slices.Num() == 0)
        {
            return nullptr;
        }

        UTexture2DArray* TextureArray = NewObject<UTexture2DArray>(Outer ? Outer : GetTransientPackage(), NAME_None, RF_Public | RF_Transient);
        if (!TextureArray)
        {
            return nullptr;
        }

        FTexturePlatformData* PlatformData = new FTexturePlatformData();
        PlatformData->SizeX = ExpectedWidth;
        PlatformData->SizeY = ExpectedHeight;
        PlatformData->PixelFormat = PF_B8G8R8A8;
        PlatformData->SetNumSlices(Slices.Num());

#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
        TextureArray->SetPlatformData(PlatformData);
#else
        TextureArray->PlatformData = PlatformData;
#endif

        TextureArray->NeverStream = true;
        TextureArray->SRGB = true;
        TextureArray->CompressionSettings = TextureCompressionSettings::TC_Default;
        TextureArray->Filter = TextureFilter::TF_Default;
#if ENGINE_MAJOR_VERSION >= 5 || ENGINE_MINOR_VERSION > 25 || WITH_EDITOR
        TextureArray->AddressX = TextureAddress::TA_Wrap;
        TextureArray->AddressY = TextureAddress::TA_Wrap;
        TextureArray->AddressZ = TextureAddress::TA_Wrap;
#endif

        FTexture2DMipMap* Mip = new FTexture2DMipMap();
        PlatformData->Mips.Add(Mip);
        Mip->SizeX = ExpectedWidth;
        Mip->SizeY = ExpectedHeight;
        Mip->SizeZ = Slices.Num();

        const int64 SliceBytes = static_cast<int64>(ExpectedWidth) * static_cast<int64>(ExpectedHeight) * 4;
        Mip->BulkData.Lock(LOCK_READ_WRITE);
        void* Data = Mip->BulkData.Realloc(SliceBytes * Slices.Num());
        for (int32 SliceIndex = 0; SliceIndex < Slices.Num(); ++SliceIndex)
        {
            FMemory::Memcpy(reinterpret_cast<uint8*>(Data) + SliceBytes * SliceIndex, Slices[SliceIndex].GetData(), SliceBytes);
        }
        Mip->BulkData.Unlock();

        TextureArray->UpdateResource();
        UE_LOG(LogTemp, Display, TEXT("[TerrainTextureArray] Loaded %d terrain texture slices from %s"), Slices.Num(), *TerrainDirectory);
        return TextureArray;
    }

void AStreamActor::Init(ASpawnActor *InActor, const TMap<FName, FModelNodeData> &Nodes)
{
    NodeMap.Empty();     // Initialize Map
    LoadedNodes.Empty(); // Initialize Map
    InstanceMap.Empty();
    UnloadBoxMap.Empty();
    DynamicComponentMap.Empty(); // Initialize Map
    SpawnActor = InActor;
    NodeMap = Nodes;
}

void AStreamActor::BeginPlay()
{
    Super::BeginPlay();
    bIsLoaded = false;
    if (IsValid(SpawnActor))
    {
        Asset = SpawnActor->GetAsset(); // 혹은 Actor->glTFAsset (SpawnActor 구현에 따라)
    }
    AsyncTick();
}

void AStreamActor::UpdateProperties(FStreamAsyncWrapper Collection)
{
    NodeMap = Collection.NodeMap;
    LoadedNodes = Collection.LoadedNodes;
    InstanceMap = Collection.InstanceMap;
    UnloadBoxMap = Collection.UnloadBoxMap;
    DynamicComponentMap = Collection.DynamicComponentMap;
}

void AStreamActor::AsyncTick()
{
    // Blueprint의 'CheckDestroyed' 매크로 로직
    if (IsValid(SpawnActor) && SpawnActor->GetIsDestroyed()) // SpawnActor에 bIsDestroyed가 있다고 가정
    {
        bAsyncLoading = false;
        return;
    }

    if (!IsValid(Asset) || NodeMap.Num() == 0)
    {
        bAsyncLoading = false;
        return;
    }

    if (bAsyncLoading)
    {
        return;
    }
    bAsyncLoading = true;

    // Blueprint의 'glTFStreamAsyncTick' 매크로 로직 (서브시스템에서 PlayerLocation 획득)
    FVector PlayerLoc = FVector::ZeroVector;
    if (UGameManagerSubSystem *GameSys = UGameManagerSubSystem::GetSubSystem(this))
    {
        PlayerLoc = GameSys->GetPlayerLocation();
    }
    const int32 Size = bIsLoaded ? ChunkSize : NodeMap.Num();
    // 비동기 스트림 액션 실행
    // 매개변수 순서와 이름은 UStreamAsyncAction::StreamAsync 정적 함수 시그니처에 맞게 조정 필요

    FglTFRuntimeStaticMeshConfig Config;
    Config.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    Config.CollisionComplexity = ECollisionTraceFlag::CTF_UseComplexAsSimple;
    TMap<EglTFRuntimeMaterialType, UMaterialInterface *> UberMaterialsOverrideMap;
    UberMaterialsOverrideMap.Add(EglTFRuntimeMaterialType::Opaque, Default.Opaque);
    UberMaterialsOverrideMap.Add(EglTFRuntimeMaterialType::Translucent, Default.Translucent);
    UberMaterialsOverrideMap.Add(EglTFRuntimeMaterialType::TwoSided, Default.TwoSided);
    UberMaterialsOverrideMap.Add(EglTFRuntimeMaterialType::TwoSidedTranslucent, Default.TranslucentTwoSided);
    UberMaterialsOverrideMap.Add(EglTFRuntimeMaterialType::Masked, Default.Opaque);
    UberMaterialsOverrideMap.Add(EglTFRuntimeMaterialType::TwoSidedMasked, Default.TwoSided);
    Config.MaterialsConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    Config.MaterialsConfig.UberMaterialsOverrideMap = UberMaterialsOverrideMap;
    Config.MaterialsConfig.UnlitOverrideMap = UberMaterialsOverrideMap;
    TMap<FString, UMaterialInterface *> MaterialsOverrideByNameMap;
    MaterialsOverrideByNameMap.Add(TEXT("glass"), Default.Glass);
    MaterialsOverrideByNameMap.Add(TEXT("tinted_glass"), Default.TintedGlass);
    MaterialsOverrideByNameMap.Add(TEXT("terrain"), Default.Terrain);
    MaterialsOverrideByNameMap.Add(TEXT("Terrain"), Default.Terrain);
    Config.MaterialsConfig.MaterialsOverrideByNameMap = MaterialsOverrideByNameMap;
    Config.MaterialsConfig.bMaterialsOverrideMapInjectParams = true;
    if (!IsValid(RuntimeTerrainTextureArray))
    {
        RuntimeTerrainTextureArray = BuildTerrainTextureArrayFromProjectPngs(this);
    }
    if (RuntimeTerrainTextureArray)
    {
        Config.MaterialsConfig.CustomTextureParams.Add(TEXT("TerrainTextures"), RuntimeTerrainTextureArray);
    }
    Config.MaterialsConfig.bGeneratesMipMaps = true;
    Config.MaterialsConfig.SpecularFactor = 0.0f;
    Config.MaterialsConfig.ImagesConfig.MaxWidth = 768;
    Config.MaterialsConfig.ImagesConfig.MaxHeight = 768;
    Config.MaterialsConfig.ImagesConfig.bCompressMips = true;
    Config.MaterialsConfig.ImagesConfig.bStreaming = true;
    Config.MaterialsConfig.bLoadMipMaps = true;
    Config.Outer = this;
    Config.bAllowCPUAccess = true;
    Config.bBuildLumenCards = true;
    Config.bBuildNavCollision = true;

    UStreamAsyncAction *AsyncAction = UStreamAsyncAction::StreamAsync(
        this,
        this,
        PlayerLoc,
        Config,
        StreamDistance,
        Size);
    if (AsyncAction)
    {
        AsyncAction->Completed.AddDynamic(this, &AStreamActor::OnStreamAsyncCompleted);
        AsyncAction->Activate();
    }
    else
    {
        bAsyncLoading = false;
    }
}

bool AStreamActor::IsTerrainMaterial(const UMaterialInterface* Material) const
{
    const UMaterialInterface* CurrentMaterial = Material;
    while (CurrentMaterial)
    {
        if (CurrentMaterial == Default.Terrain.Get() || IsTerrainLikeName(CurrentMaterial->GetName()))
        {
            return true;
        }

        const UMaterialInstance* MaterialInstance = Cast<UMaterialInstance>(CurrentMaterial);
        CurrentMaterial = MaterialInstance ? MaterialInstance->Parent : nullptr;
    }

    return false;
}

void AStreamActor::ApplyTerrainTextureArrayToLoadedMaterials()
{
    if (!RuntimeTerrainTextureArray)
    {
        return;
    }

    for (const TPair<FName, TObjectPtr<UInstancedStaticMeshComponent>>& Pair : InstanceMap)
    {
        UInstancedStaticMeshComponent* MeshComponent = Pair.Value.Get();
        if (!IsValid(MeshComponent))
        {
            continue;
        }

        const bool bMeshNameLooksTerrain = IsTerrainLikeName(Pair.Key.ToString());
        const int32 MaterialCount = MeshComponent->GetNumMaterials();
        for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
        {
            UMaterialInterface* Material = MeshComponent->GetMaterial(MaterialIndex);
            if (!bMeshNameLooksTerrain && !IsTerrainMaterial(Material))
            {
                continue;
            }

            UMaterialInstanceDynamic* DynamicMaterial = Cast<UMaterialInstanceDynamic>(Material);
            if (!DynamicMaterial)
            {
                UMaterialInterface* ParentMaterial = Material ? Material : Default.Terrain.Get();
                if (!ParentMaterial)
                {
                    continue;
                }
                DynamicMaterial = UMaterialInstanceDynamic::Create(ParentMaterial, this);
                if (DynamicMaterial)
                {
                    MeshComponent->SetMaterial(MaterialIndex, DynamicMaterial);
                }
            }

            if (DynamicMaterial)
            {
                DynamicMaterial->SetTextureParameterValue(TEXT("baseColor"), RuntimeTerrainTextureArray);
                DynamicMaterial->SetTextureParameterValue(TEXT("BaseColor"), RuntimeTerrainTextureArray);
            }
        }
    }
}

void AStreamActor::OnStreamAsyncCompleted(const FStreamAsyncWrapper &MapWrapper)
{
    // Completed 핀 로직 수행
    UpdateProperties(MapWrapper);
    ApplyTerrainTextureArrayToLoadedMaterials();
    bIsLoaded = true;
    bAsyncLoading = false;
    // Blueprint의 'DelayUntilNextTick' 후 다시 'AsyncTick' 호출
    GetWorld()->GetTimerManager().SetTimerForNextTick(this, &AStreamActor::AsyncTick);
}
