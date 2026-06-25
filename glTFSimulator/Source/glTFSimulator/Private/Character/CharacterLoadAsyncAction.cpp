// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "Character/CharacterLoadAsyncAction.h"
#include "System/FileFunctionLibrary.h"
#include "System/MacroLibrary.h"
#include "Character/CharacterController.h"
#include "Character/CharacterFunctionLibrary.h"
#include "JsonObjectConverter.h"
#include "glTFRuntimeFunctionLibrary.h"

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
    if (!OwnerCharacter.IsValid() || !UFileFunctionLibrary::CheckFile(FilePath))
    {
        OnCompleted.Broadcast(false);
        SetReadyToDestroy();
        return;
    }
    LoadAssetAsync();
}

void UCharacterLoadAsyncAction::LoadAssetAsync()
{
    FglTFRuntimeHttpResponse AssetLoadedDelegate;
    AssetLoadedDelegate.BindDynamic(this, &UCharacterLoadAsyncAction::OnglTFAssetLoaded);
    FglTFRuntimeConfig Config;
    Config.TransformBaseType = EglTFRuntimeTransformBaseType::YForward;
    Config.bAllowExternalFiles = true;
    UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilenameAsync(FilePath, false, Config, AssetLoadedDelegate);
}

void UCharacterLoadAsyncAction::OnglTFAssetLoaded(UglTFRuntimeAsset *Asset)
{
    if (!Asset)
    {
        OnCompleted.Broadcast(false);
        SetReadyToDestroy();
        return;
    }
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
                if (!StrongThis->CurrentLoadedAsset || !StrongThis->OwnerCharacter.IsValid())
                {
                    StrongThis->OnCompleted.Broadcast(false);
                    StrongThis->SetReadyToDestroy();
                    return;
                }
                ACharacterController *Owner = StrongThis->OwnerCharacter.Get();
                USkeleton *Skeleton = Owner->DefaultAsset.Skeleton;
                UMaterialInterface *Material = Owner->DefaultAsset.Material;
                if (!IsValid(Skeleton) || !IsValid(Material))
                {
                    StrongThis->OnCompleted.Broadcast(false);
                    StrongThis->SetReadyToDestroy();
                    return;
                }
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
                Config.MaterialsConfig.bGeneratesMipMaps = true;
                Config.MaterialsConfig.SpecularFactor = 0.0f;
                Config.MaterialsConfig.ImagesConfig.MaxWidth = 1024;
                Config.MaterialsConfig.ImagesConfig.MaxHeight = 1024;
                Config.MaterialsConfig.ImagesConfig.bCompressMips = true;
                Config.MaterialsConfig.ImagesConfig.bStreaming = true;
                Config.MaterialsConfig.bLoadMipMaps = true;
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
                StrongThis->CurrentLoadedAsset->LoadSkeletalMeshRecursiveAsync(TEXT(""), {}, MeshDelegate, Config, EglTFRuntimeRecursiveMode::Ignore);
            }
        }); });
}

void UCharacterLoadAsyncAction::OnMeshLoaded(USkeletalMesh *SkeletalMesh)
{
    if (SkeletalMesh && OwnerCharacter.IsValid())
    {
        FinalizePhysics(SkeletalMesh);
        OnCompleted.Broadcast(true);
    }
    else
    {
        OnCompleted.Broadcast(false);
    }

    SetReadyToDestroy();
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