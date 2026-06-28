// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Runtime/RuntimeWeaponActor.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/DamageEvents.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Materials/MaterialInterface.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeFunctionLibrary.h"
#include "glTFRuntimeParser.h"


namespace
{
    TMap<EglTFRuntimeMaterialType, UMaterialInterface*> BuildRuntimeWeaponLitGltfMaterialOverrides()
    {
        TMap<EglTFRuntimeMaterialType, UMaterialInterface*> Overrides;

        if (UMaterialInterface* Opaque = LoadObject<UMaterialInterface>(nullptr, TEXT("/glTFRuntime/M_glTFRuntimeBase")))
        {
            Overrides.Add(EglTFRuntimeMaterialType::Opaque, Opaque);
            Overrides.Add(EglTFRuntimeMaterialType::Masked, Opaque);
        }
        if (UMaterialInterface* Translucent = LoadObject<UMaterialInterface>(nullptr, TEXT("/glTFRuntime/M_glTFRuntimeTranslucent_Inst")))
        {
            Overrides.Add(EglTFRuntimeMaterialType::Translucent, Translucent);
            Overrides.Add(EglTFRuntimeMaterialType::TwoSidedTranslucent, Translucent);
        }
        if (UMaterialInterface* TwoSided = LoadObject<UMaterialInterface>(nullptr, TEXT("/glTFRuntime/M_glTFRuntimeTwoSided_Inst")))
        {
            Overrides.Add(EglTFRuntimeMaterialType::TwoSided, TwoSided);
            Overrides.Add(EglTFRuntimeMaterialType::TwoSidedMasked, TwoSided);
        }
        if (UMaterialInterface* Masked = LoadObject<UMaterialInterface>(nullptr, TEXT("/glTFRuntime/M_glTFRuntimeMasked_Inst")))
        {
            Overrides.Add(EglTFRuntimeMaterialType::Masked, Masked);
        }
        if (UMaterialInterface* TwoSidedMasked = LoadObject<UMaterialInterface>(nullptr, TEXT("/glTFRuntime/M_glTFRuntimeTwoSidedMasked_Inst")))
        {
            Overrides.Add(EglTFRuntimeMaterialType::TwoSidedMasked, TwoSidedMasked);
        }
        if (UMaterialInterface* TwoSidedTranslucent = LoadObject<UMaterialInterface>(nullptr, TEXT("/glTFRuntime/M_glTFRuntimeTwoSidedTranslucent_Inst")))
        {
            Overrides.Add(EglTFRuntimeMaterialType::TwoSidedTranslucent, TwoSidedTranslucent);
        }

        return Overrides;
    }
}

ARuntimeWeaponActor::ARuntimeWeaponActor()
{
    PrimaryActorTick.bCanEverTick = false;
    Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SetRootComponent(Root);
}

void ARuntimeWeaponActor::Destroyed()
{
    ClearLoadedComponents();
    if (IsValid(RuntimeAsset))
    {
        RuntimeAsset->ClearCache();
        RuntimeAsset->MarkAsGarbage();
        RuntimeAsset = nullptr;
    }
    Super::Destroyed();
}

void ARuntimeWeaponActor::ClearLoadedComponents()
{
    for (UStaticMeshComponent* Component : MeshComponents)
    {
        if (IsValid(Component))
        {
            Component->UnregisterComponent();
            Component->DestroyComponent();
        }
    }
    MeshComponents.Empty();
    MeshCache.Empty();
}

bool ARuntimeWeaponActor::EquipFromFile(const FString& InFilePath, USceneComponent* AttachTarget)
{
    SourceFilePath = FPaths::ConvertRelativePathToFull(InFilePath);
    const FString JsonPath = FPaths::ChangeExtension(SourceFilePath, TEXT("json"));
    LoadConfigJson(JsonPath);

    if (!LoadWeaponMesh())
    {
        return false;
    }

    if (IsValid(AttachTarget))
    {
        AttachToComponent(AttachTarget, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
        SetActorRelativeTransform(Config.HoldTransform);
    }
    return true;
}

bool ARuntimeWeaponActor::LoadConfigJson(const FString& JsonPath)
{
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

    const TSharedPtr<FJsonObject>* HoldObject = nullptr;
    if (RootObject->TryGetObjectField(TEXT("Hold"), HoldObject) && HoldObject && HoldObject->IsValid())
    {
        double X = Config.HoldTransform.GetLocation().X;
        double Y = Config.HoldTransform.GetLocation().Y;
        double Z = Config.HoldTransform.GetLocation().Z;
        double Pitch = Config.HoldTransform.Rotator().Pitch;
        double Yaw = Config.HoldTransform.Rotator().Yaw;
        double Roll = Config.HoldTransform.Rotator().Roll;
        double Scale = Config.HoldTransform.GetScale3D().X;
        (*HoldObject)->TryGetNumberField(TEXT("X"), X);
        (*HoldObject)->TryGetNumberField(TEXT("Y"), Y);
        (*HoldObject)->TryGetNumberField(TEXT("Z"), Z);
        (*HoldObject)->TryGetNumberField(TEXT("Pitch"), Pitch);
        (*HoldObject)->TryGetNumberField(TEXT("Yaw"), Yaw);
        (*HoldObject)->TryGetNumberField(TEXT("Roll"), Roll);
        (*HoldObject)->TryGetNumberField(TEXT("Scale"), Scale);
        Config.HoldTransform = FTransform(FRotator(Pitch, Yaw, Roll), FVector(X, Y, Z), FVector(Scale));
    }

    const TSharedPtr<FJsonObject>* MuzzleObject = nullptr;
    if (RootObject->TryGetObjectField(TEXT("Muzzle"), MuzzleObject) && MuzzleObject && MuzzleObject->IsValid())
    {
        double X = Config.MuzzleOffset.X;
        double Y = Config.MuzzleOffset.Y;
        double Z = Config.MuzzleOffset.Z;
        (*MuzzleObject)->TryGetNumberField(TEXT("X"), X);
        (*MuzzleObject)->TryGetNumberField(TEXT("Y"), Y);
        (*MuzzleObject)->TryGetNumberField(TEXT("Z"), Z);
        Config.MuzzleOffset = FVector(X, Y, Z);
    }

    RootObject->TryGetNumberField(TEXT("Range"), Config.Range);
    RootObject->TryGetNumberField(TEXT("Damage"), Config.Damage);
    RootObject->TryGetNumberField(TEXT("FireInterval"), Config.FireInterval);
    return true;
}

UStaticMesh* ARuntimeWeaponActor::LoadMeshByIndex(int32 MeshIndex)
{
    if (!IsValid(RuntimeAsset) || MeshIndex == INDEX_NONE)
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
    {
        const TMap<EglTFRuntimeMaterialType, UMaterialInterface*> LitOverrides = BuildRuntimeWeaponLitGltfMaterialOverrides();
        if (LitOverrides.Num() > 0)
        {
            MeshConfig.MaterialsConfig.UnlitOverrideMap = LitOverrides;
        }
    }
    MeshConfig.bAllowCPUAccess = false;
    MeshConfig.bBuildSimpleCollision = false;
    MeshConfig.bBuildComplexCollision = false;
    UStaticMesh* Mesh = RuntimeAsset->LoadStaticMesh(MeshIndex, MeshConfig);
    if (IsValid(Mesh))
    {
        MeshCache.Add(MeshIndex, Mesh);
    }
    return Mesh;
}

bool ARuntimeWeaponActor::LoadWeaponMesh()
{
    ClearLoadedComponents();

    FglTFRuntimeConfig LoaderConfig;
    LoaderConfig.bAllowExternalFiles = true;
    RuntimeAsset = UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(SourceFilePath, false, LoaderConfig);
    if (!IsValid(RuntimeAsset))
    {
        return false;
    }

    int32 ComponentIndex = 0;
    const TArray<FglTFRuntimeNode> Nodes = RuntimeAsset->GetNodes();
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
        UStaticMeshComponent* MeshComponent = NewObject<UStaticMeshComponent>(this, *FString::Printf(TEXT("WeaponMesh_%d"), ComponentIndex++));
        AddInstanceComponent(MeshComponent);
        MeshComponent->SetMobility(EComponentMobility::Movable);
        MeshComponent->SetupAttachment(Root);
        MeshComponent->SetStaticMesh(Mesh);
        MeshComponent->SetRelativeTransform(Node.Transform);
        MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        MeshComponent->RegisterComponent();
        MeshComponents.Add(MeshComponent);
    }
    return MeshComponents.Num() > 0;
}

FVector ARuntimeWeaponActor::GetMuzzleWorldLocation() const
{
    return GetActorTransform().TransformPosition(Config.MuzzleOffset);
}

void ARuntimeWeaponActor::Fire(AController* InstigatorController)
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

    FVector ViewLocation;
    FRotator ViewRotation;
    InstigatorController->GetPlayerViewPoint(ViewLocation, ViewRotation);
    const FVector End = ViewLocation + ViewRotation.Vector() * Config.Range;

    FCollisionQueryParams Params(SCENE_QUERY_STAT(RuntimeWeaponFire), true, this);
    if (APawn* Pawn = InstigatorController->GetPawn())
    {
        Params.AddIgnoredActor(Pawn);
    }

    FHitResult Hit;
    World->LineTraceSingleByChannel(Hit, ViewLocation, End, ECC_Visibility, Params);
    const FVector TraceEnd = Hit.bBlockingHit ? Hit.ImpactPoint : End;
    DrawDebugLine(World, GetMuzzleWorldLocation(), TraceEnd, FColor::Red, false, 1.0f, 0, 2.0f);

    if (Hit.bBlockingHit)
    {
        AActor* HitActor = Hit.GetActor();
        if (IsValid(HitActor))
        {
            UGameplayStatics::ApplyPointDamage(HitActor, Config.Damage, ViewRotation.Vector(), Hit, InstigatorController, this, nullptr);
        }
        if (UPrimitiveComponent* HitComponent = Hit.GetComponent())
        {
            if (HitComponent->IsSimulatingPhysics())
            {
                HitComponent->AddImpulseAtLocation(ViewRotation.Vector() * Config.Damage * 1200.0f, Hit.ImpactPoint);
            }
        }
    }
}
