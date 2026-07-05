// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Character/CharacterLoadAsyncAction.h"
#include "Async/Async.h"
#include "System/FileFunctionLibrary.h"
#include "System/MacroLibrary.h"
#include "Character/CharacterController.h"
#include "Character/CharacterFunctionLibrary.h"
#include "JsonObjectConverter.h"
#include "glTFRuntimeFunctionLibrary.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeParser.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/CoreMisc.h"

UCharacterLoadAsyncAction *UCharacterLoadAsyncAction::LoadCharacterAsync(UObject *WorldContextObject, ACharacterController *InOwner, FString InPath)
{
    UCharacterLoadAsyncAction *Action = NewObject<UCharacterLoadAsyncAction>();
    Action->OwnerCharacter = InOwner;
    Action->FilePath = InPath;
    Action->RegisterWithGameInstance(WorldContextObject);
    return Action;
}

void UCharacterLoadAsyncAction::Activate()
{
    bCancelled = false;
    bFinished = false;
    bMeshLoadInFlight = false;
    CancelActiveAssetLoad();
    CurrentLoadedAsset = nullptr;
    OnProgress.Broadcast(0.0f);

    if (!OwnerCharacter.IsValid() || !UFileFunctionLibrary::CheckFile(FilePath))
    {
        OnProgress.Broadcast(1.0f);
        OnCompleted.Broadcast(false);
        FinishAndRelease();
        return;
    }
    LoadAssetAsync();
}

void UCharacterLoadAsyncAction::LoadAssetAsync()
{
    if (bCancelled)
    {
        FinishAndRelease();
        return;
    }

    CancelActiveAssetLoad();

    // Character changes must rebuild the full runtime character path every time:
    // fresh glTFAsset -> bone map -> skeletal mesh -> generated/merged PhysicsAsset.
    // Parsing remains asynchronous. UObjects are created and SetParser() is called only
    // on the game thread after the owner, path, request serial and cancellation token are
    // still current, so cancelling during load prevents all owner mutation and callbacks.
    const int32 RequestId = AssetLoadRequestSerial;
    const FString RequestedFilePath = FilePath;

    FglTFRuntimeConfig Config;
    Config.TransformBaseType = EglTFRuntimeTransformBaseType::YForward;
    Config.bAllowExternalFiles = true;

    TWeakObjectPtr<UCharacterLoadAsyncAction> WeakThis(this);
    TSharedPtr<FThreadSafeCounter, ESPMode::ThreadSafe> CancelToken = MakeShared<FThreadSafeCounter, ESPMode::ThreadSafe>(0);
    AssetLoadCancelToken = CancelToken;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakThis, RequestedFilePath, RequestId, Config, CancelToken]()
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

        AsyncTask(ENamedThreads::GameThread, [WeakThis, RequestedFilePath, RequestId, Config, Parser, CancelToken]()
        {
            UCharacterLoadAsyncAction* StrongThis = WeakThis.Get();
            const bool bRequestStillCurrent =
                CancelToken.IsValid() &&
                CancelToken->GetValue() == 0 &&
                IsValid(StrongThis) &&
                !StrongThis->bCancelled &&
                !StrongThis->bFinished &&
                StrongThis->AssetLoadCancelToken == CancelToken &&
                StrongThis->AssetLoadRequestSerial == RequestId &&
                StrongThis->OwnerCharacter.IsValid() &&
                StrongThis->FilePath == RequestedFilePath &&
                !IsGarbageCollecting();

            if (!bRequestStillCurrent)
            {
                return;
            }

            UglTFRuntimeAsset* LoadedAsset = nullptr;
            if (Parser.IsValid())
            {
                UObject* AssetOuter = StrongThis->OwnerCharacter.Get();
                LoadedAsset = NewObject<UglTFRuntimeAsset>(AssetOuter ? AssetOuter : StrongThis);
                if (LoadedAsset)
                {
                    LoadedAsset->RuntimeContextObject = Config.RuntimeContextObject;
                    LoadedAsset->RuntimeContextString = Config.RuntimeContextString;
                    if (!LoadedAsset->SetParser(Parser.ToSharedRef()))
                    {
                        LoadedAsset->ClearCache();
                        LoadedAsset->MarkAsGarbage();
                        LoadedAsset = nullptr;
                    }
                }
            }

            StrongThis = WeakThis.Get();
            if (!IsValid(StrongThis) || StrongThis->bCancelled || StrongThis->bFinished || StrongThis->AssetLoadCancelToken != CancelToken ||
                StrongThis->AssetLoadRequestSerial != RequestId || CancelToken->GetValue() != 0 ||
                !StrongThis->OwnerCharacter.IsValid() || StrongThis->FilePath != RequestedFilePath)
            {
                if (IsValid(LoadedAsset))
                {
                    LoadedAsset->ClearCache();
                    LoadedAsset->MarkAsGarbage();
                }
                return;
            }

            StrongThis->AssetLoadCancelToken.Reset();
            StrongThis->OnglTFAssetLoaded(LoadedAsset);
        });
    });
}

void UCharacterLoadAsyncAction::OnglTFAssetLoaded(UglTFRuntimeAsset *Asset)
{
    if (bCancelled)
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
        FinishAndRelease();
        return;
    }

    if (!Asset)
    {
        OnProgress.Broadcast(1.0f);
        OnCompleted.Broadcast(false);
        FinishAndRelease();
        return;
    }

    if (!OwnerCharacter.IsValid())
    {
        CurrentLoadedAsset = Asset;
        OnProgress.Broadcast(1.0f);
        OnCompleted.Broadcast(false);
        FinishAndRelease();
        return;
    }

    OnProgress.Broadcast(0.25f);
    CurrentLoadedAsset = Asset;
    LoadBoneMapAsync();
}

void UCharacterLoadAsyncAction::LoadBoneMapAsync()
{
    TWeakObjectPtr<UCharacterLoadAsyncAction> WeakThis(this);
    FString JsonPath = UFileFunctionLibrary::GetPathWithoutExtension(FilePath) + TEXT(".json");
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakThis, JsonPath]()
              {
        TMap<FString, FString> LocalBoneMap;
        TSharedPtr<FJsonObject> Json = UFileFunctionLibrary::FromJson(JsonPath);
        if (Json.IsValid())
        {
            for (auto& Pair : Json->Values)
            {
                FString BoneValue;
                if (Pair.Value->TryGetString(BoneValue))
                {
                    LocalBoneMap.Add(BoneValue, Pair.Key);
                }
            }
        }
        AsyncTask(ENamedThreads::GameThread, [WeakThis, LocalBoneMap]()
        {
            if (UCharacterLoadAsyncAction* StrongThis = WeakThis.Get())
            {
                if (StrongThis->bCancelled || StrongThis->bFinished)
                {
                    StrongThis->FinishAndRelease();
                    return;
                }

                if (!StrongThis->CurrentLoadedAsset || !StrongThis->OwnerCharacter.IsValid())
                {
                    StrongThis->OnProgress.Broadcast(1.0f);
                    StrongThis->OnCompleted.Broadcast(false);
                    StrongThis->FinishAndRelease();
                    return;
                }
                ACharacterController *Owner = StrongThis->OwnerCharacter.Get();
                USkeleton *Skeleton = Owner->DefaultAsset.Skeleton;
                UMaterialInterface *Material = Owner->DefaultAsset.Material;
                if (!IsValid(Skeleton) || !IsValid(Material))
                {
                    StrongThis->OnProgress.Broadcast(1.0f);
                    StrongThis->OnCompleted.Broadcast(false);
                    StrongThis->FinishAndRelease();
                    return;
                }
                StrongThis->OnProgress.Broadcast(0.45f);
                // Merge skeleton and set up mesh loading.
                FglTFRuntimeSkeletalMeshConfig Config;
                Config.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
                Config.bOverwriteRefSkeleton = false;
                Config.bMergeAllBonesToBoneTree = false;
                Config.bIgnoreSkin = false;
                Config.OverrideSkinIndex = -1;
                Config.SkeletonConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
                Config.SkeletonConfig.bAddRootBone = StrongThis->CheckRootBoneName(StrongThis->CurrentLoadedAsset);
                Config.SkeletonConfig.RootBoneName = TEXT("Root");
                Config.SkeletonConfig.BonesNameMap = LocalBoneMap;
                Config.SkeletonConfig.RootNodeIndex = -1;
                Config.SkeletonConfig.bClearRotations = true;
                Config.SkeletonConfig.CopyRotationsFrom = Skeleton;
                Config.SkeletonConfig.MaxNodesTreeDepth = -1;
                Config.SkeletonConfig.bAddRootNodeIfMissing = true;
                Config.MaterialsConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
                TMap<EglTFRuntimeMaterialType, UMaterialInterface*> UberMaterialsOverrideMap;
                UberMaterialsOverrideMap.Add(EglTFRuntimeMaterialType::Opaque, Material);
                UberMaterialsOverrideMap.Add(EglTFRuntimeMaterialType::Translucent, Material);
                UberMaterialsOverrideMap.Add(EglTFRuntimeMaterialType::TwoSided, Material);
                UberMaterialsOverrideMap.Add(EglTFRuntimeMaterialType::TwoSidedTranslucent, Material);
                UberMaterialsOverrideMap.Add(EglTFRuntimeMaterialType::Masked, Material);
                UberMaterialsOverrideMap.Add(EglTFRuntimeMaterialType::TwoSidedMasked, Material);
                Config.MaterialsConfig.UberMaterialsOverrideMap = UberMaterialsOverrideMap;
                Config.MaterialsConfig.UnlitOverrideMap = UberMaterialsOverrideMap;
                Config.MaterialsConfig.bGeneratesMipMaps = false;
                Config.MaterialsConfig.SpecularFactor = 0.0f;
                Config.MaterialsConfig.ImagesConfig.MaxWidth = 1024;
                Config.MaterialsConfig.ImagesConfig.MaxHeight = 1024;
                Config.MaterialsConfig.ImagesConfig.bCompressMips = false;
                Config.MaterialsConfig.ImagesConfig.bStreaming = false;
                Config.MaterialsConfig.bLoadMipMaps = false;
                Config.bIgnoreMissingBones = true;
                Config.Outer = Owner;
                Config.bIgnoreEmptyMorphTargets = true;
                Config.bAutoGeneratePhysicsAssetBodies = true;
                Config.PhysicsAssetAutoBodyConfig.CollisionType = EglTFRuntimePhysicsAssetAutoBodyCollisionType::Sphere;
                Config.PhysicsAssetAutoBodyConfig.MinBoneSize = 12.0;
                Config.PhysicsAssetAutoBodyConfig.bDisableOverlappingCollisions = true;
                Config.PhysicsAssetAutoBodyConfig.bDisableAllCollisions = true;
                Config.PhysicsAssetAutoBodyConfig.bConsiderForBounds = true;
                Config.PhysicsAssetAutoBodyConfig.CollisionScale = 1.01f;
                Config.bAllowCPUAccess = true;
                // Load skeleton and merge based on Owner's Asset information.
                USkeleton *TargetSkel = StrongThis->CurrentLoadedAsset->LoadSkeleton(0, Config.SkeletonConfig);
                USkeleton *MergedSkel = UCharacterFunctionLibrary::MergeSkeleton(Skeleton, TargetSkel);
                Config.Skeleton = MergedSkel;
                FglTFRuntimeSkeletalMeshAsync MeshDelegate;
                MeshDelegate.BindDynamic(StrongThis, &UCharacterLoadAsyncAction::OnMeshLoaded);
                StrongThis->OnProgress.Broadcast(0.65f);
                StrongThis->bMeshLoadInFlight = true;
                StrongThis->CurrentLoadedAsset->LoadSkeletalMeshRecursiveAsync(TEXT(""), {}, MeshDelegate, Config, EglTFRuntimeRecursiveMode::Ignore);
            }
        }); });
}

void UCharacterLoadAsyncAction::OnMeshLoaded(USkeletalMesh *SkeletalMesh)
{
    bMeshLoadInFlight = false;

    if (bCancelled || bFinished)
    {
        if (IsValid(SkeletalMesh) && !SkeletalMesh->IsAsset())
        {
            SkeletalMesh->MarkAsGarbage();
        }
        FinishAndRelease();
        return;
    }

    OnProgress.Broadcast(1.0f);

    if (SkeletalMesh && OwnerCharacter.IsValid())
    {
        FinalizePhysics(SkeletalMesh);
        OnCompleted.Broadcast(true);
    }
    else
    {
        if (IsValid(SkeletalMesh) && !SkeletalMesh->IsAsset())
        {
            SkeletalMesh->MarkAsGarbage();
        }
        OnCompleted.Broadcast(false);
    }

    FinishAndRelease();
}

void UCharacterLoadAsyncAction::FinalizePhysics(USkeletalMesh *SkeletalMesh)
{
    auto Owner = OwnerCharacter.Get();
    USkeletalMeshComponent *MeshComp = Owner->GetMesh();
    MeshComp->SetSkinnedAssetAndUpdate(SkeletalMesh, true);
    UPhysicsAsset *TargetPA = MeshComp->GetPhysicsAsset();
    if (TargetPA)
    {
        UCharacterFunctionLibrary::SetupAllBodiesBelowCollidersAndConstraints(TargetPA, MeshComp, BONE_HAIR_ROOT);
        UCharacterFunctionLibrary::SetupAllBodiesBelowCollidersAndConstraints(TargetPA, MeshComp, BONE_DYN_ROOT);
        UPhysicsAsset *MergedPA = UCharacterFunctionLibrary::MergePhysicsAsset(TargetPA, Owner->DefaultAsset.PhysicsAsset);
        MeshComp->SetPhysicsAsset(MergedPA, true);
        MeshComp->SetCollisionProfileName(RAGDOLL);
        MeshComp->RecreatePhysicsState();
        UCharacterFunctionLibrary::BlendRagdoll(*MeshComp, 0.0f);
    }
}


void UCharacterLoadAsyncAction::CancelActiveAssetLoad()
{
    if (AssetLoadCancelToken.IsValid())
    {
        AssetLoadCancelToken->Set(1);
        AssetLoadCancelToken.Reset();
    }

    ++AssetLoadRequestSerial;
}

void UCharacterLoadAsyncAction::ReleaseCurrentAsset()
{
    if (IsValid(CurrentLoadedAsset))
    {
        CurrentLoadedAsset->ClearCache();
        if (CurrentLoadedAsset->IsRooted())
        {
            CurrentLoadedAsset->RemoveFromRoot();
        }
        CurrentLoadedAsset->ClearFlags(RF_Public | RF_Standalone);
        CurrentLoadedAsset->MarkAsGarbage();
    }
    CurrentLoadedAsset = nullptr;
}

void UCharacterLoadAsyncAction::CancelAndRelease()
{
    bCancelled = true;
    CancelActiveAssetLoad();
    OnCompleted.Clear();
    OnProgress.Clear();
    OwnerCharacter.Reset();
    FilePath.Reset();

    // If glTFRuntime is already generating the skeletal mesh, keep this action and its
    // runtime asset alive until OnMeshLoaded returns. The cancel state prevents owner
    // mutation or delegate broadcasts, while avoiding use-after-free in the plugin task.
    if (!bMeshLoadInFlight)
    {
        FinishAndRelease();
    }
}

void UCharacterLoadAsyncAction::FinishAndRelease()
{
    if (bFinished)
    {
        return;
    }

    bFinished = true;
    bCancelled = true;
    CancelActiveAssetLoad();
    ReleaseCurrentAsset();
    OnCompleted.Clear();
    OnProgress.Clear();
    OwnerCharacter.Reset();
    FilePath.Reset();
    SetReadyToDestroy();
}


bool UCharacterLoadAsyncAction::CheckRootBoneName(UglTFRuntimeAsset *Asset)
{
    if (!Asset)
        return true;
    for (const FglTFRuntimeNode &Node : Asset->GetNodes())
    {
        if (Node.Name.Equals(BONE_ROOT))
            return false;
    }
    return true;
}