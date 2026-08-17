// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "World/PrefabActor.h"

#include "Components/BoxComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeFunctionLibrary.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Materials/MaterialInterface.h"
#include "Model/InstancedEntitySubsystem.h"
#include "Model/glTFMaterialOverrideUtils.h"
#include "Net/UnrealNetwork.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Setting/GameSettings.h"
#include "System/GameManagerSubSystem.h"
#include "System/GlbValidation.h"
#include "System/MacroLibrary.h"
#include "System/MultiplayerWorldSubSystem.h"
#include "System/glTFRuntimeSafety.h"

namespace
{
    constexpr int32 MaxRuntimePrefabNodeCount = 500000;

    static FVector MakeSafeOriginCenteredBoxExtent(const FBox& Bounds)
    {
        if (!Bounds.IsValid)
        {
            return FVector(1.0f);
        }

        return FVector(
            FMath::Max(1.0f, FMath::Max(FMath::Abs(Bounds.Min.X), FMath::Abs(Bounds.Max.X))),
            FMath::Max(1.0f, FMath::Max(FMath::Abs(Bounds.Min.Y), FMath::Abs(Bounds.Max.Y))),
            FMath::Max(1.0f, FMath::Max(FMath::Abs(Bounds.Min.Z), FMath::Abs(Bounds.Max.Z))));
    }

    static FString ResolveReplicatedGltfPathForCurrentWorld(const UObject* WorldContextObject, const FString& InPath)
    {
        FString NormalizedPath = GlbValidation::NormalizePath(InPath);
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
                const FString Candidate = GlbValidation::NormalizePath(
                    FPaths::Combine(PATH_ROOT, WorldFolderName, TEXT("model"), RelativeModelPath));
                if (FPaths::FileExists(Candidate))
                {
                    return Candidate;
                }
            }
        }

        return FString();
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
        return !OutTransform.ContainsNaN();
    }

    static FTransform GetPrefabNodeWorldTransform(
        const TMap<int32, FglTFRuntimeNode>& NodeMap,
        const FglTFRuntimeNode& Node)
    {
        FTransform WorldTransform = Node.Transform;
        int32 ParentIndex = Node.ParentIndex;
        TSet<int32> VisitedParents;

        while (const FglTFRuntimeNode* Parent = NodeMap.Find(ParentIndex))
        {
            if (VisitedParents.Contains(ParentIndex) || Parent->Transform.ContainsNaN())
            {
                break;
            }
            VisitedParents.Add(ParentIndex);
            WorldTransform = WorldTransform * Parent->Transform;
            ParentIndex = Parent->ParentIndex;
        }
        return WorldTransform.ContainsNaN() ? FTransform::Identity : WorldTransform;
    }

    static FBox TransformBounds(const FBox& LocalBounds, const FTransform& Transform)
    {
        FBox Result(ForceInit);
        if (!LocalBounds.IsValid || Transform.ContainsNaN())
        {
            return Result;
        }

        const FVector Min = LocalBounds.Min;
        const FVector Max = LocalBounds.Max;
        const FVector Corners[8] =
        {
            FVector(Min.X, Min.Y, Min.Z),
            FVector(Min.X, Min.Y, Max.Z),
            FVector(Min.X, Max.Y, Min.Z),
            FVector(Min.X, Max.Y, Max.Z),
            FVector(Max.X, Min.Y, Min.Z),
            FVector(Max.X, Min.Y, Max.Z),
            FVector(Max.X, Max.Y, Min.Z),
            FVector(Max.X, Max.Y, Max.Z)
        };

        for (const FVector& Corner : Corners)
        {
            Result += Transform.TransformPosition(Corner);
        }
        return Result;
    }
}

APrefabActor::APrefabActor()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = true;
    SetReplicateMovement(true);
    SetNetUpdateFrequency(10.0f);
    SetMinNetUpdateFrequency(2.0f);

    Root = CreateDefaultSubobject<UBoxComponent>(TEXT("PhysicsProxy"));
    SetRootComponent(Root);
    Root->InitBoxExtent(FVector(50.0f));
    Root->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Root->SetGenerateOverlapEvents(false);
    Root->SetSimulatePhysics(false);
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
    ApplyConfigToPhysicsProxy();
}

void APrefabActor::OnRep_PrefabReplicationData()
{
    if (ReplicatedSourceFilePath.IsEmpty())
    {
        ClearLoadedComponents();
        return;
    }

    const FString ResolvedPath = ResolveReplicatedGltfPathForCurrentWorld(this, ReplicatedSourceFilePath);
    if (ResolvedPath.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("PrefabActor: replicated source file is missing; load skipped. Source=%s"), *ReplicatedSourceFilePath);
        ClearLoadedComponents();
        return;
    }

    SetRenderOnlyMode(UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this));
    LoadPrefab(ResolvedPath, ReplicatedObjectName);
}

void APrefabActor::ReleaseRuntimeResources()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("APrefabActor runtime release must run on the game thread")))
    {
        return;
    }

    ClearLoadedComponents();
}

void APrefabActor::ClearLoadedComponents()
{
    if (InstancedRegistrationId != INDEX_NONE)
    {
        if (UInstancedEntitySubsystem* InstancedEntities = UInstancedEntitySubsystem::Get(this))
        {
            InstancedEntities->UnregisterEntity(InstancedRegistrationId);
        }
        InstancedRegistrationId = INDEX_NONE;
    }

    MeshCache.Empty();
    LoadedLocalBounds.Init();
    bLoaded = false;
    SourceFilePath.Reset();
    ObjectName.Reset();
    BaseName.Reset();
    Config = FPrefabActorConfig();
    if (HasAuthority())
    {
        ReplicatedSourceFilePath.Reset();
        ReplicatedObjectName.Reset();
    }

    if (IsValid(GltfAsset))
    {
        FglTFRuntimeSafety::RequestAssetRelease(GltfAsset);
        GltfAsset = nullptr;
    }

    if (IsValid(Root))
    {
        Root->SetSimulatePhysics(false);
        Root->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Root->SetGenerateOverlapEvents(false);
    }
}

bool APrefabActor::LoadConfigJson(const FString& JsonPath)
{
    Config = FPrefabActorConfig();

    if (JsonPath.IsEmpty() || !IFileManager::Get().FileExists(*JsonPath))
    {
        return false;
    }

    constexpr int64 MaxPrefabConfigBytes = 16ll * 1024ll * 1024ll;
    const int64 JsonFileSize = IFileManager::Get().FileSize(*JsonPath);
    if (JsonFileSize < 0 || JsonFileSize > MaxPrefabConfigBytes)
    {
        return false;
    }

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
    double LoadedMassKg = Config.MassKg;
    if ((RootObject->TryGetNumberField(TEXT("MassKg"), LoadedMassKg)
            || RootObject->TryGetNumberField(TEXT("PhysicsMassKg"), LoadedMassKg))
        && FMath::IsFinite(LoadedMassKg))
    {
        Config.MassKg = FMath::Clamp(static_cast<float>(LoadedMassKg), 0.0f, 1000000000.0f);
    }
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

void APrefabActor::ApplyConfigToPhysicsProxy()
{
    if (!IsValid(Root))
    {
        return;
    }

    const bool bEnablePhysicsCollision = bLoaded && !bRenderOnlyMode && Config.bEnableCollision;
    if (Root->IsSimulatingPhysics() && (!bEnablePhysicsCollision || !Config.bSimulatePhysics))
    {
        Root->SetSimulatePhysics(false);
    }

    const FName CollisionProfileName = Config.CollisionProfileName.IsEmpty()
        ? FName(TEXT("BlockAll"))
        : FName(*Config.CollisionProfileName);
    Root->SetCollisionProfileName(CollisionProfileName);
    Root->SetCollisionEnabled(bEnablePhysicsCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
    Root->SetGenerateOverlapEvents(bEnablePhysicsCollision);
    if (bEnablePhysicsCollision && Config.bSimulatePhysics)
    {
        if (!Root->IsSimulatingPhysics())
        {
            Root->SetSimulatePhysics(true);
        }

        if (Config.MassKg > 0.0f)
        {
            Root->SetMassOverrideInKg(NAME_None, Config.MassKg, true);
        }
        else
        {
            Root->SetMassOverrideInKg(NAME_None, 0.0f, false);
        }
    }
}

UStaticMesh* APrefabActor::LoadMeshByIndex(int32 MeshIndex)
{
    if (!IsValid(GltfAsset) || MeshIndex < 0 || MeshIndex >= GltfAsset->GetNumMeshes())
    {
        return nullptr;
    }

    if (TObjectPtr<UStaticMesh>* Existing = MeshCache.Find(MeshIndex))
    {
        return Existing->Get();
    }

    UInstancedEntitySubsystem* InstancedEntities = UInstancedEntitySubsystem::Get(this);
    if (InstancedEntities)
    {
        if (UStaticMesh* SharedMesh = InstancedEntities->FindSharedMesh(SourceFilePath, MeshIndex))
        {
            MeshCache.Add(MeshIndex, SharedMesh);
            return SharedMesh;
        }
    }

    FglTFRuntimeStaticMeshConfig MeshConfig;
    MeshConfig.Outer = InstancedEntities ? InstancedEntities->GetRuntimeMeshOuter() : this;
    MeshConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    MeshConfig.MaterialsConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    MeshConfig.MaterialsConfig.bGeneratesMipMaps = false;
    const int32 TextureDimensionLimit = UGameSettings::ResolveMaxTextureResolution(this);
    MeshConfig.MaterialsConfig.ImagesConfig.MaxWidth = TextureDimensionLimit;
    MeshConfig.MaterialsConfig.ImagesConfig.MaxHeight = TextureDimensionLimit;
    MeshConfig.MaterialsConfig.ImagesConfig.bCompressMips = false;
    MeshConfig.MaterialsConfig.ImagesConfig.bStreaming = false;
    MeshConfig.MaterialsConfig.bLoadMipMaps = false;
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        glTFMaterialOverrideUtils::ApplyOverrides(
            GameManager->GetMaterialDefaultReferences(),
            MeshConfig.MaterialsConfig);
    }
    MeshConfig.bAllowCPUAccess = false;
    MeshConfig.bBuildLumenCards = true;
    MeshConfig.bBuildSimpleCollision = false;
    MeshConfig.bBuildComplexCollision = false;
    MeshConfig.bBuildNavCollision = false;
    MeshConfig.CollisionComplexity = ECollisionTraceFlag::CTF_UseDefault;

    UStaticMesh* Mesh = GltfAsset->LoadStaticMesh(MeshIndex, MeshConfig);
    if (IsValid(Mesh))
    {
        MeshCache.Add(MeshIndex, Mesh);
    }
    return Mesh;
}

bool APrefabActor::LoadPrefab(const FString& InFilePath, const FString& InObjectName)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("APrefabActor::LoadPrefab must run on the game thread")))
    {
        return false;
    }

    const FString NormalizedPath = GlbValidation::NormalizePath(InFilePath);
    if (NormalizedPath.IsEmpty() || !IFileManager::Get().FileExists(*NormalizedPath))
    {
        UE_LOG(LogTemp, Warning, TEXT("PrefabActor: source file is missing; load skipped. Path=%s"), *NormalizedPath);
        return false;
    }

    FString ValidationReason;
    if (!GlbValidation::ValidateRuntimeModelFile(NormalizedPath, ValidationReason))
    {
        UE_LOG(LogTemp, Warning, TEXT("PrefabActor: invalid glTF model skipped. Path=%s Reason=%s"), *NormalizedPath, *ValidationReason);
        return false;
    }

    ClearLoadedComponents();
    SourceFilePath = NormalizedPath;
    BaseName = FPaths::GetBaseFilename(SourceFilePath);
    ObjectName = InObjectName.IsEmpty() ? BaseName : InObjectName;
    LoadConfigJson(FPaths::ChangeExtension(SourceFilePath, TEXT("json")));

    UInstancedEntitySubsystem* InstancedEntities = UInstancedEntitySubsystem::Get(this);
    if (!InstancedEntities)
    {
        UE_LOG(LogTemp, Warning, TEXT("PrefabActor: instanced entity subsystem is unavailable."));
        ClearLoadedComponents();
        return false;
    }

    FInstancedEntityRegistrationOptions RegistrationOptions;
    RegistrationOptions.bDynamic = Config.bSimulatePhysics && !bRenderOnlyMode;
    RegistrationOptions.bAllowPhysicsDistanceDeactivation = RegistrationOptions.bDynamic;
    RegistrationOptions.bStoreAsPrefabTemplate = true;
    RegistrationOptions.InterpolationSpeed = RegistrationOptions.bDynamic ? 18.0f : 0.0f;

    // Configure the per-entity physics proxy before registration so the distance manager captures
    // the real collision/simulation state that must be restored after far-distance suspension.
    bLoaded = true;
    ApplyConfigToPhysicsProxy();

    FBox TemplateBounds(ForceInit);
    InstancedRegistrationId = InstancedEntities->RegisterEntityFromPrefabTemplate(
        SourceFilePath,
        this,
        Root,
        RegistrationOptions,
        TemplateBounds);
    if (InstancedRegistrationId != INDEX_NONE)
    {
        LoadedLocalBounds = TemplateBounds;
        if (LoadedLocalBounds.IsValid)
        {
            Root->SetBoxExtent(MakeSafeOriginCenteredBoxExtent(LoadedLocalBounds), true);
        }
        ApplyConfigToPhysicsProxy();

        if (HasAuthority())
        {
            ReplicatedSourceFilePath = SourceFilePath;
            ReplicatedObjectName = ObjectName;
            ForceNetUpdate();
        }
        return true;
    }

    FglTFRuntimeConfig LoaderConfig;
    LoaderConfig.bAllowExternalFiles = true;
    GltfAsset = UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(SourceFilePath, false, LoaderConfig);
    if (!IsValid(GltfAsset))
    {
        const FString FailedPath = SourceFilePath;
        UE_LOG(LogTemp, Warning, TEXT("PrefabActor: failed to load %s"), *FailedPath);
        ClearLoadedComponents();
        return false;
    }

    const TArray<FglTFRuntimeNode> Nodes = GltfAsset->GetNodes();
    if (Nodes.Num() > MaxRuntimePrefabNodeCount)
    {
        UE_LOG(LogTemp, Warning, TEXT("PrefabActor: glTF node count exceeds the runtime safety limit. Path=%s Nodes=%d"),
            *SourceFilePath, Nodes.Num());
        ClearLoadedComponents();
        return false;
    }

    TMap<int32, FglTFRuntimeNode> NodeMap;
    for (const FglTFRuntimeNode& Node : Nodes)
    {
        if (Node.Index >= 0 && Node.Index < Nodes.Num() && !Node.Transform.ContainsNaN())
        {
            NodeMap.Add(Node.Index, Node);
        }
    }

    const int32 MeshCount = GltfAsset->GetNumMeshes();
    TArray<FInstancedEntityMeshPart> MeshParts;
    FBox LocalBounds(ForceInit);
    for (const FglTFRuntimeNode& Node : Nodes)
    {
        if (Node.MeshIndex < 0 || Node.MeshIndex >= MeshCount || Node.Transform.ContainsNaN())
        {
            continue;
        }

        UStaticMesh* Mesh = LoadMeshByIndex(Node.MeshIndex);
        if (!IsValid(Mesh))
        {
            continue;
        }

        FTransform PartTransform = GetPrefabNodeWorldTransform(NodeMap, Node);
        if (Config.bOverrideLocalTransform)
        {
            PartTransform = PartTransform * Config.LocalTransform;
        }
        if (PartTransform.ContainsNaN())
        {
            continue;
        }

        FInstancedEntityMeshPart& Part = MeshParts.AddDefaulted_GetRef();
        Part.MeshKey = Node.MeshIndex;
        Part.Mesh = Mesh;
        Part.LocalTransform = PartTransform;

        const FBox PartBounds = TransformBounds(Mesh->GetBoundingBox(), PartTransform);
        if (PartBounds.IsValid)
        {
            LocalBounds += PartBounds.Min;
            LocalBounds += PartBounds.Max;
        }
    }

    if (MeshParts.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("PrefabActor: no renderable mesh nodes were found: %s"), *SourceFilePath);
        ClearLoadedComponents();
        return false;
    }

    if (LocalBounds.IsValid)
    {
        Root->SetBoxExtent(MakeSafeOriginCenteredBoxExtent(LocalBounds), true);
    }

    InstancedRegistrationId = InstancedEntities->RegisterEntity(
        SourceFilePath,
        this,
        Root,
        MeshParts,
        RegistrationOptions,
        LocalBounds);
    bLoaded = InstancedRegistrationId != INDEX_NONE;
    LoadedLocalBounds = LocalBounds;

    // The shared ISM actor now owns the generated meshes. The parser and per-entity cache can be released.
    MeshCache.Empty();
    if (IsValid(GltfAsset))
    {
        FglTFRuntimeSafety::RequestAssetRelease(GltfAsset);
        GltfAsset = nullptr;
    }

    if (!bLoaded)
    {
        ClearLoadedComponents();
        return false;
    }

    ApplyConfigToPhysicsProxy();
    if (HasAuthority())
    {
        ReplicatedSourceFilePath = SourceFilePath;
        ReplicatedObjectName = ObjectName;
        ForceNetUpdate();
    }
    return true;
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
