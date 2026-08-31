// Copyright 2026 OpenAI. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "RuntimeImpostorTypes.h"
#include "RuntimeImpostorComponent.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UProceduralMeshComponent;
class UTextureRenderTarget2D;
class USceneCaptureComponent2D;
class UPrimitiveComponent;
class UglTFRuntimeAsset;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRuntimeImpostorBakeEvent, bool, bSuccess);

/**
 * Runtime-generated multi-view billboard.
 *
 * The implementation intentionally targets engine-native SceneCapture + RenderTarget +
 * ProceduralMesh primitives. It does not depend on any Unity package code or asset format.
 */
UCLASS(ClassGroup=(Rendering), meta=(BlueprintSpawnableComponent))
class RUNTIMEIMPOSTORS_API URuntimeImpostorComponent final : public USceneComponent
{
    GENERATED_BODY()

public:
    URuntimeImpostorComponent();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Impostor")
    FRuntimeImpostorBakeSettings BakeSettings;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Impostor")
    FRuntimeImpostorLODSettings LODSettings;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Impostor|Assets")
    TObjectPtr<UMaterialInterface> ImpostorMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Impostor|Runtime")
    bool bTickBillboard = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Impostor|Runtime")
    bool bRebuildWhenSourceChanges = false;

    UPROPERTY(BlueprintAssignable, Category="Runtime Impostor")
    FRuntimeImpostorBakeEvent OnBakeFinished;

    UFUNCTION(BlueprintCallable, Category="Runtime Impostor")
    bool BakeFromActor(AActor* SourceActor);

    /**
     * Convenience path for an already-created glTFRuntime asset. This builds a temporary
     * preview actor from the asset's static mesh nodes, bakes it, then destroys the preview.
     * It is deliberately separate from BakeFromActor so existing streaming actors remain untouched. The generated impostor is standalone and does not retain the transient preview actor.
     */
    UFUNCTION(BlueprintCallable, Category="Runtime Impostor|glTFRuntime")
    bool BakeFromGlTFRuntimeAsset(UglTFRuntimeAsset* Asset);

    UFUNCTION(BlueprintCallable, Category="Runtime Impostor")
    void ClearBakedImpostor();

    UFUNCTION(BlueprintCallable, Category="Runtime Impostor")
    bool IsReady() const { return State == ERuntimeImpostorState::Ready; }

    UFUNCTION(BlueprintCallable, Category="Runtime Impostor")
    float GetBakeProgress() const { return BakeProgress; }

    UFUNCTION(BlueprintCallable, Category="Runtime Impostor")
    ERuntimeImpostorState GetState() const { return State; }

#if WITH_EDITOR
    UFUNCTION(CallInEditor, Category="Runtime Impostor|Editor")
    void CreateDefaultImpostorMaterial();
#endif

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
    UPROPERTY(Transient)
    TObjectPtr<UProceduralMeshComponent> BillboardMesh;

    UPROPERTY(Transient)
    TObjectPtr<USceneCaptureComponent2D> CaptureComponent;

    UPROPERTY(Transient)
    TObjectPtr<UTextureRenderTarget2D> AtlasRenderTarget;

    UPROPERTY(Transient)
    TObjectPtr<UTextureRenderTarget2D> ViewRenderTarget;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> DynamicMaterial;

    UPROPERTY(Transient)
    TObjectPtr<USceneComponent> CaptureRoot;

    TWeakObjectPtr<AActor> SourceActor;
    TArray<TWeakObjectPtr<UPrimitiveComponent>> SourcePrimitives;
    TArray<bool> SourceOriginalVisibility;
    TArray<TEnumAsByte<ECollisionEnabled::Type>> SourceOriginalCollision;

    ERuntimeImpostorState State = ERuntimeImpostorState::Uninitialized;
    float BakeProgress = 0.0f;
    FVector LocalBoundsCenter = FVector::ZeroVector;
    FVector BoundsExtent = FVector(50.0f);
    float BillboardWidth = 100.0f;
    float BillboardHeight = 100.0f;
    float CurrentLODAlpha = 0.0f;
    float LastLODAlpha = -1.0f;
    int32 LastViewIndex = INDEX_NONE;
    FTransform LastSourceTransform = FTransform::Identity;
    FName AtlasParameterName = TEXT("ImpostorAtlas");
    FName TilesXParameterName = TEXT("ImpostorTilesX");
    FName TilesYParameterName = TEXT("ImpostorTilesY");
    FName ViewIndexParameterName = TEXT("ImpostorViewIndex");
    FName OpacityParameterName = TEXT("ImpostorOpacity");

    bool InitializeRenderObjects();
    bool CaptureSourceBounds();
    bool CaptureAtlas();
    bool CreateBillboardMesh();
    bool ResolveMaterial();
    void UpdateBillboardFacing();
    void UpdateLOD(float DeltaTime);
    int32 CalculateViewIndex(const FVector& CameraLocation) const;
    void SetSourceVisibility(bool bVisible);
    void RestoreSourceState();
    void FinishBake(bool bSuccess, const FString& Reason);
    void SyncToSourceTransform();
    FVector GetCaptureCenterWorld() const;
    float GetCaptureRadius() const;
    bool IsUsableSource(AActor* Actor) const;
    bool CaptureOneView(int32 ViewIndex);
    bool ComposeViewIntoAtlas(int32 ViewIndex);
    void BuildCaptureShowOnlyList();
    void ConfigureCaptureFlags();
    bool ResolveDefaultMaterialObject();

    static FString GetDefaultMaterialObjectPath();
};
