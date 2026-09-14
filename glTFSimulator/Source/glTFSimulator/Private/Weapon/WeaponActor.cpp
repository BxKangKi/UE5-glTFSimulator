// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WeaponActor.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Weapon/WeaponActor.h"
#include "System/GlTFSimulatorGameInstance.h"
#include "System/GlTFSimulatorAssetRegistry.h"

#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/DamageEvents.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Model/glTFMaterialOverrideUtils.h"
#include "Simulator/RuntimeModelResolver.h"
#include "Setting/GameSettings.h"
#include "System/GameManagerSubSystem.h"
#include "System/glTFRuntimeSafety.h"
#include "System/WorldBakedModelAsset.h"
#include "System/MacroLibrary.h"
#include "System/SafeFileIO.h"
#include "Weapon/WeaponProjectileActor.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeParser.h"

namespace
{
    constexpr float DefaultWeaponVisualLengthCm = 140.0f;
    constexpr float DefaultWeaponVisualThicknessCm = 14.0f;
    constexpr int64 MaxWeaponConfigBytes = 16ll * 1024ll * 1024ll;
    constexpr int32 MaxRuntimeWeaponNodeCount = 500000;

    FglTFRuntimeStaticMeshConfig MakeWeaponMeshConfig(AWeaponActor* Actor)
    {
        FglTFRuntimeStaticMeshConfig MeshConfig;
        MeshConfig.Outer = Actor;
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
        return MeshConfig;
    }

    bool TryReadVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FVector& Value)
    {
        if (!Object.IsValid())
        {
            return false;
        }

        const TSharedPtr<FJsonObject>* VectorObject = nullptr;
        if (Object->TryGetObjectField(FieldName, VectorObject) && VectorObject && VectorObject->IsValid())
        {
            double X = Value.X;
            double Y = Value.Y;
            double Z = Value.Z;
            (*VectorObject)->TryGetNumberField(TEXT("X"), X);
            (*VectorObject)->TryGetNumberField(TEXT("Y"), Y);
            (*VectorObject)->TryGetNumberField(TEXT("Z"), Z);
            Value = FVector(X, Y, Z);
            return true;
        }

        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (Object->TryGetArrayField(FieldName, Array) && Array && Array->Num() >= 3)
        {
            Value = FVector(
                static_cast<float>((*Array)[0]->AsNumber()),
                static_cast<float>((*Array)[1]->AsNumber()),
                static_cast<float>((*Array)[2]->AsNumber()));
            return true;
        }

        return false;
    }

    bool TryReadTransform(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FTransform& Value)
    {
        if (!Object.IsValid())
        {
            return false;
        }

        const TSharedPtr<FJsonObject>* TransformObject = nullptr;
        if (!Object->TryGetObjectField(FieldName, TransformObject) || !TransformObject || !TransformObject->IsValid())
        {
            return false;
        }

        FVector Location = Value.GetLocation();
        FVector Scale = Value.GetScale3D();
        FRotator Rotation = Value.Rotator();

        double X = Location.X;
        double Y = Location.Y;
        double Z = Location.Z;
        double Pitch = Rotation.Pitch;
        double Yaw = Rotation.Yaw;
        double Roll = Rotation.Roll;
        double ScaleX = Scale.X;
        double ScaleY = Scale.Y;
        double ScaleZ = Scale.Z;
        double UniformScale = Scale.X;

        (*TransformObject)->TryGetNumberField(TEXT("X"), X);
        (*TransformObject)->TryGetNumberField(TEXT("Y"), Y);
        (*TransformObject)->TryGetNumberField(TEXT("Z"), Z);
        (*TransformObject)->TryGetNumberField(TEXT("Pitch"), Pitch);
        (*TransformObject)->TryGetNumberField(TEXT("Yaw"), Yaw);
        (*TransformObject)->TryGetNumberField(TEXT("Roll"), Roll);
        if ((*TransformObject)->TryGetNumberField(TEXT("Scale"), UniformScale))
        {
            ScaleX = UniformScale;
            ScaleY = UniformScale;
            ScaleZ = UniformScale;
        }
        (*TransformObject)->TryGetNumberField(TEXT("ScaleX"), ScaleX);
        (*TransformObject)->TryGetNumberField(TEXT("ScaleY"), ScaleY);
        (*TransformObject)->TryGetNumberField(TEXT("ScaleZ"), ScaleZ);

        Value = FTransform(FRotator(Pitch, Yaw, Roll), FVector(X, Y, Z), FVector(ScaleX, ScaleY, ScaleZ));
        return true;
    }

}

AWeaponActor::AWeaponActor()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = true;
    SetReplicateMovement(true);

    Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SetRootComponent(Root);
    ProjectileClass = AWeaponProjectileActor::StaticClass();
}

void AWeaponActor::ResolveCentralWeaponAssets()
{
    UGlTFSimulatorAssetRegistry* Registry = UGlTFSimulatorGameInstance::GetAssetRegistryFromContext(this);
    if (!IsValid(Registry))
    {
        if (!ProjectileClass)
        {
            ProjectileClass = AWeaponProjectileActor::StaticClass();
        }
        return;
    }

    if (!IsValid(DefaultWeaponMesh) && !Registry->DefaultWeaponMesh.IsNull())
    {
        DefaultWeaponMesh = Registry->DefaultWeaponMesh.LoadSynchronous();
    }

    UClass* ResolvedProjectileClass = Registry->WeaponProjectileActorClass.IsNull()
        ? nullptr : Registry->WeaponProjectileActorClass.LoadSynchronous();
    if (IsValid(ResolvedProjectileClass)
        && ResolvedProjectileClass->IsChildOf(AWeaponProjectileActor::StaticClass())
        && !ResolvedProjectileClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
    {
        ProjectileClass = ResolvedProjectileClass;
    }
    else if (!ProjectileClass)
    {
        ProjectileClass = AWeaponProjectileActor::StaticClass();
    }
}

void AWeaponActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ReleaseRuntimeResources();
    Super::EndPlay(EndPlayReason);
}

void AWeaponActor::Destroyed()
{
    ReleaseRuntimeResources();
    Super::Destroyed();
}

bool AWeaponActor::EquipFromModel(const FString& InModelReference, USceneComponent* AttachTarget)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("AWeaponActor::EquipFromModel must run on the game thread")))
    {
        return false;
    }

    // This actor can be reused when the toolbar selection changes. Release the prior baked facade
    // before assigning a new one; otherwise its manifest and weak dependency cache survive until a
    // later GC cycle even though the old weapon is no longer visible.
    ClearLoadedComponents();
    BakedAsset = nullptr;
    Config = FWeaponConfig();
    FResolvedRuntimeModel Model;
    FString ResolveError;
    const bool bResolved = FRuntimeModelResolver::Resolve(
        this, InModelReference, Model, ResolveError)
        && Model.Definition.ModelType == EModelDefinitionType::Dynamic
        && Model.Definition.ItemType == EModelItemType::Weapon;
    ModelReference = bResolved ? Model.Reference : FString();
    bRuntimeResourcesReleased = false;

    if (bResolved)
    {
        LoadConfigJson(Model.DefinitionJson);
    }
    else
    {
        UE_LOG(LogTemp, Warning,
            TEXT("WeaponActor: built weapon model rejected. Reference=%s Reason=%s"),
            *InModelReference, *ResolveError);
    }

    const bool bLoadedVisual = bResolved && LoadWeaponMesh(Model);
    if (!bLoadedVisual && IsValid(BakedAsset))
    {
        // A definition may be valid while all of its render nodes are unsupported. The default
        // mesh does not need that baked facade, so retire it before installing the fallback visual.
        BakedAsset = nullptr;
    }
    if (!bLoadedVisual && !CreateDefaultBoxMesh())
    {
        return false;
    }

    AttachToTarget(AttachTarget);
    return true;
}

bool AWeaponActor::EquipFromFile(
    const FString& InFilePath,
    USceneComponent* AttachTarget)
{
    // Compatibility only: the resolver deliberately rejects absolute/relative GLB paths. This
    // keeps old Blueprint node names loadable while preserving the baked-runtime trust boundary.
    return EquipFromModel(InFilePath, AttachTarget);
}

bool AWeaponActor::EquipDefault(USceneComponent* AttachTarget)
{
    ResolveCentralWeaponAssets();
    if (!ensureMsgf(IsInGameThread(), TEXT("AWeaponActor::EquipDefault must run on the game thread")))
    {
        return false;
    }

    ClearLoadedComponents();
    BakedAsset = nullptr;
    Config = FWeaponConfig();
    ModelReference.Reset();
    bRuntimeResourcesReleased = false;
    if (!CreateDefaultBoxMesh())
    {
        return false;
    }

    AttachToTarget(AttachTarget);
    return true;
}

bool AWeaponActor::LoadConfigJson(const FString& DefinitionJson)
{
    FSafeJsonLimits Limits;
    Limits.MaxFileBytes = MaxWeaponConfigBytes;
    Limits.MaxDepth = 32;
    Limits.MaxValues = 100000;
    Limits.MaxContainerEntries = 50000;
    Limits.MaxStringCharacters = 1024 * 1024;
    Limits.bAllowBackupRecovery = false;

    const FSafeJsonLoadResult LoadResult = FSafeFileIO::ParseJsonText(
        DefinitionJson, TEXT(".gwd weapon definition"), Limits);
    if (!LoadResult.IsSuccess())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("WeaponActor: embedded definition JSON was not loaded. Reason=%s"),
            *LoadResult.Error);
        return false;
    }

    const TSharedPtr<FJsonObject>& RootObject = LoadResult.JsonObject;
    if (!RootObject.IsValid())
    {
        return false;
    }

    RootObject->TryGetStringField(JSON_VERSION_FIELD, Config.Version);

    FString SocketName;
    if (RootObject->TryGetStringField(TEXT("AttachSocketName"), SocketName) || RootObject->TryGetStringField(TEXT("Socket"), SocketName))
    {
        Config.AttachSocketName = FName(SocketName);
    }

    TryReadTransform(RootObject, TEXT("Hold"), Config.HoldTransform);
    TryReadTransform(RootObject, TEXT("HoldTransform"), Config.HoldTransform);
    TryReadTransform(RootObject, TEXT("RightHandIK"), Config.RightHandIK);
    TryReadTransform(RootObject, TEXT("RightHand"), Config.RightHandIK);
    TryReadTransform(RootObject, TEXT("LeftHandIK"), Config.LeftHandIK);
    TryReadTransform(RootObject, TEXT("LeftHand"), Config.LeftHandIK);
    TryReadVector(RootObject, TEXT("Muzzle"), Config.MuzzleOffset);
    TryReadVector(RootObject, TEXT("MuzzleOffset"), Config.MuzzleOffset);

    RootObject->TryGetNumberField(TEXT("Range"), Config.Range);
    RootObject->TryGetNumberField(TEXT("Damage"), Config.Damage);
    RootObject->TryGetNumberField(TEXT("AttackPower"), Config.Damage);
    RootObject->TryGetNumberField(TEXT("ImpactImpulse"), Config.ImpactImpulse);
    RootObject->TryGetNumberField(TEXT("FireInterval"), Config.FireInterval);
    RootObject->TryGetNumberField(TEXT("TraceRadius"), Config.TraceRadius);
    RootObject->TryGetNumberField(TEXT("ProjectileSpeed"), Config.ProjectileSpeed);
    RootObject->TryGetNumberField(TEXT("ProjectileLifeSeconds"), Config.ProjectileLifeSeconds);
    RootObject->TryGetBoolField(TEXT("bProjectile"), Config.bProjectile);
    RootObject->TryGetBoolField(TEXT("Projectile"), Config.bProjectile);

    Config.Range = FMath::Max(1.0f, Config.Range);
    Config.Damage = FMath::Max(0.0f, Config.Damage);
    Config.ImpactImpulse = FMath::Max(0.0f, Config.ImpactImpulse);
    Config.FireInterval = FMath::Max(0.01f, Config.FireInterval);
    Config.TraceRadius = FMath::Max(0.0f, Config.TraceRadius);
    Config.ProjectileSpeed = FMath::Max(100.0f, Config.ProjectileSpeed);
    Config.ProjectileLifeSeconds = FMath::Max(0.1f, Config.ProjectileLifeSeconds);
    return true;
}

void AWeaponActor::ReleaseRuntimeResources()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("AWeaponActor runtime release must run on the game thread"))
        || bRuntimeResourcesReleased)
    {
        return;
    }
    bRuntimeResourcesReleased = true;

    ClearLoadedComponents();
    BakedAsset = nullptr;
}

void AWeaponActor::ClearLoadedComponents()
{
    ++AsyncMeshLoadGeneration;
    PendingAsyncMeshLoads = 0;
    bAsyncMeshLoadFailed = false;
    bResumingAsyncMeshLoad = false;
    bDispatchingAsyncMeshPreload = false;
    PendingAsyncModelReference.Reset();
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
            RemoveInstanceComponent(Component);
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
            Mesh->ClearFlags(RF_Public | RF_Standalone);
        }
    }

    MeshComponents.Empty();
    MeshCache.Empty();
}

UStaticMesh* AWeaponActor::LoadMeshByIndex(int32 MeshIndex)
{
    if (!IsValid(BakedAsset) || MeshIndex < 0 || MeshIndex >= BakedAsset->GetNumMeshes())
    {
        return nullptr;
    }

    if (TObjectPtr<UStaticMesh>* Existing = MeshCache.Find(MeshIndex))
    {
        return Existing->Get();
    }

    // Never hide an async-preload coverage bug behind a synchronous RuntimeLOD build. The latter
    // can stall the game thread for an entire mesh, so a cache miss is treated as an assembly error.
    UE_LOG(LogTemp, Error,
        TEXT("WeaponActor: preloaded mesh cache miss; refusing synchronous fallback. Model=%s Mesh=%d"),
        *ModelReference, MeshIndex);
    return nullptr;
}

bool AWeaponActor::BeginAsyncMeshPreload()
{
    check(IsInGameThread());
    if (!IsValid(BakedAsset)) return false;

    TSet<int32> UniqueMeshIndices;
    const int32 MeshCount = BakedAsset->GetNumMeshes();
    for (const FglTFRuntimeNode& Node : BakedAsset->GetNodes())
    {
        if (Node.MeshIndex >= 0 && Node.MeshIndex < MeshCount) UniqueMeshIndices.Add(Node.MeshIndex);
    }
    TArray<int32> MissingMeshIndices;
    for (const int32 MeshIndex : UniqueMeshIndices)
    {
        if (TObjectPtr<UStaticMesh>* Existing = MeshCache.Find(MeshIndex); Existing && IsValid(Existing->Get())) continue;
        MissingMeshIndices.Add(MeshIndex);
    }
    if (MissingMeshIndices.IsEmpty()) return false;

    const uint64 Generation = ++AsyncMeshLoadGeneration;
    PendingAsyncMeshLoads = MissingMeshIndices.Num();
    bAsyncMeshLoadFailed = false;
    bDispatchingAsyncMeshPreload = true;
    PendingAsyncModelReference = ModelReference;
    const FglTFRuntimeStaticMeshConfig MeshConfig = MakeWeaponMeshConfig(this);
    TWeakObjectPtr<AWeaponActor> WeakThis(this);
    for (const int32 MeshIndex : MissingMeshIndices)
    {
        BakedAsset->LoadStaticMeshAsyncNative(
            MeshIndex,
            [WeakThis, Generation, MeshIndex](UStaticMesh* Mesh)
            {
                if (AWeaponActor* Self = WeakThis.Get())
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

void AWeaponActor::HandleAsyncMeshPreloadResult(
    const uint64 Generation, const int32 MeshIndex, UStaticMesh* Mesh)
{
    check(IsInGameThread());
    if (Generation != AsyncMeshLoadGeneration || PendingAsyncMeshLoads <= 0) return;
    if (IsValid(Mesh)) MeshCache.Add(MeshIndex, Mesh);
    else bAsyncMeshLoadFailed = true;
    --PendingAsyncMeshLoads;
    if (PendingAsyncMeshLoads == 0 && !bDispatchingAsyncMeshPreload) FinishAsyncMeshPreload(Generation);
}

void AWeaponActor::FinishAsyncMeshPreload(const uint64 Generation)
{
    check(IsInGameThread());
    if (Generation != AsyncMeshLoadGeneration || PendingAsyncMeshLoads != 0) return;
    if (bAsyncMeshLoadFailed)
    {
        UE_LOG(LogTemp, Warning, TEXT("WeaponActor: asynchronous mesh preload failed: %s"), *PendingAsyncModelReference);
        BakedAsset = nullptr;
        CreateDefaultBoxMesh();
        return;
    }

    FResolvedRuntimeModel Model;
    FString ResolveError;
    if (!FRuntimeModelResolver::Resolve(this, PendingAsyncModelReference, Model, ResolveError))
    {
        UE_LOG(LogTemp, Warning, TEXT("WeaponActor: async-preloaded model no longer resolves: %s"), *ResolveError);
        BakedAsset = nullptr;
        CreateDefaultBoxMesh();
        return;
    }

    bResumingAsyncMeshLoad = true;
    const bool bFinished = LoadWeaponMesh(Model);
    bResumingAsyncMeshLoad = false;
    if (!bFinished) CreateDefaultBoxMesh();
}

bool AWeaponActor::LoadWeaponMesh(const FResolvedRuntimeModel& Model)
{
    const bool bResumeAsyncAssembly = bResumingAsyncMeshLoad
        && IsValid(BakedAsset)
        && Model.Reference == ModelReference;
    if (!bResumeAsyncAssembly)
    {
        ClearLoadedComponents();
    }

    FString LoadError;
    if (!bResumeAsyncAssembly)
    {
        BakedAsset = FRuntimeModelResolver::LoadAssetSynchronously(Model, LoadError);
        if (!IsValid(BakedAsset))
        {
            UE_LOG(LogTemp, Warning, TEXT("WeaponActor: archive model load failed: %s"), *LoadError);
            return false;
        }
    }

    int32 ComponentIndex = 0;
    // The facade remains strongly referenced throughout assembly, so a const view is safe and
    // prevents one full node-table copy per equipped weapon.
    const TArray<FglTFRuntimeNode>& Nodes = BakedAsset->GetNodes();
    if (Nodes.Num() > MaxRuntimeWeaponNodeCount)
    {
        UE_LOG(LogTemp, Warning, TEXT("WeaponActor: baked node count exceeds the runtime safety limit. Path=%s Nodes=%d"),
            *ModelReference, Nodes.Num());
        // The caller may install the lightweight default visual after this failure. Do not mark the
        // whole actor as permanently released here, otherwise EndPlay would skip that fallback's cleanup.
        return false;
    }
    const int32 MeshCount = BakedAsset->GetNumMeshes();
    if (!bResumeAsyncAssembly && BeginAsyncMeshPreload())
    {
        // EquipFromModel can attach the actor immediately; visual components arrive when the
        // worker-prepared meshes have crossed the serialized finalization gate.
        return true;
    }
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

        UStaticMeshComponent* MeshComponent = NewObject<UStaticMeshComponent>(this, *FString::Printf(TEXT("WeaponMesh_%d"), ComponentIndex++));
        AddInstanceComponent(MeshComponent);
        MeshComponent->SetMobility(EComponentMobility::Movable);
        MeshComponent->SetupAttachment(Root);
        MeshComponent->SetStaticMesh(Mesh);
        MeshComponent->SetRelativeTransform(Node.Transform);
        MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        MeshComponent->SetGenerateOverlapEvents(false);
        MeshComponent->RegisterComponent();
        MeshComponents.Add(MeshComponent);
    }
    return MeshComponents.Num() > 0;
}

bool AWeaponActor::CreateDefaultBoxMesh()
{
    ClearLoadedComponents();

    if (!IsValid(DefaultWeaponMesh))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("WeaponActor cannot create its default visual because DefaultWeaponMesh is not assigned."));
        return false;
    }

    UStaticMeshComponent* MeshComponent = NewObject<UStaticMeshComponent>(this, TEXT("DefaultWeaponBox"));
    AddInstanceComponent(MeshComponent);
    MeshComponent->SetMobility(EComponentMobility::Movable);
    MeshComponent->SetupAttachment(Root);
    MeshComponent->SetStaticMesh(DefaultWeaponMesh);
    MeshComponent->SetRelativeLocation(FVector(DefaultWeaponVisualLengthCm * 0.5f, 0.0f, 0.0f));
    MeshComponent->SetRelativeScale3D(FVector(
        DefaultWeaponVisualLengthCm / 100.0f,
        DefaultWeaponVisualThicknessCm / 100.0f,
        DefaultWeaponVisualThicknessCm / 100.0f));
    MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    MeshComponent->SetGenerateOverlapEvents(false);
    MeshComponent->RegisterComponent();
    MeshComponents.Add(MeshComponent);
    return true;
}

void AWeaponActor::AttachToTarget(USceneComponent* AttachTarget)
{
    if (!IsValid(AttachTarget))
    {
        return;
    }

    if (USkeletalMeshComponent* SkeletalTarget = Cast<USkeletalMeshComponent>(AttachTarget))
    {
        AttachToComponent(SkeletalTarget, FAttachmentTransformRules::SnapToTargetNotIncludingScale, Config.AttachSocketName);
    }
    else
    {
        AttachToComponent(AttachTarget, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
    }

    SetActorRelativeTransform(Config.HoldTransform);
}

FVector AWeaponActor::GetMuzzleWorldLocation() const
{
    return GetActorTransform().TransformPosition(Config.MuzzleOffset);
}

FTransform AWeaponActor::GetRightHandIKWorldTransform() const
{
    return Config.RightHandIK * GetActorTransform();
}

FTransform AWeaponActor::GetLeftHandIKWorldTransform() const
{
    return Config.LeftHandIK * GetActorTransform();
}

void AWeaponActor::Fire(AController* InstigatorController)
{
    ResolveCentralWeaponAssets();
    UWorld* World = GetWorld();
    if (!World || !IsValid(InstigatorController))
    {
        return;
    }

    const double Now = World->GetTimeSeconds();
    if (Now - LastFireTime < Config.FireInterval)
    {
        return;
    }
    LastFireTime = Now;

    FVector ViewLocation = GetMuzzleWorldLocation();
    FRotator ViewRotation = GetActorRotation();
    InstigatorController->GetPlayerViewPoint(ViewLocation, ViewRotation);

    const FVector MuzzleLocation = GetMuzzleWorldLocation();
    const FVector ShotDirection = ViewRotation.Vector().GetSafeNormal();
    if (ShotDirection.IsNearlyZero())
    {
        return;
    }

    if (Config.bProjectile)
    {
        UClass* SpawnClass = ProjectileClass ? ProjectileClass.Get() : AWeaponProjectileActor::StaticClass();
        FActorSpawnParameters Params;
        Params.Owner = this;
        Params.Instigator = InstigatorController->GetPawn();
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

        AWeaponProjectileActor* Projectile = World->SpawnActor<AWeaponProjectileActor>(SpawnClass, MuzzleLocation, ShotDirection.Rotation(), Params);
        if (IsValid(Projectile))
        {
            Projectile->InitProjectile(InstigatorController, Config.Damage, Config.ImpactImpulse, Config.ProjectileLifeSeconds, ShotDirection * Config.ProjectileSpeed);
        }
        return;
    }

    const FVector End = ViewLocation + ShotDirection * Config.Range;

    FCollisionQueryParams Params(SCENE_QUERY_STAT(WeaponFireTrace), true, this);
    Params.bReturnPhysicalMaterial = true;
    if (APawn* Pawn = InstigatorController->GetPawn())
    {
        Params.AddIgnoredActor(Pawn);
    }
    Params.AddIgnoredActor(this);

    FHitResult Hit;
    if (Config.TraceRadius > KINDA_SMALL_NUMBER)
    {
        World->SweepSingleByChannel(Hit, ViewLocation, End, FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(Config.TraceRadius), Params);
    }
    else
    {
        World->LineTraceSingleByChannel(Hit, ViewLocation, End, ECC_Visibility, Params);
    }

    const FVector TraceEnd = Hit.bBlockingHit ? Hit.ImpactPoint : End;
#if ENABLE_DRAW_DEBUG
    DrawDebugLine(World, MuzzleLocation, TraceEnd, FColor::Red, false, 0.75f, 0, 1.5f);
#endif

    if (!Hit.bBlockingHit)
    {
        return;
    }

    if (AActor* HitActor = Hit.GetActor())
    {
        UGameplayStatics::ApplyPointDamage(HitActor, Config.Damage, ShotDirection, Hit, InstigatorController, this, nullptr);
    }

    if (UPrimitiveComponent* HitComponent = Hit.GetComponent())
    {
        if (HitComponent->IsSimulatingPhysics())
        {
            HitComponent->AddImpulseAtLocation(ShotDirection * Config.ImpactImpulse, Hit.ImpactPoint, Hit.BoneName);
        }
    }
}
