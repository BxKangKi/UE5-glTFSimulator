// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file DynamicActor.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
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


    static FglTFRuntimeStaticMeshConfig MakeDynamicMeshConfig(
        ADynamicActor* Actor,
        UInstancedEntitySubsystem* InstancedEntities)
    {
        FglTFRuntimeStaticMeshConfig MeshConfig;
        MeshConfig.Outer = InstancedEntities ? InstancedEntities->GetRuntimeMeshOuter() : Actor;
        MeshConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
        MeshConfig.MaterialsConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
        MeshConfig.MaterialsConfig.bGeneratesMipMaps = false;
        const int32 TextureDimensionLimit = UGameSettings::ResolveMaxTextureResolution(Actor);
        MeshConfig.MaterialsConfig.ImagesConfig.MaxWidth = TextureDimensionLimit;
        MeshConfig.MaterialsConfig.ImagesConfig.MaxHeight = TextureDimensionLimit;
        MeshConfig.MaterialsConfig.ImagesConfig.bCompressMips = false;
        MeshConfig.MaterialsConfig.ImagesConfig.bStreaming = false;
        MeshConfig.MaterialsConfig.bLoadMipMaps = false;
        if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(Actor))
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
        return MeshConfig;
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
    ++AsyncMeshLoadGeneration;
    PendingAsyncMeshLoads = 0;
    bAsyncMeshLoadFailed = false;
    bResumingAsyncMeshLoad = false;
    bDispatchingAsyncMeshPreload = false;
    PendingAsyncModelReference.Reset();
    PendingAsyncObjectName.Reset();
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

    // Production loading is preload-only. Falling back to the synchronous facade here would
    // rebuild a complete mesh on the game thread and reintroduce the exact spawn hitch the async
    // preload pipeline is designed to remove. A miss means preload coverage/lifetime is broken;
    // fail the assembly instead of freezing the frame.
    UE_LOG(LogTemp, Error,
        TEXT("DynamicActor: preloaded mesh cache miss; refusing synchronous fallback. Model=%s Mesh=%d"),
        *ModelReference, MeshIndex);
    return nullptr;
}

bool ADynamicActor::BeginAsyncMeshPreload()
{
    check(IsInGameThread());
    if (!IsValid(BakedAsset)) return false;

    UInstancedEntitySubsystem* InstancedEntities = UInstancedEntitySubsystem::Get(this);
    TSet<int32> UniqueMeshIndices;
    const int32 MeshCount = BakedAsset->GetNumMeshes();
    for (const FglTFRuntimeNode& Node : BakedAsset->GetNodes())
    {
        if (Node.MeshIndex >= 0 && Node.MeshIndex < MeshCount)
        {
            UniqueMeshIndices.Add(Node.MeshIndex);
        }
    }

    TArray<int32> MissingMeshIndices;
    MissingMeshIndices.Reserve(UniqueMeshIndices.Num());
    for (const int32 MeshIndex : UniqueMeshIndices)
    {
        if (TObjectPtr<UStaticMesh>* Existing = MeshCache.Find(MeshIndex); Existing && IsValid(Existing->Get()))
        {
            continue;
        }
        if (InstancedEntities)
        {
            if (UStaticMesh* Shared = InstancedEntities->FindSharedMesh(ModelReference, MeshIndex))
            {
                MeshCache.Add(MeshIndex, Shared);
                continue;
            }
        }
        MissingMeshIndices.Add(MeshIndex);
    }

    if (MissingMeshIndices.IsEmpty()) return false;

    const uint64 Generation = ++AsyncMeshLoadGeneration;
    PendingAsyncMeshLoads = MissingMeshIndices.Num();
    bAsyncMeshLoadFailed = false;
    bDispatchingAsyncMeshPreload = true;
    PendingAsyncModelReference = ModelReference;
    PendingAsyncObjectName = ObjectName;
    const FglTFRuntimeStaticMeshConfig MeshConfig = MakeDynamicMeshConfig(this, InstancedEntities);
    TWeakObjectPtr<ADynamicActor> WeakThis(this);

    // Submit every independent range/RuntimeLOD request immediately. The archive preparation work
    // can overlap on workers; independent RuntimeLOD finalizers then run through the bounded-parallel glTFRuntime gate.
    for (const int32 MeshIndex : MissingMeshIndices)
    {
        BakedAsset->LoadStaticMeshAsyncNative(
            MeshIndex,
            [WeakThis, Generation, MeshIndex](UStaticMesh* Mesh)
            {
                if (ADynamicActor* Self = WeakThis.Get())
                {
                    Self->HandleAsyncMeshPreloadResult(Generation, MeshIndex, Mesh);
                }
            },
            MeshConfig);
    }
    bDispatchingAsyncMeshPreload = false;
    if (PendingAsyncMeshLoads == 0) FinishAsyncMeshPreload(Generation);
    return true;
}

void ADynamicActor::HandleAsyncMeshPreloadResult(
    const uint64 Generation, const int32 MeshIndex, UStaticMesh* Mesh)
{
    check(IsInGameThread());
    if (Generation != AsyncMeshLoadGeneration || PendingAsyncMeshLoads <= 0) return;
    if (IsValid(Mesh)) MeshCache.Add(MeshIndex, Mesh);
    else bAsyncMeshLoadFailed = true;
    --PendingAsyncMeshLoads;
    if (PendingAsyncMeshLoads == 0 && !bDispatchingAsyncMeshPreload)
    {
        FinishAsyncMeshPreload(Generation);
    }
}

void ADynamicActor::FinishAsyncMeshPreload(const uint64 Generation)
{
    check(IsInGameThread());
    if (Generation != AsyncMeshLoadGeneration || PendingAsyncMeshLoads != 0) return;

    if (bAsyncMeshLoadFailed)
    {
        UE_LOG(LogTemp, Warning, TEXT("DynamicActor: asynchronous mesh preload failed: %s"), *PendingAsyncModelReference);
        MeshCache.Empty();
        BakedAsset = nullptr;
        ApplyConfigToPhysicsProxy();
        return;
    }

    const FString ResumeReference = PendingAsyncModelReference;
    const FString ResumeObjectName = PendingAsyncObjectName;
    bResumingAsyncMeshLoad = true;
    const bool bFinished = LoadDynamic(ResumeReference, ResumeObjectName);
    bResumingAsyncMeshLoad = false;
    if (!bFinished)
    {
        UE_LOG(LogTemp, Warning, TEXT("DynamicActor: async-preloaded model failed during final assembly: %s"), *ResumeReference);
    }
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

    const bool bResumeAsyncAssembly = bResumingAsyncMeshLoad
        && IsValid(BakedAsset)
        && Model.Reference == ModelReference;
    if (!bResumeAsyncAssembly)
    {
        ClearLoadedComponents();
    }
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
    InstancedRegistrationId = bResumeAsyncAssembly ? INDEX_NONE : InstancedEntities->RegisterEntityFromEntityTemplate(
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
    if (!bResumeAsyncAssembly)
    {
        BakedAsset = FRuntimeModelResolver::LoadAssetSynchronously(Model, LoadError);
        if (!IsValid(BakedAsset))
        {
            const FString FailedPath = ModelReference;
            UE_LOG(LogTemp, Warning, TEXT("DynamicActor: failed to load %s: %s"),
                *FailedPath, *LoadError);
            ClearLoadedComponents();
            return false;
        }
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

    if (!bResumeAsyncAssembly && BeginAsyncMeshPreload())
    {
        // The actor is accepted immediately but collision/physics stay disabled until every mesh
        // has completed its worker build and the final shared-render registration succeeds.
        bLoaded = false;
        ApplyConfigToPhysicsProxy();
        return true;
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
