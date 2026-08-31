// Copyright 2026 OpenAI. Licensed under the MIT License.

#include "RuntimeImpostorComponent.h"

#include "CanvasTypes.h"
#include "Components/PrimitiveComponent.h"
#include "ProceduralMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "Math/BoxSphereBounds.h"
#include "RenderUtils.h"
#include "SceneView.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/Package.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFmod.h"
#include "Materials/MaterialExpressionFloor.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#endif

#include "glTFRuntimeAsset.h"

namespace RuntimeImpostorLocal
{
    static constexpr float MinExtent = 1.0f;
    static constexpr float MinCameraDistance = 100.0f;

#if WITH_EDITOR
    template <typename T>
    static T* AddMaterialExpression(
        UMaterial* Material,
        UMaterialEditorOnlyData* EditorOnlyData,
        int32 X,
        int32 Y)
    {
        T* Expression = NewObject<T>(Material);
        if (!Expression)
        {
            return nullptr;
        }

        Expression->MaterialExpressionEditorX = X;
        Expression->MaterialExpressionEditorY = Y;
        EditorOnlyData->ExpressionCollection.Expressions.Add(Expression);
        return Expression;
    }
#endif

    static FRotator MakeFacingRotation(const FVector& ToCamera)
    {
        FVector Normal = ToCamera.GetSafeNormal();
        if (Normal.IsNearlyZero())
        {
            Normal = FVector::ForwardVector;
        }

        FVector Up = FVector::UpVector;
        if (FMath::Abs(FVector::DotProduct(Normal, Up)) > 0.985f)
        {
            Up = FVector::ForwardVector;
        }
        return FRotationMatrix::MakeFromZX(Normal, Up).Rotator();
    }
}

URuntimeImpostorComponent::URuntimeImpostorComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
    bTickInEditor = false;
}

void URuntimeImpostorComponent::BeginPlay()
{
    Super::BeginPlay();

    // A project may have saved the generated material on the component; otherwise resolve it lazily.
    ResolveMaterial();
}

void URuntimeImpostorComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    RestoreSourceState();
    ClearBakedImpostor();
    Super::EndPlay(EndPlayReason);
}

bool URuntimeImpostorComponent::IsUsableSource(AActor* Actor) const
{
    return IsValid(Actor);
}

bool URuntimeImpostorComponent::ResolveDefaultMaterialObject()
{
    if (IsValid(ImpostorMaterial))
    {
        return true;
    }

    UObject* Object = StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, *GetDefaultMaterialObjectPath());
    ImpostorMaterial = Cast<UMaterialInterface>(Object);
    return IsValid(ImpostorMaterial);
}

bool URuntimeImpostorComponent::ResolveMaterial()
{
    if (IsValid(ImpostorMaterial))
    {
        return true;
    }
    return ResolveDefaultMaterialObject();
}

FString URuntimeImpostorComponent::GetDefaultMaterialObjectPath()
{
    return TEXT("/Game/RuntimeImpostorsGenerated/M_RuntimeImpostor.M_RuntimeImpostor");
}

bool URuntimeImpostorComponent::InitializeRenderObjects()
{
    if (!GetWorld())
    {
        return false;
    }

    if (!ResolveMaterial())
    {
        UE_LOG(LogTemp, Error, TEXT("RuntimeImpostors: no impostor material. Run CreateDefaultImpostorMaterial in the editor or assign ImpostorMaterial manually."));
        return false;
    }

    if (!BillboardMesh)
    {
        BillboardMesh = NewObject<UProceduralMeshComponent>(GetOwner(), TEXT("RuntimeImpostorBillboard"));
        BillboardMesh->CreationMethod = EComponentCreationMethod::Instance;
        BillboardMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        BillboardMesh->SetGenerateOverlapEvents(false);
        BillboardMesh->SetCanEverAffectNavigation(false);
        BillboardMesh->bUseAsyncCooking = false;
        BillboardMesh->RegisterComponent();
        BillboardMesh->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
    }

    if (!CaptureRoot)
    {
        CaptureRoot = NewObject<USceneComponent>(GetOwner(), TEXT("RuntimeImpostorCaptureRoot"));
        CaptureRoot->CreationMethod = EComponentCreationMethod::Instance;
        CaptureRoot->RegisterComponent();
        CaptureRoot->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
    }

    if (!CaptureComponent)
    {
        CaptureComponent = NewObject<USceneCaptureComponent2D>(GetOwner(), TEXT("RuntimeImpostorCapture"));
        CaptureComponent->CreationMethod = EComponentCreationMethod::Instance;
        CaptureComponent->RegisterComponent();
        CaptureComponent->AttachToComponent(CaptureRoot, FAttachmentTransformRules::KeepRelativeTransform);
    }

    const int32 TileResolution = FMath::Clamp(BakeSettings.TileResolution, 32, 1024);
    const int32 ViewsX = FMath::Clamp(BakeSettings.HorizontalViews, 4, 32);
    const int32 ViewsY = FMath::Clamp(BakeSettings.VerticalViews, 1, 5);

    AtlasRenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
    AtlasRenderTarget->RenderTargetFormat = RTF_RGBA8;
    AtlasRenderTarget->ClearColor = FLinearColor(0, 0, 0, 0);
    AtlasRenderTarget->bAutoGenerateMips = false;
    AtlasRenderTarget->InitAutoFormat(TileResolution * ViewsX, TileResolution * ViewsY);
    AtlasRenderTarget->UpdateResourceImmediate(true);

    ViewRenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
    ViewRenderTarget->RenderTargetFormat = RTF_RGBA8;
    ViewRenderTarget->ClearColor = FLinearColor(0, 0, 0, 0);
    ViewRenderTarget->bAutoGenerateMips = false;
    ViewRenderTarget->InitAutoFormat(TileResolution, TileResolution);
    ViewRenderTarget->UpdateResourceImmediate(true);

    CaptureComponent->TextureTarget = ViewRenderTarget;
    ConfigureCaptureFlags();

    DynamicMaterial = UMaterialInstanceDynamic::Create(ImpostorMaterial, GetOwner());
    if (!IsValid(DynamicMaterial))
    {
        return false;
    }

    DynamicMaterial->SetTextureParameterValue(AtlasParameterName, AtlasRenderTarget);
    DynamicMaterial->SetScalarParameterValue(TilesXParameterName, static_cast<float>(ViewsX));
    DynamicMaterial->SetScalarParameterValue(TilesYParameterName, static_cast<float>(ViewsY));
    DynamicMaterial->SetScalarParameterValue(ViewIndexParameterName, 0.0f);
    DynamicMaterial->SetScalarParameterValue(OpacityParameterName, 1.0f);
    return true;
}

void URuntimeImpostorComponent::ConfigureCaptureFlags()
{
    if (!CaptureComponent)
    {
        return;
    }

    CaptureComponent->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
    CaptureComponent->bCaptureEveryFrame = false;
    CaptureComponent->bCaptureOnMovement = false;
    CaptureComponent->bAlwaysPersistRenderingState = true;
    CaptureComponent->FOVAngle = 45.0f;
    CaptureComponent->ShowFlags.SetAtmosphere(false);
    CaptureComponent->ShowFlags.SetFog(false);
    CaptureComponent->ShowFlags.SetVolumetricFog(false);
    CaptureComponent->ShowFlags.SetMotionBlur(false);
    CaptureComponent->ShowFlags.SetLensFlares(false);
    CaptureComponent->ShowFlags.SetBloom(false);
    CaptureComponent->ShowFlags.SetAmbientOcclusion(true);
}

void URuntimeImpostorComponent::BuildCaptureShowOnlyList()
{
    if (!CaptureComponent)
    {
        return;
    }

    CaptureComponent->ClearShowOnlyComponents();
    for (const TWeakObjectPtr<UPrimitiveComponent>& WeakPrimitive : SourcePrimitives)
    {
        UPrimitiveComponent* Primitive = WeakPrimitive.Get();
        if (IsValid(Primitive))
        {
            CaptureComponent->ShowOnlyComponent(Primitive);
        }
    }

    if (SourceActor.IsValid())
    {
        // Include primitives attached below child actors for the common glTF/streaming hierarchy case.
        CaptureComponent->ShowOnlyActorComponents(SourceActor.Get(), true);
    }
}

FVector URuntimeImpostorComponent::GetCaptureCenterWorld() const
{
    return GetComponentTransform().TransformPosition(LocalBoundsCenter);
}

float URuntimeImpostorComponent::GetCaptureRadius() const
{
    const float Radius = BoundsExtent.Size();
    return FMath::Max(RuntimeImpostorLocal::MinExtent, Radius);
}

bool URuntimeImpostorComponent::CaptureSourceBounds()
{
    AActor* Actor = SourceActor.Get();
    if (!IsValid(Actor))
    {
        return false;
    }

    const FBox WorldBox = Actor->GetComponentsBoundingBox(true);
    if (!WorldBox.IsValid)
    {
        return false;
    }

    const FVector CenterWorld = WorldBox.GetCenter();
    const FVector ExtentWorld = WorldBox.GetExtent().ComponentMax(FVector(RuntimeImpostorLocal::MinExtent));

    const FTransform OwnerTransform = GetComponentTransform();
    LocalBoundsCenter = OwnerTransform.InverseTransformPosition(CenterWorld);
    BoundsExtent = OwnerTransform.InverseTransformVectorNoScale(ExtentWorld).GetAbs().ComponentMax(FVector(RuntimeImpostorLocal::MinExtent));

    const float MaxExtent = FMath::Max3(BoundsExtent.X, BoundsExtent.Y, BoundsExtent.Z);
    const float Padding = FMath::Max(1.0f, BakeSettings.CapturePadding);
    BillboardHeight = 2.0f * MaxExtent * Padding * FMath::Max(0.01f, LODSettings.BillboardHeightScale);
    BillboardWidth = BillboardHeight;
    return true;
}

bool URuntimeImpostorComponent::CaptureOneView(int32 ViewIndex)
{
    if (!CaptureComponent || !ViewRenderTarget || !SourceActor.IsValid())
    {
        return false;
    }

    const int32 ViewsX = FMath::Clamp(BakeSettings.HorizontalViews, 4, 32);
    const int32 ViewsY = FMath::Clamp(BakeSettings.VerticalViews, 1, 5);
    if (ViewIndex < 0 || ViewIndex >= ViewsX * ViewsY)
    {
        return false;
    }

    const int32 XIndex = ViewIndex % ViewsX;
    const int32 YIndex = ViewIndex / ViewsX;
    const float Yaw = 360.0f * (static_cast<float>(XIndex) / static_cast<float>(ViewsX));
    float Pitch = 0.0f;
    if (ViewsY > 1)
    {
        const float T = static_cast<float>(YIndex) / static_cast<float>(ViewsY - 1);
        Pitch = FMath::Lerp(-BakeSettings.PitchDegrees, BakeSettings.PitchDegrees, T);
    }

    const float Radius = GetCaptureRadius() * 2.2f;
    const FRotator LocalRotation(Pitch, Yaw, 0.0f);
    const FVector LocalDirection = LocalRotation.RotateVector(FVector::ForwardVector);
    const FVector CenterWorld = GetCaptureCenterWorld();
    const FVector CameraWorld = CenterWorld + GetComponentTransform().TransformVectorNoScale(LocalDirection).GetSafeNormal() * Radius;

    CaptureComponent->SetWorldLocation(CameraWorld);
    CaptureComponent->SetWorldRotation(RuntimeImpostorLocal::MakeFacingRotation(CenterWorld - CameraWorld));

    CaptureComponent->OrthoWidth = 2.0f * FMath::Max3(BoundsExtent.X, BoundsExtent.Y, BoundsExtent.Z) * FMath::Max(1.0f, BakeSettings.CapturePadding);
    UKismetRenderingLibrary::ClearRenderTarget2D(GetWorld(), ViewRenderTarget, FLinearColor(0, 0, 0, 0));
    CaptureComponent->CaptureScene();
    return true;
}

bool URuntimeImpostorComponent::ComposeViewIntoAtlas(int32 ViewIndex)
{
    if (!AtlasRenderTarget || !ViewRenderTarget || !GetWorld())
    {
        return false;
    }

    const int32 TileResolution = FMath::Clamp(BakeSettings.TileResolution, 32, 1024);
    const int32 ViewsX = FMath::Clamp(BakeSettings.HorizontalViews, 4, 32);
    const int32 TileX = ViewIndex % ViewsX;
    const int32 TileY = ViewIndex / ViewsX;

    UCanvas* Canvas = nullptr;
    FVector2D CanvasSize;
    FDrawToRenderTargetContext Context;
    UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(GetWorld(), AtlasRenderTarget, Canvas, CanvasSize, Context);
    if (!Canvas)
    {
        return false;
    }

    Canvas->K2_DrawTexture(
        ViewRenderTarget,
        FVector2D(static_cast<float>(TileX * TileResolution), static_cast<float>(TileY * TileResolution)),
        FVector2D(static_cast<float>(TileResolution), static_cast<float>(TileResolution)),
        FVector2D::ZeroVector,
        FVector2D::UnitVector,
        FLinearColor::White,
        BLEND_Translucent,
        0.0f,
        FVector2D(0.5f, 0.5f));

    UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(GetWorld(), Context);
    return true;
}

bool URuntimeImpostorComponent::CaptureAtlas()
{
    if (!InitializeRenderObjects() || !CaptureSourceBounds())
    {
        return false;
    }

    BuildCaptureShowOnlyList();
    UKismetRenderingLibrary::ClearRenderTarget2D(GetWorld(), AtlasRenderTarget, FLinearColor(0, 0, 0, 0));

    const int32 TotalViews = FMath::Clamp(BakeSettings.HorizontalViews, 4, 32) * FMath::Clamp(BakeSettings.VerticalViews, 1, 5);
    SetSourceVisibility(true);

    for (int32 ViewIndex = 0; ViewIndex < TotalViews; ++ViewIndex)
    {
        if (!CaptureOneView(ViewIndex) || !ComposeViewIntoAtlas(ViewIndex))
        {
            return false;
        }
        BakeProgress = static_cast<float>(ViewIndex + 1) / static_cast<float>(TotalViews);
    }

    return CreateBillboardMesh();
}

bool URuntimeImpostorComponent::CreateBillboardMesh()
{
    if (!BillboardMesh || !DynamicMaterial)
    {
        return false;
    }

    const float HalfWidth = BillboardWidth * 0.5f;
    const float HalfHeight = BillboardHeight * 0.5f;

    TArray<FVector> Vertices;
    Vertices.Reserve(4);
    Vertices.Add(FVector(-HalfWidth, -HalfHeight, 0.0f));
    Vertices.Add(FVector(HalfWidth, -HalfHeight, 0.0f));
    Vertices.Add(FVector(HalfWidth, HalfHeight, 0.0f));
    Vertices.Add(FVector(-HalfWidth, HalfHeight, 0.0f));

    TArray<int32> Indices = { 0, 1, 2, 0, 2, 3 };
    TArray<FVector> Normals;
    Normals.Init(FVector::UpVector, 4);

    TArray<FVector2D> UV0;
    UV0.Reserve(4);
    UV0.Add(FVector2D(0, 1));
    UV0.Add(FVector2D(1, 1));
    UV0.Add(FVector2D(1, 0));
    UV0.Add(FVector2D(0, 0));

    TArray<FProcMeshTangent> Tangents;
    Tangents.Init(FProcMeshTangent(1, 0, 0), 4);
    TArray<FLinearColor> VertexColors;
    VertexColors.Init(FLinearColor::White, 4);

    BillboardMesh->ClearAllMeshSections();
    BillboardMesh->CreateMeshSection_LinearColor(0, Vertices, Indices, Normals, UV0, VertexColors, Tangents, true, false);
    BillboardMesh->SetMaterial(0, DynamicMaterial);
    BillboardMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    BillboardMesh->SetVisibility(true);
    return true;
}

void URuntimeImpostorComponent::SetSourceVisibility(bool bVisible)
{
    for (const TWeakObjectPtr<UPrimitiveComponent>& WeakPrimitive : SourcePrimitives)
    {
        if (UPrimitiveComponent* Primitive = WeakPrimitive.Get())
        {
            Primitive->SetVisibility(bVisible, true);
        }
    }
}

void URuntimeImpostorComponent::RestoreSourceState()
{
    const int32 Count = FMath::Min(SourcePrimitives.Num(), SourceOriginalVisibility.Num());
    for (int32 Index = 0; Index < Count; ++Index)
    {
        if (UPrimitiveComponent* Primitive = SourcePrimitives[Index].Get())
        {
            Primitive->SetVisibility(SourceOriginalVisibility[Index], true);
            if (Index < SourceOriginalCollision.Num())
            {
                Primitive->SetCollisionEnabled(SourceOriginalCollision[Index]);
            }
        }
    }

    SourcePrimitives.Reset();
    SourceOriginalVisibility.Reset();
    SourceOriginalCollision.Reset();
}

void URuntimeImpostorComponent::FinishBake(bool bSuccess, const FString& Reason)
{
    State = bSuccess ? ERuntimeImpostorState::Ready : ERuntimeImpostorState::Failed;
    BakeProgress = bSuccess ? 1.0f : BakeProgress;

    if (!bSuccess)
    {
        if (BillboardMesh)
        {
            BillboardMesh->SetVisibility(false);
        }
        RestoreSourceState();
        UE_LOG(LogTemp, Error, TEXT("RuntimeImpostors: bake failed on %s: %s"), *GetNameSafe(GetOwner()), *Reason);
    }
    else
    {
        CurrentLODAlpha = LODSettings.bUseDistanceSwitching ? 0.0f : 1.0f;
        LastLODAlpha = -1.0f;
        if (DynamicMaterial)
        {
            DynamicMaterial->SetScalarParameterValue(OpacityParameterName, CurrentLODAlpha);
        }
        if (BillboardMesh)
        {
            BillboardMesh->SetVisibility(CurrentLODAlpha > 0.001f);
        }
        // Keep source geometry alive for the near side of the distance cross-fade.
        // UpdateLOD() becomes responsible for hiding/showing the source as distance changes.
        if (BakeSettings.bHideSourceAfterBake && !LODSettings.bUseDistanceSwitching)
        {
            SetSourceVisibility(false);
        }
    }

    OnBakeFinished.Broadcast(bSuccess);
}

bool URuntimeImpostorComponent::BakeFromActor(AActor* InSourceActor)
{
    if (!IsInGameThread())
    {
        UE_LOG(LogTemp, Error, TEXT("RuntimeImpostors: BakeFromActor must be called on the game thread."));
        return false;
    }

    if (!IsUsableSource(InSourceActor))
    {
        FinishBake(false, TEXT("SourceActor is invalid."));
        return false;
    }

    ClearBakedImpostor();
    SourceActor = InSourceActor;

    SourcePrimitives.Reset();
    TArray<UPrimitiveComponent*> PrimitiveComponents;
    InSourceActor->GetComponents<UPrimitiveComponent>(PrimitiveComponents);
    SourcePrimitives.Reserve(PrimitiveComponents.Num());
    for (UPrimitiveComponent* Primitive : PrimitiveComponents)
    {
        SourcePrimitives.Add(Primitive);
    }
    SourceOriginalVisibility.Reserve(SourcePrimitives.Num());
    SourceOriginalCollision.Reserve(SourcePrimitives.Num());
    for (const TWeakObjectPtr<UPrimitiveComponent>& WeakPrimitive : SourcePrimitives)
    {
        if (UPrimitiveComponent* Primitive = WeakPrimitive.Get())
        {
            SourceOriginalVisibility.Add(Primitive->IsVisible());
            SourceOriginalCollision.Add(Primitive->GetCollisionEnabled());
        }
    }

    State = ERuntimeImpostorState::Baking;
    BakeProgress = 0.0f;
    LastSourceTransform = InSourceActor->GetActorTransform();

    const bool bSuccess = CaptureAtlas();
    FinishBake(bSuccess, bSuccess ? FString() : TEXT("Scene capture or billboard construction failed."));
    return bSuccess;
}

bool URuntimeImpostorComponent::BakeFromGlTFRuntimeAsset(UglTFRuntimeAsset* Asset)
{
    if (!IsInGameThread() || !IsValid(Asset) || !GetWorld())
    {
        FinishBake(false, TEXT("Invalid glTFRuntime asset, world, or thread."));
        return false;
    }

    // Build a transient preview actor from glTFRuntime's already parsed node/mesh data.
    // This path is intentionally synchronous: callers that stream files asynchronously should invoke
    // it after their asset is ready. Existing project streaming actors can use BakeFromActor() instead.
    AActor* PreviewActor = GetWorld()->SpawnActor<AActor>();
    if (!IsValid(PreviewActor))
    {
        FinishBake(false, TEXT("Could not spawn glTFRuntime preview actor."));
        return false;
    }
    TArray<FglTFRuntimeNode> Nodes = Asset->GetNodes();
    FglTFRuntimeStaticMeshConfig MeshConfig;
    MeshConfig.Outer = GetTransientPackage();
    MeshConfig.CacheMode = EglTFRuntimeCacheMode::None;
    MeshConfig.MaterialsConfig.CacheMode = EglTFRuntimeCacheMode::None;
    MeshConfig.bAllowCPUAccess = false;
    MeshConfig.bBuildSimpleCollision = false;
    MeshConfig.bBuildComplexCollision = false;
    MeshConfig.bBuildNavCollision = false;
    MeshConfig.bBuildLumenCards = false;

    int32 CreatedMeshes = 0;
    for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
    {
        const FglTFRuntimeNode& Node = Nodes[NodeIndex];
        if (Node.MeshIndex < 0)
        {
            continue;
        }

        UStaticMesh* Mesh = Asset->LoadStaticMesh(Node.MeshIndex, MeshConfig);
        if (!IsValid(Mesh))
        {
            continue;
        }

        UStaticMeshComponent* MeshComponent = NewObject<UStaticMeshComponent>(PreviewActor);
        MeshComponent->CreationMethod = EComponentCreationMethod::Instance;
        MeshComponent->SetStaticMesh(Mesh);
        MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        MeshComponent->SetVisibility(true);
        MeshComponent->RegisterComponent();
        PreviewActor->AddInstanceComponent(MeshComponent);

        FTransform NodeTransform = Node.Transform;
        if (!Asset->BuildTransformFromNodeBackward(NodeIndex, NodeTransform))
        {
            NodeTransform = Node.Transform;
        }
        MeshComponent->SetWorldTransform(NodeTransform);
        ++CreatedMeshes;
    }

    const bool bSuccess = CreatedMeshes > 0 && BakeFromActor(PreviewActor);
    if (bSuccess)
    {
        // The preview actor is disposable: this convenience path produces a standalone impostor.
        SourceActor.Reset();
        CurrentLODAlpha = 1.0f;
        LastLODAlpha = -1.0f;
        if (DynamicMaterial)
        {
            DynamicMaterial->SetScalarParameterValue(OpacityParameterName, 1.0f);
        }
        if (BillboardMesh)
        {
            BillboardMesh->SetVisibility(true);
        }
    }
    PreviewActor->Destroy();
    return bSuccess;
}

void URuntimeImpostorComponent::ClearBakedImpostor()
{
    if (BillboardMesh)
    {
        BillboardMesh->ClearAllMeshSections();
        BillboardMesh->SetVisibility(false);
    }
    if (CaptureComponent)
    {
        CaptureComponent->ClearShowOnlyComponents();
    }
    AtlasRenderTarget = nullptr;
    ViewRenderTarget = nullptr;
    DynamicMaterial = nullptr;
    State = ERuntimeImpostorState::Uninitialized;
    BakeProgress = 0.0f;
    LastViewIndex = INDEX_NONE;
    RestoreSourceState();
    SourceActor.Reset();
}

int32 URuntimeImpostorComponent::CalculateViewIndex(const FVector& CameraLocation) const
{
    const int32 ViewsX = FMath::Clamp(BakeSettings.HorizontalViews, 4, 32);
    const int32 ViewsY = FMath::Clamp(BakeSettings.VerticalViews, 1, 5);
    if (ViewsX <= 0 || ViewsY <= 0)
    {
        return 0;
    }

    const FVector ToCameraWorld = CameraLocation - GetCaptureCenterWorld();
    FVector LocalDirection = GetComponentTransform().InverseTransformVectorNoScale(ToCameraWorld).GetSafeNormal();
    if (LocalDirection.IsNearlyZero())
    {
        return 0;
    }

    float Yaw = FMath::RadiansToDegrees(FMath::Atan2(LocalDirection.Y, LocalDirection.X));
    if (Yaw < 0.0f)
    {
        Yaw += 360.0f;
    }
    const float YawStep = 360.0f / static_cast<float>(ViewsX);
    const int32 X = FMath::Clamp(FMath::FloorToInt((Yaw + 0.5f * YawStep) / YawStep) % ViewsX, 0, ViewsX - 1);

    int32 Y = 0;
    if (ViewsY > 1)
    {
        const float PitchRadians = FMath::Atan2(LocalDirection.Z, FVector2D(LocalDirection.X, LocalDirection.Y).Size());
        const float PitchDegrees = FMath::RadiansToDegrees(PitchRadians);
        const float T = FMath::Clamp((PitchDegrees + BakeSettings.PitchDegrees) / FMath::Max(0.001f, 2.0f * BakeSettings.PitchDegrees), 0.0f, 1.0f);
        Y = FMath::Clamp(FMath::RoundToInt(T * static_cast<float>(ViewsY - 1)), 0, ViewsY - 1);
    }

    return Y * ViewsX + X;
}

void URuntimeImpostorComponent::UpdateBillboardFacing()
{
    if (!BillboardMesh || !GetWorld())
    {
        return;
    }

    APlayerController* Controller = UGameplayStatics::GetPlayerController(GetWorld(), 0);
    if (!Controller)
    {
        return;
    }

    FVector CameraLocation;
    FRotator CameraRotation;
    Controller->GetPlayerViewPoint(CameraLocation, CameraRotation);

    const FVector CenterWorld = GetCaptureCenterWorld();
    const FVector ToCamera = CameraLocation - CenterWorld;
    if (ToCamera.SizeSquared() < FMath::Square(RuntimeImpostorLocal::MinCameraDistance))
    {
        return;
    }

    BillboardMesh->SetWorldLocation(CenterWorld);
    BillboardMesh->SetWorldRotation(RuntimeImpostorLocal::MakeFacingRotation(ToCamera));

    const int32 ViewIndex = CalculateViewIndex(CameraLocation);
    if (DynamicMaterial && ViewIndex != LastViewIndex)
    {
        DynamicMaterial->SetScalarParameterValue(ViewIndexParameterName, static_cast<float>(ViewIndex));
        LastViewIndex = ViewIndex;
    }
}

void URuntimeImpostorComponent::UpdateLOD(float DeltaTime)
{
    if (!DynamicMaterial || !SourceActor.IsValid() || !GetWorld())
    {
        return;
    }

    if (!LODSettings.bUseDistanceSwitching)
    {
        CurrentLODAlpha = 1.0f;
    }
    else
    {
        APlayerController* Controller = UGameplayStatics::GetPlayerController(GetWorld(), 0);
        if (!Controller)
        {
            return;
        }

        FVector CameraLocation;
        FRotator CameraRotation;
        Controller->GetPlayerViewPoint(CameraLocation, CameraRotation);

        const float Distance = FVector::Distance(CameraLocation, GetCaptureCenterWorld());
        const float Start = FMath::Max(0.0f, LODSettings.StartDistance);
        const float End = FMath::Max(Start + 1.0f, LODSettings.EndDistance);
        const float Alpha = FMath::Clamp((Distance - Start) / (End - Start), 0.0f, 1.0f);
        CurrentLODAlpha = FMath::SmoothStep(0.0f, 1.0f, Alpha);
    }

    if (!FMath::IsNearlyEqual(CurrentLODAlpha, LastLODAlpha, 0.005f))
    {
        DynamicMaterial->SetScalarParameterValue(OpacityParameterName, CurrentLODAlpha);
        LastLODAlpha = CurrentLODAlpha;
    }

    // Once the impostor is fully opaque, source rendering can stop. Before that, keep the source
    // alive for a cross fade so the transition does not pop.
    const bool bWantSource = CurrentLODAlpha < 0.005f || !BakeSettings.bHideSourceAfterBake;
    if (BakeSettings.bHideSourceAfterBake)
    {
        SetSourceVisibility(bWantSource);
    }

    if (BakeSettings.bDisableSourceCollisionAfterBake)
    {
        const bool bSourceCollisionEnabled = CurrentLODAlpha < 0.995f;
        const int32 Count = FMath::Min(SourcePrimitives.Num(), SourceOriginalCollision.Num());
        for (int32 Index = 0; Index < Count; ++Index)
        {
            if (UPrimitiveComponent* Primitive = SourcePrimitives[Index].Get())
            {
                const ECollisionEnabled::Type OriginalCollision =
                    static_cast<ECollisionEnabled::Type>(SourceOriginalCollision[Index].GetValue());
                Primitive->SetCollisionEnabled(
                    bSourceCollisionEnabled ? OriginalCollision : ECollisionEnabled::NoCollision);
            }
        }
    }

    if (BillboardMesh)
    {
        BillboardMesh->SetVisibility(CurrentLODAlpha > 0.001f);
    }
}

void URuntimeImpostorComponent::SyncToSourceTransform()
{
    if (!bRebuildWhenSourceChanges || !SourceActor.IsValid())
    {
        return;
    }

    const FTransform CurrentTransform = SourceActor->GetActorTransform();
    const float LocationTolerance = 0.5f;
    const float RotationTolerance = 0.25f;
    const float ScaleTolerance = 0.005f;
    if (CurrentTransform.GetLocation().Equals(LastSourceTransform.GetLocation(), LocationTolerance) &&
        CurrentTransform.GetRotation().AngularDistance(LastSourceTransform.GetRotation()) <= FMath::DegreesToRadians(RotationTolerance) &&
        CurrentTransform.GetScale3D().Equals(LastSourceTransform.GetScale3D(), ScaleTolerance))
    {
        return;
    }

    LastSourceTransform = CurrentTransform;
    // The atlas captures geometry from the current source pose; transform changes do not require a
    // recapture, they only require the billboard center to follow the source.
    MarkRenderStateDirty();
}

void URuntimeImpostorComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (State != ERuntimeImpostorState::Ready)
    {
        return;
    }

    SyncToSourceTransform();
    UpdateLOD(DeltaTime);
    if (bTickBillboard)
    {
        UpdateBillboardFacing();
    }
}

#if WITH_EDITOR
void URuntimeImpostorComponent::CreateDefaultImpostorMaterial()
{
    const FString PackageName = TEXT("/Game/RuntimeImpostorsGenerated/M_RuntimeImpostor");
    const FString AssetName = TEXT("M_RuntimeImpostor");

    if (FindObject<UMaterial>(nullptr, *(PackageName + TEXT(".") + AssetName)))
    {
        ImpostorMaterial = Cast<UMaterial>(StaticLoadObject(UMaterial::StaticClass(), nullptr, *(PackageName + TEXT(".") + AssetName)));
        return;
    }

    UPackage* Package = CreatePackage(*PackageName);
    if (!Package)
    {
        UE_LOG(LogTemp, Error, TEXT("RuntimeImpostors: could not create package %s"), *PackageName);
        return;
    }

    UMaterial* Material = NewObject<UMaterial>(Package, *AssetName, RF_Public | RF_Standalone);
    if (!Material)
    {
        return;
    }

    Material->MaterialDomain = MD_Surface;
    Material->BlendMode = BLEND_Translucent;
    Material->SetShadingModel(MSM_Unlit);
    Material->TwoSided = true;
    Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);

    UMaterialEditorOnlyData* EditorOnlyData = Material->GetEditorOnlyData();
    if (!EditorOnlyData)
    {
        return;
    }

    UMaterialExpressionTextureSampleParameter2D* Atlas = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionTextureSampleParameter2D>(Material, EditorOnlyData, 900, 0);
    Atlas->ParameterName = TEXT("ImpostorAtlas");
    Atlas->SamplerType = SAMPLERTYPE_Color;

    UMaterialExpressionTextureCoordinate* UV = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionTextureCoordinate>(Material, EditorOnlyData, -1200, 0);
    UMaterialExpressionScalarParameter* TilesX = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionScalarParameter>(Material, EditorOnlyData, -1200, 160);
    TilesX->ParameterName = TEXT("ImpostorTilesX");
    TilesX->DefaultValue = 8.0f;
    UMaterialExpressionScalarParameter* TilesY = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionScalarParameter>(Material, EditorOnlyData, -1200, 300);
    TilesY->ParameterName = TEXT("ImpostorTilesY");
    TilesY->DefaultValue = 3.0f;
    UMaterialExpressionScalarParameter* ViewIndex = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionScalarParameter>(Material, EditorOnlyData, -1200, 440);
    ViewIndex->ParameterName = TEXT("ImpostorViewIndex");
    ViewIndex->DefaultValue = 0.0f;
    UMaterialExpressionScalarParameter* Opacity = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionScalarParameter>(Material, EditorOnlyData, 900, 360);
    Opacity->ParameterName = TEXT("ImpostorOpacity");
    Opacity->DefaultValue = 1.0f;

    UMaterialExpressionConstant* One = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionConstant>(Material, EditorOnlyData, -1000, 540);
    One->R = 1.0f;

    UMaterialExpressionDivide* InvTilesX = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionDivide>(Material, EditorOnlyData, -800, 160);
    InvTilesX->A.Expression = One;
    InvTilesX->B.Expression = TilesX;

    UMaterialExpressionDivide* InvTilesY = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionDivide>(Material, EditorOnlyData, -800, 300);
    InvTilesY->A.Expression = One;
    InvTilesY->B.Expression = TilesY;

    UMaterialExpressionAppendVector* InvTiles = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionAppendVector>(Material, EditorOnlyData, -600, 100);
    InvTiles->A.Expression = InvTilesX;
    InvTiles->B.Expression = InvTilesY;

    UMaterialExpressionMultiply* LocalUV = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionMultiply>(Material, EditorOnlyData, -350, 0);
    LocalUV->A.Expression = UV;
    LocalUV->B.Expression = InvTiles;

    UMaterialExpressionFloor* Frame = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionFloor>(Material, EditorOnlyData, -800, 440);
    Frame->Input.Expression = ViewIndex;

    UMaterialExpressionFmod* Column = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionFmod>(Material, EditorOnlyData, -600, 440);
    Column->A.Expression = Frame;
    Column->B.Expression = TilesX;

    UMaterialExpressionDivide* RowDivide = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionDivide>(Material, EditorOnlyData, -600, 560);
    RowDivide->A.Expression = Frame;
    RowDivide->B.Expression = TilesX;

    UMaterialExpressionFloor* Row = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionFloor>(Material, EditorOnlyData, -400, 560);
    Row->Input.Expression = RowDivide;

    UMaterialExpressionMultiply* ColumnOffset = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionMultiply>(Material, EditorOnlyData, -200, 420);
    ColumnOffset->A.Expression = Column;
    ColumnOffset->B.Expression = InvTilesX;

    UMaterialExpressionMultiply* RowOffset = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionMultiply>(Material, EditorOnlyData, -200, 560);
    RowOffset->A.Expression = Row;
    RowOffset->B.Expression = InvTilesY;

    UMaterialExpressionAppendVector* Offset = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionAppendVector>(Material, EditorOnlyData, 0, 420);
    Offset->A.Expression = ColumnOffset;
    Offset->B.Expression = RowOffset;

    UMaterialExpressionAdd* AtlasUV = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionAdd>(Material, EditorOnlyData, 240, 100);
    AtlasUV->A.Expression = LocalUV;
    AtlasUV->B.Expression = Offset;

    Atlas->Coordinates.Expression = AtlasUV;

    UMaterialExpressionComponentMask* AlphaMask = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionComponentMask>(Material, EditorOnlyData, 1160, 520);
    AlphaMask->Input.Expression = Atlas;
    AlphaMask->R = false;
    AlphaMask->G = false;
    AlphaMask->B = false;
    AlphaMask->A = true;

    UMaterialExpressionMultiply* OpacityMul = RuntimeImpostorLocal::AddMaterialExpression<UMaterialExpressionMultiply>(Material, EditorOnlyData, 1400, 360);
    OpacityMul->A.Expression = AlphaMask;
    OpacityMul->B.Expression = Opacity;

    EditorOnlyData->EmissiveColor.Expression = Atlas;
    EditorOnlyData->Opacity.Expression = OpacityMul;

    Material->PostEditChange();
    Material->ForceRecompileForRendering(EMaterialShaderPrecompileMode::None);

    FAssetRegistryModule::AssetCreated(Material);
    Package->MarkPackageDirty();

    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    UPackage::SavePackage(Package, Material, *Filename, SaveArgs);

    ImpostorMaterial = Material;
    UE_LOG(LogTemp, Display, TEXT("RuntimeImpostors: created %s"), *GetDefaultMaterialObjectPath());
}
#endif
