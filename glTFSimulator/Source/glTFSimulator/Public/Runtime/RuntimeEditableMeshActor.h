// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Runtime/RuntimePlacementTypes.h"
#include "RuntimeEditableMeshActor.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UProceduralMeshComponent;
class USceneComponent;

UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API ARuntimeEditableMeshActor : public AActor
{
    GENERATED_BODY()

public:
    ARuntimeEditableMeshActor();

    UFUNCTION(BlueprintCallable, Category="Runtime Mesh Edit")
    void BeginObject(const FString& InRuntimeName);

    UFUNCTION(BlueprintCallable, Category="Runtime Mesh Edit")
    void BeginEditingExistingObject();

    /** Adds a brand-new vertex at a world-space location and appends that vertex to the active polygon chain. */
    UFUNCTION(BlueprintCallable, Category="Runtime Mesh Edit")
    int32 AddVertexWorld(const FVector& WorldLocation);

    /** Appends an already-existing vertex to the active polygon chain, so close/merge operations reuse the same vertex index. */
    UFUNCTION(BlueprintCallable, Category="Runtime Mesh Edit")
    bool AddExistingVertexToCurrentFace(int32 VertexIndex);

    /** Moves an existing vertex in world space while preserving every triangle that references that vertex. */
    UFUNCTION(BlueprintCallable, Category="Runtime Mesh Edit")
    bool MoveVertexWorld(int32 VertexIndex, const FVector& WorldLocation);

    /** Selects an existing vertex as the start of the next connected vertex/face chain. */
    UFUNCTION(BlueprintCallable, Category="Runtime Mesh Edit")
    bool StartConnectedVertexFromIndex(int32 SourceVertexIndex);

    /** Clears the active connected source and only drops the temporary one-vertex chain when no real face has started yet. */
    UFUNCTION(BlueprintCallable, Category="Runtime Mesh Edit")
    void ClearConnectedVertexSource();

    /** Returns the vertex index that the next new edge starts from, or INDEX_NONE when no connected chain is active. */
    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit")
    int32 GetConnectedVertexSourceIndex() const { return ConnectedVertexSourceIndex; }

    /** Returns how many vertices are currently participating in the active polygon chain. */
    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit")
    int32 GetCurrentFaceVertexCount() const { return CurrentFace.Num(); }

    /** Returns whether the supplied vertex already belongs to the active polygon chain. */
    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit")
    bool IsVertexInCurrentFace(int32 VertexIndex) const { return CurrentFace.Contains(VertexIndex); }

    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit")
    FVector GetVertexWorldLocation(int32 VertexIndex) const;

    UFUNCTION(BlueprintCallable, Category="Runtime Mesh Edit")
    void FinalizeObject();

    UFUNCTION(BlueprintCallable, Category="Runtime Mesh Edit")
    void ClearMesh();

    UFUNCTION(BlueprintCallable, Category="Runtime Mesh Edit|Preview")
    void SetPreviewVertexWorld(const FVector& WorldLocation);

    UFUNCTION(BlueprintCallable, Category="Runtime Mesh Edit|Preview")
    void ClearPreviewVertex();

    UFUNCTION(BlueprintCallable, Category="Runtime Mesh Edit|Preview")
    void SetEditPreviewVisible(bool bVisible);

    UFUNCTION(BlueprintCallable, Category="Runtime Mesh Edit|Preview")
    void SetHighlightedVertexIndex(int32 VertexIndex, bool bMoving = false);

    UFUNCTION(BlueprintCallable, Category="Runtime Mesh Edit")
    FRuntimeGeneratedMeshRecord ToGeneratedMeshRecord() const;

    UFUNCTION(BlueprintCallable, Category="Runtime Mesh Edit")
    void LoadFromGeneratedMeshRecord(const FRuntimeGeneratedMeshRecord& Record);

    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit")
    bool IsFinalized() const { return bFinalized; }

    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit")
    bool HasAnyVertex() const { return Vertices.Num() > 0; }

    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit")
    bool HasAnyTriangle() const { return Triangles.Num() >= 3; }

    /** Returns true when the active edit only contains a point/line that should be canceled instead of finalized. */
    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit")
    bool HasDanglingPointOrLineEdit() const;

    /** Removes an unfinished one/two-vertex branch while keeping already renderable faces. */
    UFUNCTION(BlueprintCallable, Category="Runtime Mesh Edit")
    bool DiscardDanglingPointOrLineEdit();

    /** Returns true only when this actor has enough valid topology to become a finalized world object. */
    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit")
    bool CanFinalizeAsObject() const;

    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit")
    bool IsTopologyValid() const;

    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit")
    FVector GetLastVertexWorld() const;

    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit")
    TArray<FVector> GetVertices() const { return Vertices; }

    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit")
    TArray<int32> GetTriangles() const { return Triangles; }

    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit")
    FString GetRuntimeName() const { return RuntimeName; }

    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit|Preview")
    bool HasPreviewVertex() const { return bHasPreviewVertex; }

    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit|Preview")
    FVector GetPreviewVertexWorld() const;

    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit|Preview")
    int32 GetHighlightedVertexIndex() const { return HighlightedVertexIndex; }

    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit|Preview")
    bool IsHighlightedVertexMoving() const { return bHighlightedVertexMoving; }

    UFUNCTION(BlueprintPure, Category="Runtime Mesh Edit|Selection")
    bool FindNearestVertexToRay(const FVector& RayStart, const FVector& RayDirection, float MaxDistance, int32& OutVertexIndex, FVector& OutVertexWorldLocation, float& OutDistance) const;

private:
    UPROPERTY(VisibleAnywhere, Category="Runtime Mesh Edit")
    TObjectPtr<USceneComponent> Root;

    /** Final/committed triangle mesh. It remains visible in the world after FinalizeObject. */
    UPROPERTY(VisibleAnywhere, Category="Runtime Mesh Edit")
    TObjectPtr<UProceduralMeshComponent> MeshComponent;

    /** Point, edge, selected-vertex, and candidate-face helper mesh for in-progress runtime modeling. */
    UPROPERTY(VisibleAnywhere, Category="Runtime Mesh Edit|Preview")
    TObjectPtr<UProceduralMeshComponent> PreviewComponent;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Mesh Edit|Preview", meta=(AllowPrivateAccess="true"))
    bool bShowEditPreview = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Mesh Edit|Preview", meta=(AllowPrivateAccess="true"))
    bool bShowCandidateFacePreview = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Mesh Edit|Preview", meta=(AllowPrivateAccess="true"))
    float PreviewVertexSize = 16.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Mesh Edit|Preview", meta=(AllowPrivateAccess="true"))
    float SelectedVertexPreviewScale = 1.8f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Mesh Edit|Preview", meta=(AllowPrivateAccess="true"))
    float PreviewEdgeThickness = 6.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Mesh Edit|Materials", meta=(AllowPrivateAccess="true"))
    TObjectPtr<UMaterialInterface> GeneratedMeshBaseMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Runtime Mesh Edit|Materials", meta=(AllowPrivateAccess="true"))
    TObjectPtr<UMaterialInterface> PreviewVertexColorMaterial;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> GeneratedMeshMaterialInstance;

    UPROPERTY()
    FString RuntimeName;

    UPROPERTY()
    FString BaseName;

    UPROPERTY()
    TArray<FVector> Vertices;

    UPROPERTY()
    TArray<int32> Triangles;

    /** Stores the ordered vertex indices of the polygon currently being drawn or extended. */
    UPROPERTY()
    TArray<int32> CurrentFace;

    /** Stores where the currently edited polygon's generated triangles begin inside Triangles. */
    int32 CurrentFaceTriangleStartIndex = 0;

    /** True after the mesh has been finalized and converted into a normal world object. */
    bool bFinalized = false;
    bool bHasPreviewVertex = false;
    FVector PreviewVertex = FVector::ZeroVector;
    int32 HighlightedVertexIndex = INDEX_NONE;
    bool bHighlightedVertexMoving = false;
    int32 ConnectedVertexSourceIndex = INDEX_NONE;

    void RebuildMesh(bool bCreateCollision);
    void RebuildPreviewMesh();
    void AddOrUpdateFace();
    void ApplyRuntimeMaterials();
    bool IsTopologyValidFor(const TArray<FVector>& TestVertices, const TArray<int32>& TestTriangles) const;
    bool CanAppendVertexToCurrentFace(int32 VertexIndex, bool& bOutClosedToFirstVertex) const;
    int32 ClampCurrentFaceTriangleStartIndex(const TArray<int32>& TriangleArray) const;
    void AddOrUpdateFaceForArrays(TArray<int32>& InOutTriangles, const TArray<int32>& Face, int32 ReplaceFromIndex) const;
    void AppendPreviewBox(TArray<FVector>& OutVertices, TArray<int32>& OutTriangles, TArray<FColor>& OutColors, const FVector& Center, const FVector& Extent, const FColor& Color) const;
    void AppendPreviewSegment(TArray<FVector>& OutVertices, TArray<int32>& OutTriangles, TArray<FColor>& OutColors, const FVector& Start, const FVector& End, float Thickness, const FColor& Color) const;
    void AppendPreviewEdgeIfValid(TSet<FIntPoint>& EdgeSet, int32 A, int32 B) const;
};
