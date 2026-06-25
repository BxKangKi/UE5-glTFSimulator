// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Runtime/RuntimePrefabActor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeFunctionLibrary.h"
#include "glTFRuntimeParser.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Materials/MaterialInterface.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"


namespace
{
    TMap<EglTFRuntimeMaterialType, UMaterialInterface*> BuildRuntimeLitGltfMaterialOverrides()
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


namespace
{
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

ARuntimePrefabActor::ARuntimePrefabActor()
{
    PrimaryActorTick.bCanEverTick = false;
    Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SetRootComponent(Root);
}

void ARuntimePrefabActor::Destroyed()
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

void ARuntimePrefabActor::ClearLoadedComponents()
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
    bLoaded = false;
}

bool ARuntimePrefabActor::LoadConfigJson(const FString& JsonPath)
{
    Config = FRuntimePrefabActorConfig();

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

void ARuntimePrefabActor::ApplyConfigToMeshComponent(UStaticMeshComponent* MeshComponent) const
{
    if (!IsValid(MeshComponent))
    {
        return;
    }

    MeshComponent->SetCollisionEnabled(Config.bEnableCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
    MeshComponent->SetCollisionProfileName(Config.CollisionProfileName.IsEmpty() ? TEXT("BlockAll") : FName(*Config.CollisionProfileName));
    MeshComponent->SetSimulatePhysics(Config.bSimulatePhysics);
}

UStaticMesh* ARuntimePrefabActor::LoadMeshByIndex(int32 MeshIndex)
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
        const TMap<EglTFRuntimeMaterialType, UMaterialInterface*> LitOverrides = BuildRuntimeLitGltfMaterialOverrides();
        if (LitOverrides.Num() > 0)
        {
            MeshConfig.MaterialsConfig.UnlitOverrideMap = LitOverrides;
        }
    }
    MeshConfig.bAllowCPUAccess = true;
    MeshConfig.bBuildSimpleCollision = true;
    MeshConfig.bBuildComplexCollision = true;
    MeshConfig.CollisionComplexity = ECollisionTraceFlag::CTF_UseComplexAsSimple;

    UStaticMesh* Mesh = RuntimeAsset->LoadStaticMesh(MeshIndex, MeshConfig);
    if (IsValid(Mesh))
    {
        MeshCache.Add(MeshIndex, Mesh);
    }
    return Mesh;
}

bool ARuntimePrefabActor::LoadPrefab(const FString& InFilePath, const FString& InRuntimeName)
{
    ClearLoadedComponents();

    SourceFilePath = FPaths::ConvertRelativePathToFull(InFilePath);
    BaseName = FPaths::GetBaseFilename(SourceFilePath);
    RuntimeName = InRuntimeName.IsEmpty() ? BaseName : InRuntimeName;
    LoadConfigJson(FPaths::ChangeExtension(SourceFilePath, TEXT("json")));

    FglTFRuntimeConfig LoaderConfig;
    LoaderConfig.bAllowExternalFiles = true;
    RuntimeAsset = UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(SourceFilePath, false, LoaderConfig);
    if (!IsValid(RuntimeAsset))
    {
        UE_LOG(LogTemp, Warning, TEXT("RuntimePrefabActor: failed to load %s"), *SourceFilePath);
        return false;
    }

    const TArray<FglTFRuntimeNode> Nodes = RuntimeAsset->GetNodes();
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

        UStaticMeshComponent* MeshComponent = NewObject<UStaticMeshComponent>(this, *FString::Printf(TEXT("RuntimePrefabMesh_%d"), ComponentIndex++));
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
    return bLoaded;
}

FRuntimePlacedObjectRecord ARuntimePrefabActor::ToPlacementRecord() const
{
    FRuntimePlacedObjectRecord Record;
    Record.RuntimeName = RuntimeName;
    Record.BaseName = BaseName;
    Record.SourceFile = SourceFilePath;
    Record.Kind = ERuntimePlacedObjectKind::Prefab;
    Record.Transform = GetActorTransform();
    return Record;
}
