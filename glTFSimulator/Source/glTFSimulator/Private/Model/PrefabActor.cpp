// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "World/PrefabActor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Model/glTFMaterialOverrideUtils.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeFunctionLibrary.h"
#include "glTFRuntimeParser.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Setting/GameSettings.h"
#include "System/MultiplayerWorldSubSystem.h"
#include "System/MacroLibrary.h"
#include "Net/UnrealNetwork.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{

    static FString ResolveReplicatedGltfPathForCurrentWorld(const UObject* WorldContextObject, const FString& InPath)
    {
        FString NormalizedPath = InPath;
        FPaths::NormalizeFilename(NormalizedPath);
        if (FPaths::FileExists(NormalizedPath))
        {
            return NormalizedPath;
        }

        FString RelativeModelPath = FPaths::GetCleanFilename(NormalizedPath);
        const int32 ModelSegmentIndex = NormalizedPath.Find(TEXT("/model/"), ESearchCase::IgnoreCase, ESearchDir::FromStart);
        if (ModelSegmentIndex != INDEX_NONE)
        {
            RelativeModelPath = NormalizedPath.Mid(ModelSegmentIndex + 7);
        }

        if (const UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(WorldContextObject))
        {
            const FString WorldFolderName = Multiplayer->GetSelectedWorldFolderName();
            if (!WorldFolderName.IsEmpty())
            {
                const FString Candidate = FPaths::Combine(PATH_ROOT, WorldFolderName, TEXT("model"), RelativeModelPath);
                if (FPaths::FileExists(Candidate))
                {
                    return Candidate;
                }
                return Candidate;
            }
        }

        return NormalizedPath;
    }

    static bool ReadTransformObject(const TSharedPtr<FJsonObject>& Object, FTransform& OutTransform)
    {
        if (!Object.IsValid())
        {
            return false;
        }

        double X = OutTransform.GetLocation().X;
        double Y = OutTransform.GetLocation().Y;
        double Z = OutTransform.GetLocation().Z;
        double Pitch = OutTransform.Rotator().Pitch;
        double Yaw = OutTransform.Rotator().Yaw;
        double Roll = OutTransform.Rotator().Roll;
        double ScaleX = OutTransform.GetScale3D().X;
        double ScaleY = OutTransform.GetScale3D().Y;
        double ScaleZ = OutTransform.GetScale3D().Z;
        double UniformScale = ScaleX;

        Object->TryGetNumberField(TEXT("X"), X);
        Object->TryGetNumberField(TEXT("Y"), Y);
        Object->TryGetNumberField(TEXT("Z"), Z);
        Object->TryGetNumberField(TEXT("Pitch"), Pitch);
        Object->TryGetNumberField(TEXT("Yaw"), Yaw);
        Object->TryGetNumberField(TEXT("Roll"), Roll);
        if (Object->TryGetNumberField(TEXT("Scale"), UniformScale))
        {
            ScaleX = UniformScale;
            ScaleY = UniformScale;
            ScaleZ = UniformScale;
        }
        Object->TryGetNumberField(TEXT("ScaleX"), ScaleX);
        Object->TryGetNumberField(TEXT("ScaleY"), ScaleY);
        Object->TryGetNumberField(TEXT("ScaleZ"), ScaleZ);

        OutTransform = FTransform(FRotator(Pitch, Yaw, Roll), FVector(X, Y, Z), FVector(ScaleX, ScaleY, ScaleZ));
        return true;
    }
}

APrefabActor::APrefabActor()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = true;
    SetReplicateMovement(true);
    SetNetUpdateFrequency(10.0f);
    SetMinNetUpdateFrequency(2.0f);

    Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SetRootComponent(Root);
}

void APrefabActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(APrefabActor, ReplicatedSourceFilePath);
    DOREPLIFETIME(APrefabActor, ReplicatedObjectName);
}

void APrefabActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ReleaseRuntimeResources();
    Super::EndPlay(EndPlayReason);
}

void APrefabActor::Destroyed()
{
    ReleaseRuntimeResources();
    Super::Destroyed();
}

void APrefabActor::SetRenderOnlyMode(bool bInRenderOnlyMode)
{
    bRenderOnlyMode = bInRenderOnlyMode;
    for (TObjectPtr<UStaticMeshComponent>& MeshComponent : MeshComponents)
    {
        ApplyConfigToMeshComponent(MeshComponent.Get());
    }
}

void APrefabActor::OnRep_PrefabReplicationData()
{
    if (ReplicatedSourceFilePath.IsEmpty())
    {
        return;
    }

    SetRenderOnlyMode(UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this));
    LoadPrefab(ResolveReplicatedGltfPathForCurrentWorld(this, ReplicatedSourceFilePath), ReplicatedObjectName);
}

void APrefabActor::ReleaseRuntimeResources()
{
    ClearLoadedComponents();
    if (IsValid(GltfAsset))
    {
        GltfAsset->ClearCache();
        GltfAsset->MarkAsGarbage();
        GltfAsset = nullptr;
    }
}

void APrefabActor::ClearLoadedComponents()
{
    TSet<UStaticMesh*> MeshesToRelease;

    for (UStaticMeshComponent* Component : MeshComponents)
    {
        if (IsValid(Component))
        {
            if (UStaticMesh* Mesh = Component->GetStaticMesh())
            {
                MeshesToRelease.Add(Mesh);
            }
            Component->SetStaticMesh(nullptr);
            Component->UnregisterComponent();
            Component->DestroyComponent();
        }
    }

    for (const TPair<int32, TObjectPtr<UStaticMesh>>& Pair : MeshCache)
    {
        if (UStaticMesh* Mesh = Pair.Value.Get())
        {
            MeshesToRelease.Add(Mesh);
        }
    }

    for (UStaticMesh* Mesh : MeshesToRelease)
    {
        if (IsValid(Mesh) && !Mesh->IsAsset())
        {
            Mesh->MarkAsGarbage();
        }
    }

    MeshComponents.Empty();
    MeshCache.Empty();
    bLoaded = false;
}

bool APrefabActor::LoadConfigJson(const FString& JsonPath)
{
    Config = FPrefabActorConfig();

    FString JsonString;
    if (!FFileHelper::LoadFileToString(JsonString, *JsonPath))
    {
        return false;
    }

    TSharedPtr<FJsonObject> RootObject;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
    if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
    {
        return false;
    }

    RootObject->TryGetStringField(TEXT("DisplayName"), Config.DisplayName);
    if (Config.DisplayName.IsEmpty())
    {
        RootObject->TryGetStringField(TEXT("Name"), Config.DisplayName);
    }

    RootObject->TryGetBoolField(TEXT("EnableCollision"), Config.bEnableCollision);
    RootObject->TryGetBoolField(TEXT("bEnableCollision"), Config.bEnableCollision);
    RootObject->TryGetBoolField(TEXT("SimulatePhysics"), Config.bSimulatePhysics);
    RootObject->TryGetBoolField(TEXT("bSimulatePhysics"), Config.bSimulatePhysics);
    RootObject->TryGetStringField(TEXT("CollisionProfile"), Config.CollisionProfileName);
    RootObject->TryGetStringField(TEXT("CollisionProfileName"), Config.CollisionProfileName);

    const TSharedPtr<FJsonObject>* TransformObject = nullptr;
    if (RootObject->TryGetObjectField(TEXT("Transform"), TransformObject) && TransformObject && TransformObject->IsValid())
    {
        Config.bOverrideLocalTransform = ReadTransformObject(*TransformObject, Config.LocalTransform);
    }
    else if (RootObject->TryGetObjectField(TEXT("LocalTransform"), TransformObject) && TransformObject && TransformObject->IsValid())
    {
        Config.bOverrideLocalTransform = ReadTransformObject(*TransformObject, Config.LocalTransform);
    }

    return true;
}

void APrefabActor::ApplyConfigToMeshComponent(UStaticMeshComponent* MeshComponent) const
{
    if (!IsValid(MeshComponent))
    {
        return;
    }

    const bool bEnablePhysicsCollision = !bRenderOnlyMode && Config.bEnableCollision;
    MeshComponent->SetCollisionEnabled(bEnablePhysicsCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
    MeshComponent->SetCollisionProfileName(Config.CollisionProfileName.IsEmpty() ? TEXT("BlockAll") : FName(*Config.CollisionProfileName));
    MeshComponent->SetGenerateOverlapEvents(bEnablePhysicsCollision);
    MeshComponent->SetSimulatePhysics(!bRenderOnlyMode && Config.bSimulatePhysics);
}

UStaticMesh* APrefabActor::LoadMeshByIndex(int32 MeshIndex)
{
    if (!IsValid(GltfAsset) || MeshIndex == INDEX_NONE)
    {
        return nullptr;
    }

    if (TObjectPtr<UStaticMesh>* Existing = MeshCache.Find(MeshIndex))
    {
        return Existing->Get();
    }

    FglTFRuntimeStaticMeshConfig MeshConfig;
    MeshConfig.Outer = this;
    MeshConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    MeshConfig.MaterialsConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    MeshConfig.MaterialsConfig.bGeneratesMipMaps = false;
    const int32 TextureDimensionLimit = UGameSettings::ResolveMaxTextureResolution(this);
    MeshConfig.MaterialsConfig.ImagesConfig.MaxWidth = TextureDimensionLimit;
    MeshConfig.MaterialsConfig.ImagesConfig.MaxHeight = TextureDimensionLimit;
    MeshConfig.MaterialsConfig.ImagesConfig.bCompressMips = false;
    MeshConfig.MaterialsConfig.ImagesConfig.bStreaming = false;
    MeshConfig.MaterialsConfig.bLoadMipMaps = false;
    {
        const TMap<EglTFRuntimeMaterialType, UMaterialInterface*> LitOverrides =
            glTFMaterialOverrideUtils::BuildOverrideMap(MaterialAssets);
        if (LitOverrides.Num() > 0)
        {
            MeshConfig.MaterialsConfig.UberMaterialsOverrideMap = LitOverrides;
            MeshConfig.MaterialsConfig.UnlitOverrideMap = LitOverrides;
        }
    }
    glTFMaterialOverrideUtils::ApplyNamedOverrides(MaterialAssets, MeshConfig.MaterialsConfig);
    MeshConfig.bAllowCPUAccess = true;
    MeshConfig.bBuildLumenCards = !bRenderOnlyMode;
    MeshConfig.bBuildSimpleCollision = !bRenderOnlyMode;
    MeshConfig.bBuildComplexCollision = !bRenderOnlyMode;
    MeshConfig.bBuildNavCollision = !bRenderOnlyMode;
    MeshConfig.CollisionComplexity = bRenderOnlyMode ? ECollisionTraceFlag::CTF_UseDefault : ECollisionTraceFlag::CTF_UseComplexAsSimple;

    UStaticMesh* Mesh = GltfAsset->LoadStaticMesh(MeshIndex, MeshConfig);
    if (IsValid(Mesh))
    {
        MeshCache.Add(MeshIndex, Mesh);
    }
    return Mesh;
}

bool APrefabActor::LoadPrefab(const FString& InFilePath, const FString& InObjectName)
{
    ClearLoadedComponents();

    SourceFilePath = FPaths::ConvertRelativePathToFull(InFilePath);
    BaseName = FPaths::GetBaseFilename(SourceFilePath);
    ObjectName = InObjectName.IsEmpty() ? BaseName : InObjectName;
    LoadConfigJson(FPaths::ChangeExtension(SourceFilePath, TEXT("json")));

    FglTFRuntimeConfig LoaderConfig;
    LoaderConfig.bAllowExternalFiles = true;
    GltfAsset = UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(SourceFilePath, false, LoaderConfig);
    if (!IsValid(GltfAsset))
    {
        UE_LOG(LogTemp, Warning, TEXT("PrefabActor: failed to load %s"), *SourceFilePath);
        return false;
    }

    const TArray<FglTFRuntimeNode> Nodes = GltfAsset->GetNodes();
    int32 ComponentIndex = 0;
    for (const FglTFRuntimeNode& Node : Nodes)
    {
        if (Node.MeshIndex == INDEX_NONE)
        {
            continue;
        }

        UStaticMesh* Mesh = LoadMeshByIndex(Node.MeshIndex);
        if (!IsValid(Mesh))
        {
            continue;
        }

        UStaticMeshComponent* MeshComponent = NewObject<UStaticMeshComponent>(this, *FString::Printf(TEXT("PrefabMesh_%d"), ComponentIndex++));
        if (!IsValid(MeshComponent))
        {
            continue;
        }

        AddInstanceComponent(MeshComponent);
        MeshComponent->SetMobility(EComponentMobility::Movable);
        MeshComponent->SetupAttachment(Root);
        MeshComponent->SetStaticMesh(Mesh);
        FTransform ComponentTransform = Node.Transform;
        if (Config.bOverrideLocalTransform)
        {
            ComponentTransform = ComponentTransform * Config.LocalTransform;
        }
        MeshComponent->SetRelativeTransform(ComponentTransform);
        ApplyConfigToMeshComponent(MeshComponent);
        MeshComponent->RegisterComponent();
        MeshComponents.Add(MeshComponent);
    }

    bLoaded = MeshComponents.Num() > 0;
    if (bLoaded && HasAuthority())
    {
        ReplicatedSourceFilePath = SourceFilePath;
        ReplicatedObjectName = ObjectName;
        ForceNetUpdate();
    }
    return bLoaded;
}

FPlacedObjectRecord APrefabActor::ToPlacementRecord() const
{
    FPlacedObjectRecord Record;
    Record.ObjectName = ObjectName;
    Record.BaseName = BaseName;
    Record.SourceFile = SourceFilePath;
    Record.Kind = EPlacedObjectKind::Prefab;
    Record.Transform = GetActorTransform();
    return Record;
}
