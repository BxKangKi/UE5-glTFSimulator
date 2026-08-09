// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Weapon/WeaponActor.h"

#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/DamageEvents.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Model/glTFMaterialOverrideUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Setting/GameSettings.h"
#include "System/GlbValidation.h"
#include "System/glTFRuntimeSafety.h"
#include "System/MacroLibrary.h"
#include "Weapon/WeaponProjectileActor.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeFunctionLibrary.h"
#include "glTFRuntimeParser.h"

namespace
{
    constexpr float DefaultWeaponVisualLengthCm = 140.0f;
    constexpr float DefaultWeaponVisualThicknessCm = 14.0f;
    constexpr int64 MaxWeaponConfigBytes = 16ll * 1024ll * 1024ll;
    constexpr int32 MaxRuntimeWeaponNodeCount = 500000;

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

    TSharedRef<FJsonObject> MakeTransformJson(const FTransform& Transform)
    {
        const FVector Location = Transform.GetLocation();
        const FRotator Rotation = Transform.Rotator();
        const FVector Scale = Transform.GetScale3D();

        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("X"), Location.X);
        Object->SetNumberField(TEXT("Y"), Location.Y);
        Object->SetNumberField(TEXT("Z"), Location.Z);
        Object->SetNumberField(TEXT("Pitch"), Rotation.Pitch);
        Object->SetNumberField(TEXT("Yaw"), Rotation.Yaw);
        Object->SetNumberField(TEXT("Roll"), Rotation.Roll);
        Object->SetNumberField(TEXT("ScaleX"), Scale.X);
        Object->SetNumberField(TEXT("ScaleY"), Scale.Y);
        Object->SetNumberField(TEXT("ScaleZ"), Scale.Z);
        return Object;
    }

    TSharedRef<FJsonObject> MakeVectorJson(const FVector& Vector)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("X"), Vector.X);
        Object->SetNumberField(TEXT("Y"), Vector.Y);
        Object->SetNumberField(TEXT("Z"), Vector.Z);
        return Object;
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

bool AWeaponActor::EquipFromFile(const FString& InFilePath, USceneComponent* AttachTarget)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("AWeaponActor::EquipFromFile must run on the game thread")))
    {
        return false;
    }

    Config = FWeaponConfig();
    SourceFilePath = InFilePath.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(InFilePath);

    if (!SourceFilePath.IsEmpty())
    {
        const FString JsonPath = FPaths::ChangeExtension(SourceFilePath, TEXT("json"));
        LoadConfigJson(JsonPath);
    }

    const bool bLoadedVisual = !SourceFilePath.IsEmpty() && LoadWeaponMesh();
    if (!bLoadedVisual && !CreateDefaultBoxMesh())
    {
        return false;
    }

    AttachToTarget(AttachTarget);
    return true;
}

bool AWeaponActor::EquipDefault(USceneComponent* AttachTarget)
{
    if (!ensureMsgf(IsInGameThread(), TEXT("AWeaponActor::EquipDefault must run on the game thread")))
    {
        return false;
    }

    Config = FWeaponConfig();
    SourceFilePath.Reset();
    if (!CreateDefaultBoxMesh())
    {
        return false;
    }

    AttachToTarget(AttachTarget);
    return true;
}

bool AWeaponActor::LoadConfigJson(const FString& JsonPath)
{
    const int64 JsonFileSize = IFileManager::Get().FileSize(*JsonPath);
    if (JsonFileSize > MaxWeaponConfigBytes)
    {
        UE_LOG(LogTemp, Warning, TEXT("WeaponActor: config JSON exceeds the safety limit: %s"), *JsonPath);
        return false;
    }

    FString JsonString;
    if (!FFileHelper::LoadFileToString(JsonString, *JsonPath))
    {
        SaveDefaultConfigJson(JsonPath);
        return false;
    }

    TSharedPtr<FJsonObject> RootObject;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
    if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
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

bool AWeaponActor::SaveDefaultConfigJson(const FString& JsonPath) const
{
    if (JsonPath.IsEmpty())
    {
        return false;
    }

    IFileManager::Get().MakeDirectory(*FPaths::GetPath(JsonPath), true);

    TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
    RootObject->SetStringField(JSON_VERSION_FIELD, JSON_SCHEMA_VERSION);
    RootObject->SetStringField(TEXT("AttachSocketName"), Config.AttachSocketName.ToString());
    RootObject->SetObjectField(TEXT("HoldTransform"), MakeTransformJson(Config.HoldTransform));
    RootObject->SetObjectField(TEXT("RightHandIK"), MakeTransformJson(Config.RightHandIK));
    RootObject->SetObjectField(TEXT("LeftHandIK"), MakeTransformJson(Config.LeftHandIK));
    RootObject->SetObjectField(TEXT("MuzzleOffset"), MakeVectorJson(Config.MuzzleOffset));
    RootObject->SetNumberField(TEXT("Range"), Config.Range);
    RootObject->SetNumberField(TEXT("Damage"), Config.Damage);
    RootObject->SetNumberField(TEXT("ImpactImpulse"), Config.ImpactImpulse);
    RootObject->SetNumberField(TEXT("FireInterval"), Config.FireInterval);
    RootObject->SetNumberField(TEXT("TraceRadius"), Config.TraceRadius);
    RootObject->SetBoolField(TEXT("bProjectile"), Config.bProjectile);
    RootObject->SetNumberField(TEXT("ProjectileSpeed"), Config.ProjectileSpeed);
    RootObject->SetNumberField(TEXT("ProjectileLifeSeconds"), Config.ProjectileLifeSeconds);

    FString OutputString;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
    return FJsonSerializer::Serialize(RootObject, Writer)
        && FFileHelper::SaveStringToFile(OutputString, *JsonPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void AWeaponActor::ReleaseRuntimeResources()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("AWeaponActor runtime release must run on the game thread")))
    {
        return;
    }

    ClearLoadedComponents();
    if (IsValid(GltfAsset))
    {
        FglTFRuntimeSafety::RequestAssetRelease(GltfAsset);
        GltfAsset = nullptr;
    }
}

void AWeaponActor::ClearLoadedComponents()
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
            Mesh->ClearFlags(RF_Public | RF_Standalone);
        }
    }

    MeshComponents.Empty();
    MeshCache.Empty();
}

UStaticMesh* AWeaponActor::LoadMeshByIndex(int32 MeshIndex)
{
    if (!IsValid(GltfAsset) || MeshIndex < 0 || MeshIndex >= GltfAsset->GetNumMeshes())
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
    const TMap<EglTFRuntimeMaterialType, UMaterialInterface*> MaterialOverrides =
        glTFMaterialOverrideUtils::BuildOverrideMap(MaterialAssets);
    if (MaterialOverrides.Num() > 0)
    {
        MeshConfig.MaterialsConfig.UberMaterialsOverrideMap = MaterialOverrides;
        MeshConfig.MaterialsConfig.UnlitOverrideMap = MaterialOverrides;
    }
    glTFMaterialOverrideUtils::ApplyNamedOverrides(MaterialAssets, MeshConfig.MaterialsConfig);
    MeshConfig.bAllowCPUAccess = false;
    MeshConfig.bBuildLumenCards = true;
    MeshConfig.bBuildSimpleCollision = false;
    MeshConfig.bBuildComplexCollision = false;

    UStaticMesh* Mesh = GltfAsset->LoadStaticMesh(MeshIndex, MeshConfig);
    if (IsValid(Mesh))
    {
        MeshCache.Add(MeshIndex, Mesh);
    }
    return Mesh;
}

bool AWeaponActor::LoadWeaponMesh()
{
    ClearLoadedComponents();

    SourceFilePath = GlbValidation::NormalizePath(SourceFilePath);
    FString ValidationReason;
    if (!GlbValidation::ValidateRuntimeMeshFile(SourceFilePath, ValidationReason))
    {
        UE_LOG(LogTemp, Warning, TEXT("WeaponActor: invalid GLB skipped. Path=%s Reason=%s"), *SourceFilePath, *ValidationReason);
        return false;
    }

    FglTFRuntimeConfig LoaderConfig;
    LoaderConfig.bAllowExternalFiles = true;
    GltfAsset = UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(SourceFilePath, false, LoaderConfig);
    if (!IsValid(GltfAsset))
    {
        return false;
    }

    int32 ComponentIndex = 0;
    const TArray<FglTFRuntimeNode> Nodes = GltfAsset->GetNodes();
    if (Nodes.Num() > MaxRuntimeWeaponNodeCount)
    {
        UE_LOG(LogTemp, Warning, TEXT("WeaponActor: GLB node count exceeds the runtime safety limit. Path=%s Nodes=%d"),
            *SourceFilePath, Nodes.Num());
        ReleaseRuntimeResources();
        return false;
    }
    const int32 MeshCount = GltfAsset->GetNumMeshes();
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
