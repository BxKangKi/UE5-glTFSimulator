// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file CharacterLoadAsyncAction.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Character/CharacterLoadAsyncAction.h"

#include "Animation/Skeleton.h"
#include "Character/CharacterController.h"
#include "Character/CharacterBoneSchema.h"
#include "Character/CharacterFunctionLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Setting/GameSettings.h"
#include "System/FileFunctionLibrary.h"
#include "System/SafeFileIO.h"
#include "System/glTFRuntimeSafety.h"
#include "System/MacroLibrary.h"
#include "Simulator/RuntimeModelResolver.h"
#include "System/WorldBakedModelAsset.h"
#include "TimerManager.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    constexpr int32 MaxSkeletonBonesForGeneratedSecondaryPhysics = 512;
    constexpr int32 MaxModelDatabaseResolveRetries = 120;
    constexpr float ModelDatabaseResolveRetrySeconds = 0.05f;

    // Unity builds concatenate multiple .cpp files into one translation unit. The file-specific
    // prefix prevents this helper from colliding with similarly named model-loading validators.
    bool IsFiniteCharacterLoadVector(const FVector& Vector)
    {
        return FMath::IsFinite(Vector.X) &&
            FMath::IsFinite(Vector.Y) &&
            FMath::IsFinite(Vector.Z);
    }

    bool IsFiniteCharacterLoadQuat(const FQuat& Rotation)
    {
        return FMath::IsFinite(Rotation.X) &&
            FMath::IsFinite(Rotation.Y) &&
            FMath::IsFinite(Rotation.Z) &&
            FMath::IsFinite(Rotation.W);
    }

    bool IsFiniteCharacterLoadTransform(const FTransform& Transform)
    {
        const FQuat Rotation = Transform.GetRotation();
        return !Transform.ContainsNaN() &&
            IsFiniteCharacterLoadVector(Transform.GetLocation()) &&
            IsFiniteCharacterLoadVector(Transform.GetScale3D()) &&
            IsFiniteCharacterLoadQuat(Rotation) &&
            Rotation.IsNormalized();
    }

    void ReleaseTransientRuntimeObject(UObject* Object)
    {
        if (IsValid(Object) && !Object->IsAsset())
        {
            Object->ClearFlags(RF_Public | RF_Standalone);
            Object->SetFlags(RF_Transient);
        }
    }
}

UCharacterLoadAsyncAction* UCharacterLoadAsyncAction::LoadCharacterAsync(
    UObject* WorldContextObject,
    ACharacterController* InOwner,
    FString InPath)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("LoadCharacterAsync must create its UObject on the game thread")))
    {
        return nullptr;
    }

    UCharacterLoadAsyncAction* Action = NewObject<UCharacterLoadAsyncAction>();
    Action->OwnerCharacter = InOwner;
    Action->ReleaseObserver = InOwner;
    Action->FilePath = MoveTemp(InPath);
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}

void UCharacterLoadAsyncAction::Activate()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("UCharacterLoadAsyncAction::Activate must run on the game thread")))
    {
        TWeakObjectPtr<UCharacterLoadAsyncAction> WeakThis(this);
        if (!FSafeFileIO::DispatchTrackedGameThread([WeakThis]()
        {
            if (UCharacterLoadAsyncAction* StrongThis = WeakThis.Get())
            {
                StrongThis->Activate();
            }
        }))
        {
            // Module shutdown suppresses late UObject callbacks; no off-thread cleanup is safe here.
            return;
        }
        return;
    }

    bCancelled = false;
    bFinished = false;
    bMeshLoadInFlight = false;
    DetectedMeshIndex = INDEX_NONE;
    DetectedSkinIndex = INDEX_NONE;
    ClearGameThreadStageTimer();
    CurrentLoadedAsset = nullptr;
    CurrentRuntimeSkeleton = nullptr;
    PendingSkeletalMesh = nullptr;
    PendingRuntimePhysicsAsset = nullptr;
    PendingBoneMap.Empty();
    ModelDatabaseRetryCount = 0;
    OnProgress.Broadcast(0.0f);

    if (!OwnerCharacter.IsValid())
    {
        FailLoad(TEXT("Character built-model preflight failed because the owner pawn is invalid"));
        return;
    }

    ResolveAndLoadModel();
}

void UCharacterLoadAsyncAction::ResolveAndLoadModel()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("ResolveAndLoadModel must run on the game thread")))
    {
        return;
    }

    GameThreadStageTimer.Invalidate();
    if (bCancelled || bFinished)
    {
        TryFinishCancelledRequest();
        return;
    }
    if (!OwnerCharacter.IsValid())
    {
        FailLoad(TEXT("Character owner became invalid while waiting for the built-model database"));
        return;
    }

    FResolvedRuntimeModel Model;
    FString ResolveError;
    if (!FRuntimeModelResolver::Resolve(this, FilePath, Model, ResolveError))
    {
        const bool bDatabaseNotReady = ResolveError.Equals(
            TEXT("a verified .gwd model database is not open"), ESearchCase::IgnoreCase);
        if (bDatabaseNotReady && ModelDatabaseRetryCount < MaxModelDatabaseResolveRetries)
        {
            ++ModelDatabaseRetryCount;
            OnProgress.Broadcast(0.01f);
            ScheduleModelDatabaseRetry();
            return;
        }

        FailLoad(FString::Printf(
            TEXT("Character built-model lookup failed. Reference=%s Reason=%s"),
            *FilePath, *ResolveError));
        return;
    }

    if (Model.Definition.ModelType != EModelDefinitionType::Character)
    {
        FailLoad(FString::Printf(
            TEXT("Character built-model lookup resolved a non-character model. Reference=%s"),
            *FilePath));
        return;
    }

    FilePath = Model.Reference;
    PendingBoneMap = MoveTemp(Model.Definition.Bones);
    FString BoneMapError;
    if (!CharacterBoneSchema::ValidateSourceToCanonicalMap(PendingBoneMap, BoneMapError))
    {
        FailLoad(FString::Printf(
            TEXT("Character archive contains an invalid canonical bone map: %s"),
            *BoneMapError));
        return;
    }

    // This creates only the lightweight baked-world facade and reads its node/range tables.
    // No source GLB bytes are opened, parsed, or retained on the gameplay path.
    UWorldBakedModelAsset* Asset = FRuntimeModelResolver::LoadAssetSynchronously(Model, ResolveError);
    if (!IsValid(Asset))
    {
        FailLoad(FString::Printf(
            TEXT("Character baked-data initialization failed. Reference=%s Reason=%s"),
            *FilePath, *ResolveError));
        return;
    }
    OnBakedAssetLoaded(Asset);
}

void UCharacterLoadAsyncAction::ScheduleModelDatabaseRetry()
{
    ClearGameThreadStageTimer();
    ACharacterController* Owner = OwnerCharacter.Get();
    UWorld* World = IsValid(Owner) ? Owner->GetWorld() : nullptr;
    if (!IsValid(World))
    {
        FailLoad(TEXT("Character owner world became invalid while waiting for the built-model database"));
        return;
    }

    TWeakObjectPtr<UCharacterLoadAsyncAction> WeakThis(this);
    FTimerDelegate Delegate = FTimerDelegate::CreateLambda([WeakThis]()
    {
        if (UCharacterLoadAsyncAction* StrongThis = WeakThis.Get())
        {
            StrongThis->ResolveAndLoadModel();
        }
    });
    World->GetTimerManager().SetTimer(
        GameThreadStageTimer, Delegate, ModelDatabaseResolveRetrySeconds, false);
}

void UCharacterLoadAsyncAction::OnBakedAssetLoaded(UWorldBakedModelAsset* Asset)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("OnBakedAssetLoaded must run on the game thread")))
    {
        return;
    }

    if (bCancelled)
    {
        FinishAndRelease();
        return;
    }

    if (!IsValid(Asset) || !OwnerCharacter.IsValid())
    {
        CurrentLoadedAsset = Asset;
        FailLoad(FString::Printf(
            TEXT("Character baked-data facade failed or owner became invalid. Path=%s"),
            *FilePath));
        return;
    }

    CurrentLoadedAsset = Asset;
    if (!ResolveCharacterSkin(Asset))
    {
        FailLoad(FString::Printf(
            TEXT("Character baked model has no valid skinned mesh node: %s"),
            *FilePath));
        return;
    }

    OnProgress.Broadcast(0.25f);
    ContinueWithEmbeddedBoneMap();
}

void UCharacterLoadAsyncAction::ContinueWithEmbeddedBoneMap()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("ContinueWithEmbeddedBoneMap must run on the game thread")))
    {
        return;
    }

    if (bCancelled || bFinished)
    {
        TryFinishCancelledRequest();
        return;
    }

    if (!IsValid(CurrentLoadedAsset) || !OwnerCharacter.IsValid())
    {
        FailLoad(TEXT("Character asset or owner became invalid before mesh creation"));
        return;
    }
    // Bone remapping was range-read and validated with this model's definition member. No source
    // JSON file is touched here, and no redundant worker hop is needed.
    OnProgress.Broadcast(0.40f);
    ScheduleGameThreadStage(&UCharacterLoadAsyncAction::BeginSkeletalMeshLoad_GameThread);
}

void UCharacterLoadAsyncAction::BeginSkeletalMeshLoad_GameThread()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("BeginSkeletalMeshLoad_GameThread must run on the game thread")))
    {
        return;
    }

    GameThreadStageTimer.Invalidate();
    if (bCancelled || bFinished)
    {
        FinishAndRelease();
        return;
    }

    ACharacterController* Owner = OwnerCharacter.Get();
    if (!IsValid(Owner) || !IsValid(CurrentLoadedAsset) ||
        DetectedMeshIndex == INDEX_NONE || DetectedSkinIndex == INDEX_NONE)
    {
        FailLoad(TEXT("Character asset, owner, or skin became invalid before asynchronous mesh creation"));
        return;
    }

    USkeleton* DefaultSkeleton = Owner->DefaultSkeleton.Get();
    UMaterialInterface* Material = Owner->DefaultMaterial.Get();
    if (!IsValid(DefaultSkeleton) || !IsValid(Material))
    {
        FailLoad(TEXT("Character default skeleton or material is not assigned"));
        return;
    }
    FString SkeletonError;
    if (!CharacterBoneSchema::ValidateCanonicalSkeleton(DefaultSkeleton, SkeletonError))
    {
        FailLoad(FString::Printf(
            TEXT("Default character skeleton does not match the canonical bone schema: %s"),
            *SkeletonError));
        return;
    }

    // FglTFRuntimeSkeletalMeshConfig stores raw UObject pointers. The owner is deliberately weak and
    // may be destroyed during travel, so keep the source skeleton/material GC-safe on this action
    // until the native terminal callback has acknowledged completion or cancellation.
    SourceSkeletonReferenceGuard = DefaultSkeleton;
    SourceMaterialReferenceGuard = Material;

    // Creating/duplicating UObjects must remain on the game thread. Archive range I/O, zlib and
    // checksums run on a worker; UObject reconstruction/finalization is marshalled back here.
    CurrentRuntimeSkeleton = UCharacterFunctionLibrary::DuplicateSkeleton(DefaultSkeleton);
    if (!IsValid(CurrentRuntimeSkeleton))
    {
        FailLoad(TEXT("Failed to create the transient runtime skeleton"));
        return;
    }

    FglTFRuntimeSkeletalMeshConfig Config;
    // These cache flags remain part of the finalizer configuration. The baked facade
    // does not use a source parser cache and keeps only weak, reclaimable texture reuse entries.
    Config.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    Config.Skeleton = CurrentRuntimeSkeleton;
    Config.bOverwriteRefSkeleton = false;
    Config.bMergeAllBonesToBoneTree = true;
    Config.bIgnoreSkin = false;
    Config.OverrideSkinIndex = DetectedSkinIndex;
    Config.SkeletonConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    Config.SkeletonConfig.bAddRootBone = CheckRootBoneName(CurrentLoadedAsset);
    Config.SkeletonConfig.RootBoneName = TEXT("Root");
    Config.SkeletonConfig.BonesNameMap = PendingBoneMap;
    Config.SkeletonConfig.RootNodeIndex = -1;
    Config.SkeletonConfig.bClearRotations = true;
    Config.SkeletonConfig.CopyRotationsFrom = DefaultSkeleton;
    Config.SkeletonConfig.MaxNodesTreeDepth = -1;
    Config.SkeletonConfig.bAddRootNodeIfMissing = true;
    Config.MaterialsConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;

    TMap<EglTFRuntimeMaterialType, UMaterialInterface*> MaterialMap;
    MaterialMap.Add(EglTFRuntimeMaterialType::Opaque, Material);
    MaterialMap.Add(EglTFRuntimeMaterialType::Translucent, Material);
    MaterialMap.Add(EglTFRuntimeMaterialType::TwoSided, Material);
    MaterialMap.Add(EglTFRuntimeMaterialType::TwoSidedTranslucent, Material);
    MaterialMap.Add(EglTFRuntimeMaterialType::Masked, Material);
    MaterialMap.Add(EglTFRuntimeMaterialType::TwoSidedMasked, Material);
    Config.MaterialsConfig.UberMaterialsOverrideMap = MaterialMap;
    Config.MaterialsConfig.UnlitOverrideMap = MaterialMap;
    Config.MaterialsConfig.bGeneratesMipMaps = false;
    Config.MaterialsConfig.SpecularFactor = 0.0f;

    const int32 TextureDimensionLimit = UGameSettings::ResolveMaxTextureResolution(Owner);
    Config.MaterialsConfig.ImagesConfig.MaxWidth = TextureDimensionLimit;
    Config.MaterialsConfig.ImagesConfig.MaxHeight = TextureDimensionLimit;
    Config.MaterialsConfig.ImagesConfig.bCompressMips = false;
    Config.MaterialsConfig.ImagesConfig.bStreaming = false;
    Config.MaterialsConfig.bLoadMipMaps = false;
    Config.bIgnoreMissingBones = true;
    Config.Outer = GetTransientPackage();
    Config.bIgnoreEmptyMorphTargets = true;

    // Physics bodies are built in a separate game-thread stage from the directly assigned
    // physics template. Avoid asking the glTF worker/finalizer to generate a second asset.
    Config.bAutoGeneratePhysicsAssetBodies = false;
    Config.bAllowCPUAccess = false;

    OnProgress.Broadcast(0.55f);
    bMeshLoadInFlight = true;

    FglTFRuntimeSkeletalMeshAsync MeshDelegate;
    MeshDelegate.BindDynamic(this, &UCharacterLoadAsyncAction::OnMeshLoaded);
    // The facade range-reads only this mesh, its selected skin, and directly referenced
    // material/texture .dat members on a worker. UObject finalization is marshalled back to GT.
    CurrentLoadedAsset->LoadSkeletalMeshAsync(
        DetectedMeshIndex,
        DetectedSkinIndex,
        MeshDelegate,
        Config);
}

void UCharacterLoadAsyncAction::OnMeshLoaded(USkeletalMesh* SkeletalMesh)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("glTFRuntime skeletal-mesh callback must run on the game thread")))
    {
        return;
    }

    bMeshLoadInFlight = false;

    if (bCancelled || bFinished)
    {
        ReleaseTransientRuntimeObject(SkeletalMesh);
        FinishAndRelease();
        return;
    }

    if (!IsValid(SkeletalMesh) || !OwnerCharacter.IsValid() ||
        SkeletalMesh->GetRefSkeleton().GetNum() <= 0)
    {
        ReleaseTransientRuntimeObject(SkeletalMesh);
        FglTFRuntimeSafety::ReportRecoverableFailure(
            FilePath,
            TEXT("glTFRuntime returned a null or structurally invalid skeletal mesh"));
        FailLoad(FString::Printf(
            TEXT("glTFRuntime returned an invalid character mesh. Path=%s"),
            *FilePath));
        return;
    }

    const USkeleton* TargetSkeleton = SourceSkeletonReferenceGuard.Get();
    FString HierarchyError;
    if (!IsValid(TargetSkeleton)
        || !CharacterBoneSchema::ValidateCanonicalHierarchyMatches(
            SkeletalMesh->GetRefSkeleton(),
            TargetSkeleton->GetReferenceSkeleton(),
            HierarchyError))
    {
        ReleaseTransientRuntimeObject(SkeletalMesh);
        FglTFRuntimeSafety::ReportRecoverableFailure(
            FilePath,
            FString::Printf(TEXT("canonical character skeleton hierarchy mismatch: %s"), *HierarchyError));
        FailLoad(FString::Printf(
            TEXT("Character mesh skeleton does not match the canonical target hierarchy: %s"),
            *HierarchyError));
        return;
    }

    PendingSkeletalMesh = SkeletalMesh;
    ReleaseTransientRuntimeObject(PendingSkeletalMesh);

    OnProgress.Broadcast(0.80f);

    // Split finalization across frames so the mesh finalizer, physics setup, and component swap
    // never stack in one game-thread frame.
    ScheduleGameThreadStage(&UCharacterLoadAsyncAction::BuildRuntimePhysics_GameThread);
}

void UCharacterLoadAsyncAction::BuildRuntimePhysics_GameThread()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("BuildRuntimePhysics_GameThread must run on the game thread")))
    {
        return;
    }

    GameThreadStageTimer.Invalidate();
    if (bCancelled || bFinished)
    {
        FinishAndRelease();
        return;
    }

    if (!IsValid(PendingSkeletalMesh) || !OwnerCharacter.IsValid())
    {
        FailLoad(TEXT("Character mesh or owner became invalid before physics finalization"));
        return;
    }

    ACharacterController* Owner = OwnerCharacter.Get();
    UPhysicsAsset* PhysicsSource = IsValid(Owner) ? Owner->DefaultPhysicsAsset.Get() : nullptr;
    if (IsValid(PhysicsSource))
    {
        PendingRuntimePhysicsAsset = DuplicateObject<UPhysicsAsset>(
            PhysicsSource,
            Owner,
            MakeUniqueObjectName(
                Owner,
                UPhysicsAsset::StaticClass(),
                FName(TEXT("RuntimeCharacterPhysicsAsset"))));

        if (IsValid(PendingRuntimePhysicsAsset))
        {
            ReleaseTransientRuntimeObject(PendingRuntimePhysicsAsset);
            PendingRuntimePhysicsAsset = UCharacterFunctionLibrary::MergePhysicsAsset(
                PendingRuntimePhysicsAsset,
                nullptr,
                PendingSkeletalMesh);
        }
    }

    OnProgress.Broadcast(0.84f);
    ScheduleGameThreadStage(&UCharacterLoadAsyncAction::BuildHairPhysics_GameThread);
}

void UCharacterLoadAsyncAction::BuildHairPhysics_GameThread()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("BuildHairPhysics_GameThread must run on the game thread")))
    {
        return;
    }

    GameThreadStageTimer.Invalidate();
    if (bCancelled || bFinished)
    {
        FinishAndRelease();
        return;
    }

    if (!IsValid(PendingSkeletalMesh) || !OwnerCharacter.IsValid())
    {
        FailLoad(TEXT("Character mesh or owner became invalid during staged physics setup"));
        return;
    }

    const int32 BoneCount = PendingSkeletalMesh->GetRefSkeleton().GetNum();
    if (IsValid(PendingRuntimePhysicsAsset) &&
        BoneCount <= MaxSkeletonBonesForGeneratedSecondaryPhysics)
    {
        UCharacterFunctionLibrary::SetupAllBodiesBelowCollidersAndConstraints(
            PendingRuntimePhysicsAsset,
            PendingSkeletalMesh,
            BONE_HAIR_ROOT);
    }
    else if (IsValid(PendingRuntimePhysicsAsset) &&
        BoneCount > MaxSkeletonBonesForGeneratedSecondaryPhysics)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Skipping generated secondary-physics chains for a large character rig. Bones=%d Limit=%d Path=%s"),
            BoneCount,
            MaxSkeletonBonesForGeneratedSecondaryPhysics,
            *FilePath);
    }

    OnProgress.Broadcast(0.88f);
    ScheduleGameThreadStage(&UCharacterLoadAsyncAction::BuildDynamicPhysics_GameThread);
}

void UCharacterLoadAsyncAction::BuildDynamicPhysics_GameThread()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("BuildDynamicPhysics_GameThread must run on the game thread")))
    {
        return;
    }

    GameThreadStageTimer.Invalidate();
    if (bCancelled || bFinished)
    {
        FinishAndRelease();
        return;
    }

    if (!IsValid(PendingSkeletalMesh) || !OwnerCharacter.IsValid())
    {
        FailLoad(TEXT("Character mesh or owner became invalid during staged physics setup"));
        return;
    }

    if (IsValid(PendingRuntimePhysicsAsset) &&
        PendingSkeletalMesh->GetRefSkeleton().GetNum() <= MaxSkeletonBonesForGeneratedSecondaryPhysics)
    {
        UCharacterFunctionLibrary::SetupAllBodiesBelowCollidersAndConstraints(
            PendingRuntimePhysicsAsset,
            PendingSkeletalMesh,
            BONE_DYN_ROOT);
    }

    OnProgress.Broadcast(0.92f);
    ScheduleGameThreadStage(&UCharacterLoadAsyncAction::FinalizeRuntimePhysics_GameThread);
}

void UCharacterLoadAsyncAction::FinalizeRuntimePhysics_GameThread()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("FinalizeRuntimePhysics_GameThread must run on the game thread")))
    {
        return;
    }

    GameThreadStageTimer.Invalidate();
    if (bCancelled || bFinished)
    {
        FinishAndRelease();
        return;
    }

    ACharacterController* Owner = OwnerCharacter.Get();
    if (!IsValid(Owner) || !IsValid(PendingSkeletalMesh))
    {
        FailLoad(TEXT("Character mesh or owner became invalid before physics finalization"));
        return;
    }

    UPhysicsAsset* PhysicsSource = Owner->DefaultPhysicsAsset.Get();
    if (IsValid(PendingRuntimePhysicsAsset) && IsValid(PhysicsSource))
    {
        PendingRuntimePhysicsAsset = UCharacterFunctionLibrary::MergePhysicsAsset(
            PendingRuntimePhysicsAsset,
            PhysicsSource,
            PendingSkeletalMesh);
    }

    OnProgress.Broadcast(0.96f);
    ScheduleGameThreadStage(&UCharacterLoadAsyncAction::CommitRuntimeMesh_GameThread);
}

void UCharacterLoadAsyncAction::CommitRuntimeMesh_GameThread()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("CommitRuntimeMesh_GameThread must run on the game thread")))
    {
        return;
    }

    GameThreadStageTimer.Invalidate();
    if (bCancelled || bFinished)
    {
        FinishAndRelease();
        return;
    }

    ACharacterController* Owner = OwnerCharacter.Get();
    USkeletalMesh* MeshToCommit = PendingSkeletalMesh.Get();
    UPhysicsAsset* PhysicsToCommit = PendingRuntimePhysicsAsset.Get();
    USkeleton* SkeletonToCommit = CurrentRuntimeSkeleton.Get();

    const bool bSuccess =
        IsValid(Owner) && IsValid(MeshToCommit) &&
        Owner->CommitRuntimeCharacterResources(
            MeshToCommit,
            PhysicsToCommit,
            SkeletonToCommit);

    if (bSuccess)
    {
        // Ownership has moved to ACharacterController. Do not clear these committed resources in
        // FinishAndRelease; resetting the action's refs is enough.
        PendingSkeletalMesh = nullptr;
        PendingRuntimePhysicsAsset = nullptr;
        CurrentRuntimeSkeleton = nullptr;
    }
    else
    {
        ReleaseTransientRuntimeObject(PendingRuntimePhysicsAsset);
        ReleaseTransientRuntimeObject(PendingSkeletalMesh);
    }

    OnProgress.Broadcast(1.0f);
    OnCompleted.Broadcast(bSuccess);
    FinishAndRelease();
}

void UCharacterLoadAsyncAction::ScheduleGameThreadStage(
    void (UCharacterLoadAsyncAction::*StageFunction)())
{
    if (!ensureMsgf(IsInGameThread(), TEXT("Character load stage scheduling must run on the game thread")))
    {
        return;
    }

    if (bCancelled || bFinished || StageFunction == nullptr)
    {
        FinishAndRelease();
        return;
    }

    ClearGameThreadStageTimer();
    ACharacterController* Owner = OwnerCharacter.Get();
    UWorld* World = IsValid(Owner) ? Owner->GetWorld() : nullptr;
    if (!World)
    {
        FailLoad(TEXT("Character owner world became invalid while scheduling a game-thread stage"));
        return;
    }

    TWeakObjectPtr<UCharacterLoadAsyncAction> WeakThis(this);
    FTimerDelegate Delegate = FTimerDelegate::CreateLambda([WeakThis, StageFunction]()
    {
        if (UCharacterLoadAsyncAction* StrongThis = WeakThis.Get())
        {
            (StrongThis->*StageFunction)();
        }
    });
    GameThreadStageTimer = World->GetTimerManager().SetTimerForNextTick(Delegate);
}

void UCharacterLoadAsyncAction::ClearGameThreadStageTimer()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("Character load timer mutation must run on the game thread")))
    {
        return;
    }

    if (!GameThreadStageTimer.IsValid())
    {
        return;
    }

    if (ACharacterController* Owner = OwnerCharacter.Get())
    {
        if (UWorld* World = Owner->GetWorld())
        {
            World->GetTimerManager().ClearTimer(GameThreadStageTimer);
        }
    }
    GameThreadStageTimer.Invalidate();
}

bool UCharacterLoadAsyncAction::ResolveCharacterSkin(UWorldBakedModelAsset* Asset)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("ResolveCharacterSkin must run on the game thread")))
    {
        return false;
    }

    DetectedMeshIndex = INDEX_NONE;
    DetectedSkinIndex = INDEX_NONE;
    if (!IsValid(Asset))
    {
        return false;
    }

    const int32 MeshCount = Asset->GetNumMeshes();
    const TArray<FglTFRuntimeNode>& Nodes = Asset->GetNodes();
    constexpr int32 MaxSafeCharacterNodeCount = 65536;
    if (MeshCount <= 0 || Nodes.Num() <= 0 || Nodes.Num() > MaxSafeCharacterNodeCount)
    {
        return false;
    }

    for (const FglTFRuntimeNode& Node : Nodes)
    {
        if (Node.Index >= 0 && Node.Index < Nodes.Num() &&
            Node.MeshIndex >= 0 && Node.MeshIndex < MeshCount &&
            Node.SkinIndex >= 0 && Asset->HasSkin(Node.SkinIndex)
            && IsFiniteCharacterLoadTransform(Node.Transform))
        {
            DetectedMeshIndex = Node.MeshIndex;
            DetectedSkinIndex = Node.SkinIndex;
            return true;
        }
    }
    return false;
}

void UCharacterLoadAsyncAction::FailLoad(const FString& Reason)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("FailLoad must run on the game thread")))
    {
        return;
    }

    if (bFinished)
    {
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("%s"), *Reason);
    UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("CharacterLoadAsyncAction"), Reason);
    OnProgress.Broadcast(1.0f);
    OnCompleted.Broadcast(false);
    FinishAndRelease();
}

void UCharacterLoadAsyncAction::ReleaseCurrentAsset()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("ReleaseCurrentAsset must run on the game thread")))
    {
        return;
    }

    CurrentLoadedAsset = nullptr;

    ReleaseTransientRuntimeObject(PendingRuntimePhysicsAsset);
    ReleaseTransientRuntimeObject(PendingSkeletalMesh);
    ReleaseTransientRuntimeObject(CurrentRuntimeSkeleton);
    PendingRuntimePhysicsAsset = nullptr;
    PendingSkeletalMesh = nullptr;
    CurrentRuntimeSkeleton = nullptr;
    SourceSkeletonReferenceGuard = nullptr;
    SourceMaterialReferenceGuard = nullptr;
    PendingBoneMap.Empty();
}

void UCharacterLoadAsyncAction::CancelAndRelease()
{
    if (!IsInGameThread())
    {
        TWeakObjectPtr<UCharacterLoadAsyncAction> WeakThis(this);
        if (!FSafeFileIO::DispatchTrackedGameThread([WeakThis]()
        {
            if (UCharacterLoadAsyncAction* StrongThis = WeakThis.Get())
            {
                StrongThis->CancelAndRelease();
            }
        }))
        {
            // Shutdown owns final UObject teardown once new game-thread continuations are rejected.
            return;
        }
        return;
    }

    bCancelled = true;
    ClearGameThreadStageTimer();
    OnCompleted.Clear();
    OnProgress.Clear();
    OwnerCharacter.Reset();
    FilePath.Reset();

    // Keep the action alive until the outstanding range read/finalizer callback has drained.
    TryFinishCancelledRequest();
}

bool UCharacterLoadAsyncAction::HasAsyncWorkInFlight() const
{
    return bMeshLoadInFlight;
}

void UCharacterLoadAsyncAction::TryFinishCancelledRequest()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("Cancelled character-load finalization must run on the game thread")))
    {
        return;
    }

    if (bCancelled && !bFinished && !HasAsyncWorkInFlight())
    {
        FinishAndRelease();
    }
}

void UCharacterLoadAsyncAction::FinishAndRelease()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("FinishAndRelease must run on the game thread")))
    {
        return;
    }

    if (bFinished)
    {
        return;
    }

    if (HasAsyncWorkInFlight())
    {
        // Cancellation may arrive while parser/JSON/glTFRuntime work is still running. Keep this
        // action registered and its UObject references alive until every GT drain callback fires.
        bCancelled = true;
        return;
    }

    bFinished = true;
    bCancelled = true;
    bMeshLoadInFlight = false;
    ClearGameThreadStageTimer();
    ReleaseCurrentAsset();
    OnCompleted.Clear();
    OnProgress.Clear();
    OwnerCharacter.Reset();
    FilePath.Reset();
    SetReadyToDestroy();

    // The owner may have queued a newer character while this request's worker/finalizer was
    // draining. Notify it only after all parser/generated-object references have been released.
    TWeakObjectPtr<ACharacterController> Observer = ReleaseObserver;
    ReleaseObserver.Reset();
    if (ACharacterController* Character = Observer.Get())
    {
        Character->HandleCharacterLoadActionReleased(this);
    }
}

bool UCharacterLoadAsyncAction::CheckRootBoneName(UWorldBakedModelAsset* Asset)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("CheckRootBoneName must run on the game thread")))
    {
        return true;
    }

    if (!IsValid(Asset))
    {
        return true;
    }

    for (const FglTFRuntimeNode& Node : Asset->GetNodes())
    {
        if (Node.Name.Equals(BONE_ROOT))
        {
            return false;
        }
    }
    return true;
}
