// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file DynamicActor.cpp
 * 역할: 배치 가능한 런타임 동적 객체을 표현합니다.
 * 핵심 기능: gworld 모델 로드, 물리·충돌 설정, 배치 객체 수명.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "Model/DynamicActor.h"

#include "Components/BoxComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/GameInstance.h"
#include "glTFRuntimeAsset.h"
#include "Materials/MaterialInterface.h"
#include "Model/InstancedEntitySubsystem.h"
#include "Model/glTFMaterialOverrideUtils.h"
#include "Net/UnrealNetwork.h"
#include "Simulator/RuntimeModelResolver.h"
#include "Setting/GameSettings.h"
#include "System/GameManagerSubSystem.h"
#include "System/MultiplayerWorldSubSystem.h"
#include "System/SafeFileIO.h"
#include "System/glTFRuntimeSafety.h"
#include "System/WorldBakedModelAsset.h"

namespace
{
    constexpr int32 MaxRuntimeDynamicNodeCount = 500000;

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

    static FTransform GetDynamicNodeWorldTransform(
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

    static void ResetCommonDynamicRootTransform(
        TMap<int32, FglTFRuntimeNode>& NodeMap,
        const TArray<FglTFRuntimeNode>& Nodes,
        const int32 MeshCount)
    {
        int32 CommonRootIndex = INDEX_NONE;
        for (const FglTFRuntimeNode& Node : Nodes)
        {
            if (Node.MeshIndex < 0 || Node.MeshIndex >= MeshCount || Node.Index < 0) continue;
            int32 RootIndex = Node.Index;
            int32 ParentIndex = Node.ParentIndex;
            TSet<int32> Visited;
            while (const FglTFRuntimeNode* Parent = NodeMap.Find(ParentIndex))
            {
                if (Visited.Contains(ParentIndex)) return;
                Visited.Add(ParentIndex);
                RootIndex = Parent->Index;
                ParentIndex = Parent->ParentIndex;
            }
            if (CommonRootIndex == INDEX_NONE) CommonRootIndex = RootIndex;
            else if (CommonRootIndex != RootIndex) return;
        }

        if (FglTFRuntimeNode* CommonRoot = NodeMap.Find(CommonRootIndex))
        {
            // The reusable entity template is identity-rooted. World placement translation and
            // rotation live only in the chunk object's actor transform; authored local scale stays.
            CommonRoot->Transform.SetLocation(FVector::ZeroVector);
            CommonRoot->Transform.SetRotation(FQuat::Identity);
        }
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

ADynamicActor::ADynamicActor()
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

void ADynamicActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ADynamicActor, ReplicatedModelReference);
    DOREPLIFETIME(ADynamicActor, ReplicatedObjectName);
}

void ADynamicActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ReleaseRuntimeResources();
    Super::EndPlay(EndPlayReason);
}

void ADynamicActor::Destroyed()
{
    ReleaseRuntimeResources();
    Super::Destroyed();
}

void ADynamicActor::SetRenderOnlyMode(bool bInRenderOnlyMode)
{
    bRenderOnlyMode = bInRenderOnlyMode;
    ApplyConfigToPhysicsProxy();
}

void ADynamicActor::OnRep_DynamicReplicationData()
{
    if (ReplicatedModelReference.IsEmpty())
    {
        ClearLoadedComponents();
        return;
    }

    SetRenderOnlyMode(UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(this));
    LoadDynamic(ReplicatedModelReference, ReplicatedObjectName);
}

void ADynamicActor::ReleaseRuntimeResources()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("ADynamicActor runtime release must run on the game thread"))
        || bRuntimeResourcesReleased)
    {
        return;
    }
    bRuntimeResourcesReleased = true;

    ClearLoadedComponents();
}

void ADynamicActor::ClearLoadedComponents()
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
    ModelReference.Reset();
    ObjectName.Reset();
    BaseName.Reset();
    Config = FDynamicActorConfig();
    if (HasAuthority())
    {
        ReplicatedModelReference.Reset();
        ReplicatedObjectName.Reset();
    }

    BakedAsset = nullptr;

    if (IsValid(Root))
    {
        Root->SetSimulatePhysics(false);
        Root->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Root->SetGenerateOverlapEvents(false);
    }
}

bool ADynamicActor::LoadConfigJson(const FString& DefinitionJson)
{
    Config = FDynamicActorConfig();
    constexpr int64 MaxDynamicConfigBytes = 16ll * 1024ll * 1024ll;
    FSafeJsonLimits Limits;
    Limits.MaxFileBytes = MaxDynamicConfigBytes;
    Limits.bAllowBackupRecovery = false;
    const FSafeJsonLoadResult Parsed = FSafeFileIO::ParseJsonText(
        DefinitionJson, TEXT(".gwd Dynamic definition"), Limits);
    const TSharedPtr<FJsonObject> RootObject = Parsed.JsonObject;
    if (!Parsed.IsSuccess() || !RootObject.IsValid())
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

void ADynamicActor::ApplyConfigToPhysicsProxy()
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

UStaticMesh* ADynamicActor::LoadMeshByIndex(int32 MeshIndex)
{
    if (!IsValid(BakedAsset) || MeshIndex < 0 || MeshIndex >= BakedAsset->GetNumMeshes())
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
        if (UStaticMesh* SharedMesh = InstancedEntities->FindSharedMesh(ModelReference, MeshIndex))
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

    UStaticMesh* Mesh = BakedAsset->LoadStaticMesh(MeshIndex, MeshConfig);
    if (IsValid(Mesh))
    {
        MeshCache.Add(MeshIndex, Mesh);
    }
    return Mesh;
}

bool ADynamicActor::LoadDynamic(const FString& InModelReference, const FString& InObjectName)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("ADynamicActor::LoadDynamic must run on the game thread")))
    {
        return false;
    }

    FResolvedRuntimeModel Model;
    FString ResolveError;
    if (!FRuntimeModelResolver::Resolve(this, InModelReference, Model, ResolveError)
        || Model.Definition.ModelType != EModelDefinitionType::Dynamic
        || Model.Definition.EntityType == EModelEntityType::Vehicle
        || Model.Definition.ItemType == EModelItemType::Weapon)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("DynamicActor: built model rejected. Reference=%s Reason=%s"),
            *InModelReference, *ResolveError);
        return false;
    }

    ClearLoadedComponents();
    ModelReference = Model.Reference;
    bRuntimeResourcesReleased = false;
    BaseName = Model.Definition.Name;
    ObjectName = InObjectName.IsEmpty() ? BaseName : InObjectName;
    LoadConfigJson(Model.DefinitionJson);

    UInstancedEntitySubsystem* InstancedEntities = UInstancedEntitySubsystem::Get(this);
    if (!InstancedEntities)
    {
        UE_LOG(LogTemp, Warning, TEXT("DynamicActor: instanced entity subsystem is unavailable."));
        ClearLoadedComponents();
        return false;
    }

    FInstancedEntityRegistrationOptions RegistrationOptions;
    RegistrationOptions.bDynamic = Config.bSimulatePhysics && !bRenderOnlyMode;
    RegistrationOptions.bAllowPhysicsDistanceDeactivation = RegistrationOptions.bDynamic;
    RegistrationOptions.bStoreAsEntityTemplate = true;
    RegistrationOptions.InterpolationSpeed = RegistrationOptions.bDynamic ? 18.0f : 0.0f;

    // Configure the per-entity physics proxy before registration so the distance manager captures
    // the real collision/simulation state that must be restored after far-distance suspension.
    bLoaded = true;
    ApplyConfigToPhysicsProxy();

    FBox TemplateBounds(ForceInit);
    InstancedRegistrationId = InstancedEntities->RegisterEntityFromEntityTemplate(
        ModelReference,
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
            ReplicatedModelReference = ModelReference;
            ReplicatedObjectName = ObjectName;
            ForceNetUpdate();
        }
        return true;
    }

    FString LoadError;
    BakedAsset = FRuntimeModelResolver::LoadAssetSynchronously(Model, LoadError);
    if (!IsValid(BakedAsset))
    {
        const FString FailedPath = ModelReference;
        UE_LOG(LogTemp, Warning, TEXT("DynamicActor: failed to load %s: %s"),
            *FailedPath, *LoadError);
        ClearLoadedComponents();
        return false;
    }

    // The facade owns this immutable baked node table for the duration of this load. Referencing it
    // avoids duplicating a potentially large hierarchy for every streamed Dynamic instance.
    const TArray<FglTFRuntimeNode>& Nodes = BakedAsset->GetNodes();
    if (Nodes.Num() > MaxRuntimeDynamicNodeCount)
    {
        UE_LOG(LogTemp, Warning, TEXT("DynamicActor: glTF node count exceeds the runtime safety limit. Path=%s Nodes=%d"),
            *ModelReference, Nodes.Num());
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

    const int32 MeshCount = BakedAsset->GetNumMeshes();
    ResetCommonDynamicRootTransform(NodeMap, Nodes, MeshCount);
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

        const FglTFRuntimeNode* NormalizedNode = NodeMap.Find(Node.Index);
        if (!NormalizedNode) continue;
        FTransform PartTransform = GetDynamicNodeWorldTransform(NodeMap, *NormalizedNode);
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
        UE_LOG(LogTemp, Warning, TEXT("DynamicActor: no renderable mesh nodes were found: %s"), *ModelReference);
        ClearLoadedComponents();
        return false;
    }

    if (LocalBounds.IsValid)
    {
        Root->SetBoxExtent(MakeSafeOriginCenteredBoxExtent(LocalBounds), true);
    }

    InstancedRegistrationId = InstancedEntities->RegisterEntity(
        ModelReference,
        this,
        Root,
        MeshParts,
        RegistrationOptions,
        LocalBounds);
    bLoaded = InstancedRegistrationId != INDEX_NONE;
    LoadedLocalBounds = LocalBounds;

    // The shared ISM actor now owns the generated meshes. The baked facade and per-entity cache can
    // be released, allowing unused metadata and streamed dependencies to leave memory.
    MeshCache.Empty();
    BakedAsset = nullptr;

    if (!bLoaded)
    {
        ClearLoadedComponents();
        return false;
    }

    ApplyConfigToPhysicsProxy();
    if (HasAuthority())
    {
        ReplicatedModelReference = ModelReference;
        ReplicatedObjectName = ObjectName;
        ForceNetUpdate();
    }
    return true;
}
