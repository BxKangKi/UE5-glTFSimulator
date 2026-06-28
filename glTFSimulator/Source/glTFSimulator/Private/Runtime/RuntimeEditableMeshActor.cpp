// Copyright © 2026 BxKangKi. Licensed under the MIT License.

// Runtime mesh authoring note: the code below intentionally contains dense comments because these paths are edited from Blueprint-facing runtime tools.
// Runtime mesh authoring note: each changed block explains what state it reads, what state it writes, and why that state is needed during live polygon editing.

#include "Runtime/RuntimeEditableMeshActor.h"
#include "Components/SceneComponent.h"
#include "Engine/EngineTypes.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"

ARuntimeEditableMeshActor::ARuntimeEditableMeshActor()
{
    PrimaryActorTick.bCanEverTick = false;
    Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SetRootComponent(Root);

    MeshComponent = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("GeneratedMesh"));
    MeshComponent->SetupAttachment(Root);
    MeshComponent->SetCollisionProfileName(TEXT("BlockAll"));
    MeshComponent->bUseAsyncCooking = true;
    MeshComponent->SetVisibility(true, true);
    MeshComponent->SetHiddenInGame(false, true);

    PreviewComponent = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("EditPreviewMesh"));
    PreviewComponent->SetupAttachment(Root);
    PreviewComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    PreviewComponent->SetGenerateOverlapEvents(false);
    PreviewComponent->bUseAsyncCooking = true;
    PreviewComponent->SetVisibility(true, true);
    PreviewComponent->SetHiddenInGame(false, true);

    GeneratedMeshBaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    if (!IsValid(GeneratedMeshBaseMaterial))
    {
        GeneratedMeshBaseMaterial = UMaterial::GetDefaultMaterial(EMaterialDomain::MD_Surface);
    }
    PreviewVertexColorMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial"));
}

void ARuntimeEditableMeshActor::ApplyRuntimeMaterials()
{
    if (IsValid(MeshComponent) && IsValid(GeneratedMeshBaseMaterial))
    {
        if (!IsValid(GeneratedMeshMaterialInstance))
        {
            GeneratedMeshMaterialInstance = UMaterialInstanceDynamic::Create(GeneratedMeshBaseMaterial, this);
            if (IsValid(GeneratedMeshMaterialInstance))
            {
                // These parameters are ignored by plain engine materials, but lit glTF-compatible materials use them.
                GeneratedMeshMaterialInstance->SetScalarParameterValue(TEXT("emissiveStrength"), 0.05f);
                GeneratedMeshMaterialInstance->SetVectorParameterValue(TEXT("emissiveFactor"), FLinearColor(0.02f, 0.02f, 0.02f, 1.0f));
            }
        }
        MeshComponent->SetMaterial(0, IsValid(GeneratedMeshMaterialInstance) ? GeneratedMeshMaterialInstance.Get() : GeneratedMeshBaseMaterial.Get());
    }

    if (IsValid(PreviewComponent) && IsValid(PreviewVertexColorMaterial))
    {
        PreviewComponent->SetMaterial(0, PreviewVertexColorMaterial);
        PreviewComponent->SetMaterial(1, PreviewVertexColorMaterial);
    }
}

void ARuntimeEditableMeshActor::BeginObject(const FString& InRuntimeName)
{
    RuntimeName = InRuntimeName.IsEmpty() ? TEXT("GeneratedMesh") : InRuntimeName;
    BaseName = RuntimeName;
    FString IgnoredSuffix;
    RuntimeName.Split(TEXT(";"), &BaseName, &IgnoredSuffix, ESearchCase::CaseSensitive, ESearchDir::FromStart);
    bFinalized = false;
    CurrentFaceTriangleStartIndex = 0; // A new object has no committed triangles, so the active polygon starts at triangle slot zero.
    HighlightedVertexIndex = INDEX_NONE;
    bHighlightedVertexMoving = false;
    ConnectedVertexSourceIndex = INDEX_NONE;
    ClearMesh();
    SetActorHiddenInGame(false);
    SetActorEnableCollision(true);
    RebuildPreviewMesh();
}

void ARuntimeEditableMeshActor::BeginEditingExistingObject()
{
    bFinalized = false;
    bHasPreviewVertex = false;
    PreviewVertex = FVector::ZeroVector;
    CurrentFace.Empty();
    CurrentFaceTriangleStartIndex = Triangles.Num(); // Existing triangles are treated as committed, so new polygon edits append after them.
    HighlightedVertexIndex = INDEX_NONE;
    bHighlightedVertexMoving = false;
    ConnectedVertexSourceIndex = INDEX_NONE;
    SetActorHiddenInGame(false);
    SetActorEnableCollision(true);
    RebuildMesh(false);
    RebuildPreviewMesh();
}

void ARuntimeEditableMeshActor::ClearMesh()
{
    Vertices.Empty();
    Triangles.Empty();
    CurrentFace.Empty();
    CurrentFaceTriangleStartIndex = 0; // Clearing the mesh removes every generated triangle, including the live polygon range.
    bHasPreviewVertex = false;
    PreviewVertex = FVector::ZeroVector;
    HighlightedVertexIndex = INDEX_NONE;
    bHighlightedVertexMoving = false;
    ConnectedVertexSourceIndex = INDEX_NONE;
    if (IsValid(MeshComponent))
    {
        MeshComponent->ClearAllMeshSections();
        MeshComponent->SetVisibility(true, true);
        MeshComponent->SetHiddenInGame(false, true);
        MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    }
    if (IsValid(PreviewComponent))
    {
        PreviewComponent->ClearAllMeshSections();
    }
    RebuildPreviewMesh();
}

int32 ARuntimeEditableMeshActor::AddVertexWorld(const FVector& WorldLocation)
{
    // Do not allow finalized meshes to be modified through the live-edit API.
    if (bFinalized)
    {
        // INDEX_NONE tells the caller that no vertex was created.
        return INDEX_NONE;
    }

    // A brand-new polygon range starts exactly where the current triangle array ends.
    if (CurrentFace.Num() == 0)
    {
        // Remember the first triangle slot owned by this live polygon so later vertices can replace only this polygon's fan triangles.
        CurrentFaceTriangleStartIndex = Triangles.Num();
    }

    // If the user clicked an existing vertex first, seed the face with that connected source.
    if (CurrentFace.Num() == 0 && Vertices.IsValidIndex(ConnectedVertexSourceIndex))
    {
        // The source vertex becomes the first corner of the active polygon chain.
        CurrentFace.Add(ConnectedVertexSourceIndex);
    }

    // Convert the world-space cursor hit into the actor's local mesh space.
    const FVector Local = GetActorTransform().InverseTransformPosition(WorldLocation);

    // Store the new local-space point and keep the returned index for face construction and UI messages.
    const int32 NewIndex = Vertices.Add(Local);

    // Append the new vertex to the active polygon instead of stopping at triangles or quads.
    CurrentFace.Add(NewIndex);

    // Treat the newest point as the next edge source, so continued clicks extend the chain naturally.
    ConnectedVertexSourceIndex = NewIndex;

    // Highlight the newest point so the user gets immediate selection feedback.
    HighlightedVertexIndex = NewIndex;

    // New vertices are not drag edits, so clear the moving highlight state.
    bHighlightedVertexMoving = false;

    // A real vertex was committed, so the temporary preview point is no longer needed.
    bHasPreviewVertex = false;

    // Reset the preview location so stale data cannot draw a ghost point.
    PreviewVertex = FVector::ZeroVector;

    // Rebuild the live polygon as an n-gon triangle fan from the first vertex.
    AddOrUpdateFace();

    // Rebuild the visible mesh without final collision because the user is still editing.
    RebuildMesh(false);

    // Rebuild helper points, helper edges, and red/green topology feedback.
    RebuildPreviewMesh();

    // Return the index so gameplay/UI code can show exactly which vertex was added.
    return NewIndex;
}

bool ARuntimeEditableMeshActor::AddExistingVertexToCurrentFace(int32 VertexIndex)
{
    // This flag becomes true when the user clicks the first vertex again to close the currently open polygon visually.
    bool bClosedToFirstVertex = false;

    // Reject finalized meshes, invalid indices, same-vertex edges, and duplicate vertices that would corrupt the polygon chain.
    if (bFinalized || !CanAppendVertexToCurrentFace(VertexIndex, bClosedToFirstVertex))
    {
        // False tells the caller to keep the previous state and usually show a warning message.
        return false;
    }

    // A connection started from an existing vertex needs its triangle replacement range to begin after committed geometry.
    if (CurrentFace.Num() == 0)
    {
        // Existing triangles before this index are preserved when the active polygon is rebuilt.
        CurrentFaceTriangleStartIndex = Triangles.Num();
    }

    // Closing back to the first vertex should not duplicate that vertex in the face array.
    if (!bClosedToFirstVertex)
    {
        // Appending an existing index performs the requested merge because no duplicate vertex is created.
        CurrentFace.Add(VertexIndex);
    }

    // The clicked/merged vertex becomes the next connected source for any following edge.
    ConnectedVertexSourceIndex = VertexIndex;

    // Highlight the merged target so selection feedback is visible immediately.
    HighlightedVertexIndex = VertexIndex;

    // A merge click is not a drag operation, so ensure the highlight uses the normal selected color.
    bHighlightedVertexMoving = false;

    // The user clicked an existing vertex, so remove any temporary world-position preview point.
    bHasPreviewVertex = false;

    // Reset the preview point value to avoid stale helper geometry.
    PreviewVertex = FVector::ZeroVector;

    // Rebuild or replace the active polygon's fan triangles while preserving committed triangles before the active range.
    AddOrUpdateFace();

    // Refresh the procedural mesh so the merged edge/face appears immediately.
    RebuildMesh(false);

    // Refresh helper geometry so the selected/merged vertex remains highlighted.
    RebuildPreviewMesh();

    // True tells the manager that the merge/connection was accepted.
    return true;
}

bool ARuntimeEditableMeshActor::MoveVertexWorld(int32 VertexIndex, const FVector& WorldLocation)
{
    if (bFinalized || !Vertices.IsValidIndex(VertexIndex))
    {
        return false;
    }

    Vertices[VertexIndex] = GetActorTransform().InverseTransformPosition(WorldLocation);
    bHasPreviewVertex = false;
    PreviewVertex = FVector::ZeroVector;
    HighlightedVertexIndex = VertexIndex;
    RebuildMesh(false);
    RebuildPreviewMesh();
    return true;
}

bool ARuntimeEditableMeshActor::StartConnectedVertexFromIndex(int32 SourceVertexIndex)
{
    // A finalized mesh must first be reopened through BeginEditingExistingObject before vertices can be connected.
    if (bFinalized || !Vertices.IsValidIndex(SourceVertexIndex))
    {
        // False tells the manager that the requested source cannot be used.
        return false;
    }

    // The source vertex is the first point of a new active polygon chain.
    ConnectedVertexSourceIndex = SourceVertexIndex;

    // Starting a new chain deliberately leaves existing committed triangles intact.
    CurrentFace.Empty();

    // Future AddOrUpdateFace calls will replace only triangles appended after this point.
    CurrentFaceTriangleStartIndex = Triangles.Num();

    // Seed the active face with the clicked source vertex.
    CurrentFace.Add(SourceVertexIndex);

    // Highlight the source vertex so the user sees where the next segment begins.
    HighlightedVertexIndex = SourceVertexIndex;

    // This is a selection click, not a drag, so use the normal selected color.
    bHighlightedVertexMoving = false;

    // Rebuild helper geometry so the connected-source marker appears immediately.
    RebuildPreviewMesh();

    // True tells the manager that connected creation mode is now active.
    return true;
}

void ARuntimeEditableMeshActor::ClearConnectedVertexSource()
{
    // Clear the manager-visible edge source first.
    ConnectedVertexSourceIndex = INDEX_NONE;

    // A single-vertex face is only a pending source marker, so it can be removed safely.
    if (CurrentFace.Num() == 1)
    {
        // Preserve real polygons with two or more vertices so work is not lost when a mode changes.
        CurrentFace.Empty();

        // Reset the live-triangle range to the end of the committed triangle array.
        CurrentFaceTriangleStartIndex = Triangles.Num();
    }

    // Refresh helper geometry after the source marker changes.
    RebuildPreviewMesh();
}

FVector ARuntimeEditableMeshActor::GetVertexWorldLocation(int32 VertexIndex) const
{
    if (!Vertices.IsValidIndex(VertexIndex))
    {
        return GetActorLocation();
    }
    return GetActorTransform().TransformPosition(Vertices[VertexIndex]);
}

void ARuntimeEditableMeshActor::SetPreviewVertexWorld(const FVector& WorldLocation)
{
    if (bFinalized)
    {
        return;
    }

    PreviewVertex = GetActorTransform().InverseTransformPosition(WorldLocation);
    bHasPreviewVertex = true;
    RebuildPreviewMesh();
}

void ARuntimeEditableMeshActor::ClearPreviewVertex()
{
    bHasPreviewVertex = false;
    PreviewVertex = FVector::ZeroVector;
    RebuildPreviewMesh();
}

void ARuntimeEditableMeshActor::SetEditPreviewVisible(bool bVisible)
{
    bShowEditPreview = bVisible;
    RebuildPreviewMesh();
}

void ARuntimeEditableMeshActor::SetHighlightedVertexIndex(int32 VertexIndex, bool bMoving)
{
    const int32 NewIndex = Vertices.IsValidIndex(VertexIndex) ? VertexIndex : INDEX_NONE;
    if (HighlightedVertexIndex == NewIndex && bHighlightedVertexMoving == bMoving)
    {
        return;
    }

    HighlightedVertexIndex = NewIndex;
    bHighlightedVertexMoving = NewIndex != INDEX_NONE && bMoving;
    RebuildPreviewMesh();
}

bool ARuntimeEditableMeshActor::CanAppendVertexToCurrentFace(int32 VertexIndex, bool& bOutClosedToFirstVertex) const
{
    // Default to a normal append unless this function proves that the click closes the face.
    bOutClosedToFirstVertex = false;

    // Invalid vertex indices cannot participate in a real edge or face.
    if (!Vertices.IsValidIndex(VertexIndex))
    {
        // Rejecting invalid indices prevents procedural mesh sections from referencing missing vertices.
        return false;
    }

    // An empty active face can always start from any valid vertex.
    if (CurrentFace.Num() == 0)
    {
        // True lets AddExistingVertexToCurrentFace seed the face with this existing vertex.
        return true;
    }

    // Adding the same vertex twice in a row would create a zero-length edge.
    if (VertexIndex == CurrentFace.Last())
    {
        // Reject the append because it would immediately produce degenerate topology.
        return false;
    }

    // Check whether this vertex already exists somewhere in the active polygon chain.
    const int32 ExistingFaceIndex = CurrentFace.IndexOfByKey(VertexIndex);

    // Vertices not already in the chain can be appended normally.
    if (ExistingFaceIndex == INDEX_NONE)
    {
        // True means this is a clean merge/connection to an existing mesh vertex.
        return true;
    }

    // Clicking the first vertex after at least three corners means the user is closing the polygon loop.
    if (ExistingFaceIndex == 0 && CurrentFace.Num() >= 3)
    {
        // Mark the operation as a close action so the first vertex is not duplicated at the end of the face array.
        bOutClosedToFirstVertex = true;

        // Closing to the first vertex is allowed because the fan triangulation already treats the polygon as closed.
        return true;
    }

    // Reusing any other in-chain vertex would create a bow-tie/repeated-index polygon.
    return false;
}

int32 ARuntimeEditableMeshActor::ClampCurrentFaceTriangleStartIndex(const TArray<int32>& TriangleArray) const
{
    // Clamp protects old save data or hot-reload state from leaving the start index outside the current triangle array.
    return FMath::Clamp(CurrentFaceTriangleStartIndex, 0, TriangleArray.Num());
}

void ARuntimeEditableMeshActor::AddOrUpdateFaceForArrays(TArray<int32>& InOutTriangles, const TArray<int32>& Face, int32 ReplaceFromIndex) const
{
    // Clamp the replacement start so RemoveAt never receives an invalid range.
    const int32 SafeReplaceFromIndex = FMath::Clamp(ReplaceFromIndex, 0, InOutTriangles.Num());

    // Remove the previous live-polygon fan while preserving committed triangles before SafeReplaceFromIndex.
    if (InOutTriangles.Num() > SafeReplaceFromIndex)
    {
        // This is the key change that lets a triangle grow into a quad, pentagon, hexagon, and beyond without breaking the chain.
        InOutTriangles.RemoveAt(SafeReplaceFromIndex, InOutTriangles.Num() - SafeReplaceFromIndex, EAllowShrinking::No);
    }

    // Fewer than three vertices form only points/lines, not a renderable face.
    if (Face.Num() < 3)
    {
        // Leave the triangle array with only committed triangles.
        return;
    }

    // Copy the face because a closing click may put the first vertex conceptually at the end without storing a duplicate.
    TArray<int32> SanitizedFace = Face;

    // Remove a duplicated closing vertex if any older state or Blueprint call inserted one explicitly.
    if (SanitizedFace.Num() > 1 && SanitizedFace[0] == SanitizedFace.Last())
    {
        // The triangle fan closes the polygon automatically, so the duplicate endpoint is not needed.
        SanitizedFace.RemoveAt(SanitizedFace.Num() - 1, 1, EAllowShrinking::No);
    }

    // After sanitizing, fewer than three vertices still cannot create a face.
    if (SanitizedFace.Num() < 3)
    {
        // Return without adding triangles so the preview remains red/incomplete.
        return;
    }

    // Triangulate any n-gon as a simple fan rooted at the first vertex.
    for (int32 FaceIndex = 1; FaceIndex + 1 < SanitizedFace.Num(); ++FaceIndex)
    {
        // First corner of every fan triangle.
        InOutTriangles.Add(SanitizedFace[0]);

        // Current middle corner of the fan triangle.
        InOutTriangles.Add(SanitizedFace[FaceIndex]);

        // Next outer corner of the fan triangle.
        InOutTriangles.Add(SanitizedFace[FaceIndex + 1]);
    }
}

void ARuntimeEditableMeshActor::AddOrUpdateFace()
{
    // Replace only the triangle range that belongs to the active polygon.
    AddOrUpdateFaceForArrays(Triangles, CurrentFace, ClampCurrentFaceTriangleStartIndex(Triangles));

    // Do not clear CurrentFace at triangle or quad size; the user can keep adding vertices to form any n-gon.
}

bool ARuntimeEditableMeshActor::IsTopologyValidFor(const TArray<FVector>& TestVertices, const TArray<int32>& TestTriangles) const
{
    if (TestTriangles.Num() < 3 || (TestTriangles.Num() % 3) != 0)
    {
        return false;
    }

    for (int32 TriIndex = 0; TriIndex + 2 < TestTriangles.Num(); TriIndex += 3)
    {
        const int32 A = TestTriangles[TriIndex];
        const int32 B = TestTriangles[TriIndex + 1];
        const int32 C = TestTriangles[TriIndex + 2];
        if (!TestVertices.IsValidIndex(A) || !TestVertices.IsValidIndex(B) || !TestVertices.IsValidIndex(C))
        {
            return false;
        }
        if (A == B || B == C || C == A)
        {
            return false;
        }

        const FVector AB = TestVertices[B] - TestVertices[A];
        const FVector AC = TestVertices[C] - TestVertices[A];
        if (FVector::CrossProduct(AB, AC).SizeSquared() <= 1.0f)
        {
            return false;
        }
    }

    return true;
}

bool ARuntimeEditableMeshActor::HasDanglingPointOrLineEdit() const
{
    // A face with zero vertices has no pending point/line edit.
    if (CurrentFace.Num() == 0)
    {
        // Nothing active means nothing can be dangling.
        return false;
    }

    // Three or more vertices can produce a real polygon face, so this helper only cares about one/two-vertex edits.
    if (CurrentFace.Num() >= 3)
    {
        // Let normal topology validation judge real faces.
        return false;
    }

    // If the whole actor only has one or two vertices, it is definitely just a point/line object and must be canceled.
    if (Vertices.Num() <= 2)
    {
        // This covers newly created objects where the user right-clicks after placing only one or two vertices.
        return true;
    }

    // Existing triangles before CurrentFaceTriangleStartIndex are treated as committed/finalized geometry.
    const int32 SafeCommittedTriangleEnd = FMath::Clamp(CurrentFaceTriangleStartIndex, 0, Triangles.Num());

    // Build a small reference set of vertices already used by committed triangles.
    TSet<int32> CommittedVertexIndices;

    // Walk only the committed triangle range so the active incomplete edit does not count as valid geometry.
    for (int32 TriIndex = 0; TriIndex + 2 < SafeCommittedTriangleEnd; TriIndex += 3)
    {
        // Store the first triangle corner if the index is valid.
        if (Vertices.IsValidIndex(Triangles[TriIndex]))
        {
            CommittedVertexIndices.Add(Triangles[TriIndex]);
        }

        // Store the second triangle corner if the index is valid.
        if (Vertices.IsValidIndex(Triangles[TriIndex + 1]))
        {
            CommittedVertexIndices.Add(Triangles[TriIndex + 1]);
        }

        // Store the third triangle corner if the index is valid.
        if (Vertices.IsValidIndex(Triangles[TriIndex + 2]))
        {
            CommittedVertexIndices.Add(Triangles[TriIndex + 2]);
        }
    }

    // If any active one/two-vertex chain point is not part of committed geometry, it is an orphan point/line edit.
    for (const int32 FaceVertexIndex : CurrentFace)
    {
        // Invalid indices are unsafe to finalize.
        if (!Vertices.IsValidIndex(FaceVertexIndex))
        {
            // Treat invalid active face entries as dangling so the manager cancels/reverts the edit.
            return true;
        }

        // A newly added point/line endpoint will not be referenced by committed triangles yet.
        if (!CommittedVertexIndices.Contains(FaceVertexIndex))
        {
            // This is the exact case that used to leave a detached cursor or unusable one/two-vertex object.
            return true;
        }
    }

    // A one-vertex source marker made from an already committed vertex is just selection state, not a dangling edit.
    return false;
}

bool ARuntimeEditableMeshActor::DiscardDanglingPointOrLineEdit()
{
    // Only one/two-vertex active chains are considered dangling point/line edits.
    if (!HasDanglingPointOrLineEdit())
    {
        // Nothing was removed.
        return false;
    }

    // Collect every vertex index used by any real triangle that currently exists.
    TSet<int32> ReferencedByTriangles;

    // Walk the whole triangle array because any triangle is real renderable geometry at this point.
    for (int32 TriIndex = 0; TriIndex + 2 < Triangles.Num(); TriIndex += 3)
    {
        // Add the first corner when valid.
        if (Vertices.IsValidIndex(Triangles[TriIndex]))
        {
            ReferencedByTriangles.Add(Triangles[TriIndex]);
        }

        // Add the second corner when valid.
        if (Vertices.IsValidIndex(Triangles[TriIndex + 1]))
        {
            ReferencedByTriangles.Add(Triangles[TriIndex + 1]);
        }

        // Add the third corner when valid.
        if (Vertices.IsValidIndex(Triangles[TriIndex + 2]))
        {
            ReferencedByTriangles.Add(Triangles[TriIndex + 2]);
        }
    }

    // Store unreferenced vertices from the active point/line branch so they can be removed from the vertex array.
    TArray<int32> VertexIndicesToRemove;

    // Inspect only the active one/two-vertex chain.
    for (const int32 FaceVertexIndex : CurrentFace)
    {
        // Invalid face indices are fixed by clearing CurrentFace below; there is no vertex array entry to remove.
        if (!Vertices.IsValidIndex(FaceVertexIndex))
        {
            // Skip invalid values.
            continue;
        }

        // Vertices that are not used by triangles are the dangling endpoints that caused the cursor/state bug.
        if (!ReferencedByTriangles.Contains(FaceVertexIndex))
        {
            // AddUnique prevents double removal when the same index appears twice through bad hot-reload state.
            VertexIndicesToRemove.AddUnique(FaceVertexIndex);
        }
    }

    // Remove vertices from highest index to lowest so lower stored indices stay valid while deleting.
    VertexIndicesToRemove.Sort([](const int32 Left, const int32 Right)
    {
        // Returning true for the larger index first gives descending order.
        return Left > Right;
    });

    // Delete each unreferenced dangling vertex and remap triangle indices above it just in case future edits referenced higher vertices.
    for (const int32 RemoveIndex : VertexIndicesToRemove)
    {
        // Skip stale indices defensively.
        if (!Vertices.IsValidIndex(RemoveIndex))
        {
            // Continue removing any other still-valid indices.
            continue;
        }

        // Remove the orphan vertex from the mesh's vertex buffer.
        Vertices.RemoveAt(RemoveIndex, 1, EAllowShrinking::No);

        // Shift triangle references above the removed vertex down by one.
        for (int32& TriangleVertexIndex : Triangles)
        {
            // Only indices above the removed slot need to change.
            if (TriangleVertexIndex > RemoveIndex)
            {
                --TriangleVertexIndex;
            }
        }
    }

    // Clear the active point/line branch so finalization no longer sees a dangling edit.
    CurrentFace.Empty();

    // The active triangle replacement range now starts after the remaining committed triangles.
    CurrentFaceTriangleStartIndex = Triangles.Num();

    // Clear connected creation state because the source line has been canceled.
    ConnectedVertexSourceIndex = INDEX_NONE;

    // Remove point/line previews from the helper component.
    bHasPreviewVertex = false;

    // Clear the stored preview position so no ghost cursor remains.
    PreviewVertex = FVector::ZeroVector;

    // Clear the highlighted vertex because the branch was canceled.
    HighlightedVertexIndex = INDEX_NONE;

    // Clear drag highlight state as well.
    bHighlightedVertexMoving = false;

    // Rebuild the mesh without collision while the manager decides whether to finalize.
    RebuildMesh(false);

    // Rebuild helpers so the canceled line disappears immediately.
    RebuildPreviewMesh();

    // Report that cleanup happened.
    return true;
}

bool ARuntimeEditableMeshActor::CanFinalizeAsObject() const
{
    // A finalized world object must have at least three vertices.
    if (Vertices.Num() < 3)
    {
        // One/two-vertex objects are intentionally canceled by the gameplay manager.
        return false;
    }

    // A finalized world object must have at least one triangle.
    if (!HasAnyTriangle())
    {
        // Points and line-only edits should never become generated mesh actors.
        return false;
    }

    // A one/two-vertex active branch means the user started an edit but did not form a face.
    if (HasDanglingPointOrLineEdit())
    {
        // Cancel/revert instead of leaving orphan vertices in the saved mesh.
        return false;
    }

    // The final gate is the existing triangle validity check.
    return IsTopologyValid();
}

bool ARuntimeEditableMeshActor::IsTopologyValid() const
{
    return IsTopologyValidFor(Vertices, Triangles);
}

void ARuntimeEditableMeshActor::FinalizeObject()
{
    bFinalized = true;
    bHasPreviewVertex = false;
    PreviewVertex = FVector::ZeroVector;
    CurrentFace.Empty();
    CurrentFaceTriangleStartIndex = Triangles.Num(); // Loaded/finalized triangles are committed, so later edits append after them.
    HighlightedVertexIndex = INDEX_NONE;
    bHighlightedVertexMoving = false;
    ConnectedVertexSourceIndex = INDEX_NONE;
    SetActorHiddenInGame(false);
    SetActorEnableCollision(true);
    RebuildMesh(true);
    RebuildPreviewMesh();
}

void ARuntimeEditableMeshActor::RebuildMesh(bool bCreateCollision)
{
    if (!IsValid(MeshComponent))
    {
        return;
    }

    MeshComponent->ClearAllMeshSections();
    MeshComponent->SetVisibility(true, true);
    MeshComponent->SetHiddenInGame(false, true);
    if (Vertices.Num() == 0 || Triangles.Num() < 3)
    {
        MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        return;
    }

    const bool bValidTopology = IsTopologyValid();
    TArray<FVector> Normals;
    Normals.Init(FVector::UpVector, Vertices.Num());
    for (int32 TriIndex = 0; TriIndex + 2 < Triangles.Num(); TriIndex += 3)
    {
        const int32 A = Triangles[TriIndex];
        const int32 B = Triangles[TriIndex + 1];
        const int32 C = Triangles[TriIndex + 2];
        if (Vertices.IsValidIndex(A) && Vertices.IsValidIndex(B) && Vertices.IsValidIndex(C))
        {
            const FVector Normal = FVector::CrossProduct(Vertices[B] - Vertices[A], Vertices[C] - Vertices[A]).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
            Normals[A] = Normal;
            Normals[B] = Normal;
            Normals[C] = Normal;
        }
    }

    TArray<FVector2D> UV0;
    UV0.Reserve(Vertices.Num());
    for (const FVector& Vertex : Vertices)
    {
        UV0.Add(FVector2D(Vertex.X * 0.01f, Vertex.Y * 0.01f));
    }
    TArray<FColor> VertexColors;
    VertexColors.Init(bValidTopology ? FColor::Green : FColor::Red, Vertices.Num());
    TArray<FProcMeshTangent> Tangents;
    Tangents.Init(FProcMeshTangent(1.0f, 0.0f, 0.0f), Vertices.Num());

    MeshComponent->CreateMeshSection(0, Vertices, Triangles, Normals, UV0, VertexColors, Tangents, bCreateCollision && bValidTopology);
    MeshComponent->SetCollisionEnabled((bCreateCollision && bValidTopology) ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::QueryOnly);
    ApplyRuntimeMaterials();
}

void ARuntimeEditableMeshActor::AppendPreviewBox(TArray<FVector>& OutVertices, TArray<int32>& OutTriangles, TArray<FColor>& OutColors, const FVector& Center, const FVector& Extent, const FColor& Color) const
{
    const int32 BaseIndex = OutVertices.Num();
    OutVertices.Add(Center + FVector(-Extent.X, -Extent.Y, -Extent.Z));
    OutVertices.Add(Center + FVector( Extent.X, -Extent.Y, -Extent.Z));
    OutVertices.Add(Center + FVector( Extent.X,  Extent.Y, -Extent.Z));
    OutVertices.Add(Center + FVector(-Extent.X,  Extent.Y, -Extent.Z));
    OutVertices.Add(Center + FVector(-Extent.X, -Extent.Y,  Extent.Z));
    OutVertices.Add(Center + FVector( Extent.X, -Extent.Y,  Extent.Z));
    OutVertices.Add(Center + FVector( Extent.X,  Extent.Y,  Extent.Z));
    OutVertices.Add(Center + FVector(-Extent.X,  Extent.Y,  Extent.Z));

    for (int32 Index = 0; Index < 8; ++Index)
    {
        OutColors.Add(Color);
    }

    const int32 LocalTriangles[] =
    {
        0, 2, 1, 0, 3, 2,
        4, 5, 6, 4, 6, 7,
        0, 1, 5, 0, 5, 4,
        1, 2, 6, 1, 6, 5,
        2, 3, 7, 2, 7, 6,
        3, 0, 4, 3, 4, 7
    };

    for (int32 Index : LocalTriangles)
    {
        OutTriangles.Add(BaseIndex + Index);
    }
}

void ARuntimeEditableMeshActor::AppendPreviewSegment(TArray<FVector>& OutVertices, TArray<int32>& OutTriangles, TArray<FColor>& OutColors, const FVector& Start, const FVector& End, float Thickness, const FColor& Color) const
{
    const FVector Delta = End - Start;
    const float Length = Delta.Size();
    if (Length <= KINDA_SMALL_NUMBER)
    {
        return;
    }

    const FVector AxisX = Delta / Length;
    FVector AxisZ = FVector::UpVector;
    if (FMath::Abs(FVector::DotProduct(AxisX, AxisZ)) > 0.95f)
    {
        AxisZ = FVector::RightVector;
    }
    const FVector AxisY = FVector::CrossProduct(AxisZ, AxisX).GetSafeNormal();
    AxisZ = FVector::CrossProduct(AxisX, AxisY).GetSafeNormal();

    const float HalfLength = Length * 0.5f;
    const float HalfThickness = FMath::Max(1.0f, Thickness * 0.5f);
    const FVector Center = (Start + End) * 0.5f;
    const int32 BaseIndex = OutVertices.Num();

    const FVector X = AxisX * HalfLength;
    const FVector Y = AxisY * HalfThickness;
    const FVector Z = AxisZ * HalfThickness;

    OutVertices.Add(Center - X - Y - Z);
    OutVertices.Add(Center + X - Y - Z);
    OutVertices.Add(Center + X + Y - Z);
    OutVertices.Add(Center - X + Y - Z);
    OutVertices.Add(Center - X - Y + Z);
    OutVertices.Add(Center + X - Y + Z);
    OutVertices.Add(Center + X + Y + Z);
    OutVertices.Add(Center - X + Y + Z);

    for (int32 Index = 0; Index < 8; ++Index)
    {
        OutColors.Add(Color);
    }

    const int32 LocalTriangles[] =
    {
        0, 2, 1, 0, 3, 2,
        4, 5, 6, 4, 6, 7,
        0, 1, 5, 0, 5, 4,
        1, 2, 6, 1, 6, 5,
        2, 3, 7, 2, 7, 6,
        3, 0, 4, 3, 4, 7
    };

    for (int32 Index : LocalTriangles)
    {
        OutTriangles.Add(BaseIndex + Index);
    }
}

void ARuntimeEditableMeshActor::AppendPreviewEdgeIfValid(TSet<FIntPoint>& EdgeSet, int32 A, int32 B) const
{
    if (!Vertices.IsValidIndex(A) || !Vertices.IsValidIndex(B) || A == B)
    {
        return;
    }

    const int32 MinIndex = FMath::Min(A, B);
    const int32 MaxIndex = FMath::Max(A, B);
    EdgeSet.Add(FIntPoint(MinIndex, MaxIndex));
}

void ARuntimeEditableMeshActor::RebuildPreviewMesh()
{
    if (!IsValid(PreviewComponent))
    {
        return;
    }

    PreviewComponent->ClearAllMeshSections();
    const bool bShouldShowPreview = bShowEditPreview && !bFinalized;
    PreviewComponent->SetVisibility(bShouldShowPreview, true);
    PreviewComponent->SetHiddenInGame(!bShouldShowPreview, true);
    PreviewComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    if (!bShouldShowPreview)
    {
        return;
    }

    const FColor TopologyColor = IsTopologyValid() ? FColor::Green : FColor::Red;
    const FColor NormalHelperColor = FColor::Cyan;
    const FColor SelectedHelperColor = bHighlightedVertexMoving ? FColor(255, 128, 0) : FColor::Yellow;
    const FColor ConnectedSourceHelperColor = FColor(255, 255, 0);
    const float SafeVertexSize = FMath::Max(2.0f, PreviewVertexSize);
    const float SafeThickness = FMath::Max(1.0f, PreviewEdgeThickness);
    const FVector VertexExtent(SafeVertexSize, SafeVertexSize, SafeVertexSize);

    // Candidate preview for placing a brand-new vertex at the center-crosshair hit location.
    if (bShowCandidateFacePreview && bHasPreviewVertex && CurrentFace.Num() >= 2)
    {
        // Copy committed vertices because the preview vertex should not be committed yet.
        TArray<FVector> CandidateVertices = Vertices;

        // Add the temporary preview point as the final candidate vertex.
        const int32 PreviewIndex = CandidateVertices.Add(PreviewVertex);

        // Copy current triangles so the candidate fan can be tested without mutating the real mesh.
        TArray<int32> CandidateTriangles = Triangles;

        // Copy the active polygon chain and append the temporary preview index.
        TArray<int32> CandidateFace = CurrentFace;

        // The temporary preview point represents the next n-gon corner.
        CandidateFace.Add(PreviewIndex);

        // Preview the next n-gon fan without duplicating the previous live fan.
        AddOrUpdateFaceForArrays(CandidateTriangles, CandidateFace, ClampCurrentFaceTriangleStartIndex(CandidateTriangles));

        // Only draw a candidate face when a real triangle can exist.
        if (CandidateTriangles.Num() >= 3)
        {
            // Red means invalid/incomplete topology; green means the candidate fan is currently valid.
            const bool bCandidateValid = IsTopologyValidFor(CandidateVertices, CandidateTriangles);

            // ProceduralMeshComponent requires a normal array even for simple debug preview geometry.
            TArray<FVector> Normals;

            // Use a simple up-vector normal per vertex because the preview is only visual feedback.
            Normals.Init(FVector::UpVector, CandidateVertices.Num());

            // ProceduralMeshComponent requires UVs, so provide zeroed UVs for debug material usage.
            TArray<FVector2D> UV0;

            // Keep all preview UVs at zero because the vertex-color material does not need meaningful UVs.
            UV0.Init(FVector2D::ZeroVector, CandidateVertices.Num());

            // Give every candidate vertex the same validity color.
            TArray<FColor> VertexColors;

            // The engine debug vertex-color material reads this green/red feedback directly.
            VertexColors.Init(bCandidateValid ? FColor::Green : FColor::Red, CandidateVertices.Num());

            // ProceduralMeshComponent requires tangents, so create a stable default tangent per vertex.
            TArray<FProcMeshTangent> Tangents;

            // A constant X-axis tangent is enough for preview shading.
            Tangents.Init(FProcMeshTangent(1.0f, 0.0f, 0.0f), CandidateVertices.Num());

            // Section 0 is reserved for translucent-looking candidate face feedback.
            PreviewComponent->CreateMeshSection(0, CandidateVertices, CandidateTriangles, Normals, UV0, VertexColors, Tangents, false);
        }
    }
    // Candidate preview for merging/connecting to an already-existing highlighted vertex.
    else if (bShowCandidateFacePreview && !bHasPreviewVertex && CurrentFace.Num() >= 2 && Vertices.IsValidIndex(HighlightedVertexIndex) && !bHighlightedVertexMoving)
    {
        // Track whether the highlighted vertex is the first point and therefore closes the polygon without adding a duplicate index.
        bool bWouldCloseToFirstVertex = false;

        // Only preview merge targets that the same validation path would allow on click.
        if (CanAppendVertexToCurrentFace(HighlightedVertexIndex, bWouldCloseToFirstVertex))
        {
            // Existing vertices are reused directly, so candidate vertices equal the real vertex array.
            TArray<FVector> CandidateVertices = Vertices;

            // Copy triangles so the preview can replace the live fan without committing it.
            TArray<int32> CandidateTriangles = Triangles;

            // Copy the live face chain before adding the highlighted merge target.
            TArray<int32> CandidateFace = CurrentFace;

            // Do not append a duplicate when the user is closing back to the first vertex.
            if (!bWouldCloseToFirstVertex)
            {
                // Appending an existing index previews the requested merge/connection.
                CandidateFace.Add(HighlightedVertexIndex);
            }

            // Rebuild the candidate n-gon fan from the same live-triangle range.
            AddOrUpdateFaceForArrays(CandidateTriangles, CandidateFace, ClampCurrentFaceTriangleStartIndex(CandidateTriangles));

            // Draw the candidate face only when it contains at least one triangle.
            if (CandidateTriangles.Num() >= 3)
            {
                // Evaluate the candidate topology so hover feedback becomes green or red before the click.
                const bool bCandidateValid = IsTopologyValidFor(CandidateVertices, CandidateTriangles);

                // Provide simple normals for the candidate procedural mesh section.
                TArray<FVector> Normals;

                // Preview normals do not need to be exact because color is the important feedback.
                Normals.Init(FVector::UpVector, CandidateVertices.Num());

                // Provide required UVs for the candidate section.
                TArray<FVector2D> UV0;

                // Use zero UVs because debug vertex colors drive the material.
                UV0.Init(FVector2D::ZeroVector, CandidateVertices.Num());

                // Allocate one color per vertex for the candidate face.
                TArray<FColor> VertexColors;

                // Green marks a valid merge candidate; red marks a bad topology candidate.
                VertexColors.Init(bCandidateValid ? FColor::Green : FColor::Red, CandidateVertices.Num());

                // Provide required tangents for the candidate section.
                TArray<FProcMeshTangent> Tangents;

                // Use a stable default tangent for preview geometry.
                Tangents.Init(FProcMeshTangent(1.0f, 0.0f, 0.0f), CandidateVertices.Num());

                // Reuse section 0 so either new-vertex preview or merge-preview owns the candidate surface.
                PreviewComponent->CreateMeshSection(0, CandidateVertices, CandidateTriangles, Normals, UV0, VertexColors, Tangents, false);
            }
        }
    }

    TArray<FVector> HelperVertices;
    TArray<int32> HelperTriangles;
    TArray<FColor> HelperColors;

    if (Vertices.Num() == 0 && !bHasPreviewVertex)
    {
        AppendPreviewBox(HelperVertices, HelperTriangles, HelperColors, FVector::ZeroVector, VertexExtent, TopologyColor);
    }

    for (int32 VertexIndex = 0; VertexIndex < Vertices.Num(); ++VertexIndex)
    {
        const bool bHighlighted = VertexIndex == HighlightedVertexIndex;
        const bool bConnectedSource = VertexIndex == ConnectedVertexSourceIndex;
        const bool bSelected = bHighlighted || bConnectedSource;
        const float Scale = bSelected ? SelectedVertexPreviewScale : 1.0f;
        const FColor VertexColor = bHighlighted ? SelectedHelperColor : (bConnectedSource ? ConnectedSourceHelperColor : NormalHelperColor);
        AppendPreviewBox(HelperVertices, HelperTriangles, HelperColors, Vertices[VertexIndex], VertexExtent * Scale, VertexColor);
    }

    TSet<FIntPoint> Edges;
    for (int32 TriIndex = 0; TriIndex + 2 < Triangles.Num(); TriIndex += 3)
    {
        AppendPreviewEdgeIfValid(Edges, Triangles[TriIndex], Triangles[TriIndex + 1]);
        AppendPreviewEdgeIfValid(Edges, Triangles[TriIndex + 1], Triangles[TriIndex + 2]);
        AppendPreviewEdgeIfValid(Edges, Triangles[TriIndex + 2], Triangles[TriIndex]);
    }

    // Add explicit active-polygon edges so unfinished n-gons show their intended outline.
    for (int32 Index = 0; Index + 1 < CurrentFace.Num(); ++Index)
    {
        // Consecutive vertices define the edge chain the user is currently drawing.
        AppendPreviewEdgeIfValid(Edges, CurrentFace[Index], CurrentFace[Index + 1]);
    }

    // Once a face has at least three corners, show the implicit closing edge from last vertex back to first.
    if (CurrentFace.Num() >= 3)
    {
        // This line makes pentagons/hexagons/etc. read as one continuous polygon instead of a broken strip.
        AppendPreviewEdgeIfValid(Edges, CurrentFace.Last(), CurrentFace[0]);
    }

    // If the user is hovering another existing vertex during connected creation, preview the merge edge before the click.
    if (CurrentFace.Num() > 0 && Vertices.IsValidIndex(HighlightedVertexIndex) && HighlightedVertexIndex != CurrentFace.Last() && !bHighlightedVertexMoving)
    {
        // This visual edge makes it clear that clicking the highlighted vertex will merge/connect to that existing vertex.
        AppendPreviewEdgeIfValid(Edges, CurrentFace.Last(), HighlightedVertexIndex);
    }

    // Convert every collected logical edge into a small rectangular prism helper mesh.
    for (const FIntPoint& Edge : Edges)
    {
        AppendPreviewSegment(HelperVertices, HelperTriangles, HelperColors, Vertices[Edge.X], Vertices[Edge.Y], SafeThickness, NormalHelperColor);
    }

    if (bHasPreviewVertex)
    {
        AppendPreviewBox(HelperVertices, HelperTriangles, HelperColors, PreviewVertex, VertexExtent, TopologyColor);
        int32 EdgeStartIndex = INDEX_NONE;
        if (CurrentFace.Num() > 0 && Vertices.IsValidIndex(CurrentFace.Last()))
        {
            EdgeStartIndex = CurrentFace.Last();
        }
        else if (Vertices.IsValidIndex(ConnectedVertexSourceIndex))
        {
            EdgeStartIndex = ConnectedVertexSourceIndex;
        }
        else if (Vertices.Num() > 0)
        {
            EdgeStartIndex = Vertices.Num() - 1;
        }

        if (Vertices.IsValidIndex(EdgeStartIndex))
        {
            AppendPreviewSegment(HelperVertices, HelperTriangles, HelperColors, Vertices[EdgeStartIndex], PreviewVertex, SafeThickness, TopologyColor);
        }
    }

    if (HelperVertices.Num() > 0 && HelperTriangles.Num() >= 3)
    {
        TArray<FVector> Normals;
        Normals.Init(FVector::UpVector, HelperVertices.Num());
        TArray<FVector2D> UV0;
        UV0.Init(FVector2D::ZeroVector, HelperVertices.Num());
        TArray<FProcMeshTangent> Tangents;
        Tangents.Init(FProcMeshTangent(1.0f, 0.0f, 0.0f), HelperVertices.Num());
        PreviewComponent->CreateMeshSection(1, HelperVertices, HelperTriangles, Normals, UV0, HelperColors, Tangents, false);
    }

    ApplyRuntimeMaterials();
}

FVector ARuntimeEditableMeshActor::GetLastVertexWorld() const
{
    if (Vertices.Num() == 0)
    {
        return GetActorLocation();
    }
    return GetActorTransform().TransformPosition(Vertices.Last());
}

FVector ARuntimeEditableMeshActor::GetPreviewVertexWorld() const
{
    if (!bHasPreviewVertex)
    {
        return GetLastVertexWorld();
    }
    return GetActorTransform().TransformPosition(PreviewVertex);
}

bool ARuntimeEditableMeshActor::FindNearestVertexToRay(const FVector& RayStart, const FVector& RayDirection, float MaxDistance, int32& OutVertexIndex, FVector& OutVertexWorldLocation, float& OutDistance) const
{
    OutVertexIndex = INDEX_NONE;
    OutVertexWorldLocation = FVector::ZeroVector;
    OutDistance = TNumericLimits<float>::Max();

    const FVector SafeDirection = RayDirection.GetSafeNormal();
    if (SafeDirection.IsNearlyZero() || Vertices.Num() == 0)
    {
        return false;
    }

    const float MaxDistanceSq = FMath::Square(FMath::Max(1.0f, MaxDistance));
    for (int32 Index = 0; Index < Vertices.Num(); ++Index)
    {
        const FVector WorldVertex = GetActorTransform().TransformPosition(Vertices[Index]);
        const FVector ToVertex = WorldVertex - RayStart;
        const float AlongRay = FVector::DotProduct(ToVertex, SafeDirection);
        if (AlongRay < 0.0f)
        {
            continue;
        }

        const FVector ClosestPoint = RayStart + SafeDirection * AlongRay;
        const float DistSq = FVector::DistSquared(WorldVertex, ClosestPoint);
        if (DistSq <= MaxDistanceSq && DistSq < FMath::Square(OutDistance))
        {
            OutVertexIndex = Index;
            OutVertexWorldLocation = WorldVertex;
            OutDistance = FMath::Sqrt(DistSq);
        }
    }

    return OutVertexIndex != INDEX_NONE;
}

FRuntimeGeneratedMeshRecord ARuntimeEditableMeshActor::ToGeneratedMeshRecord() const
{
    FRuntimeGeneratedMeshRecord Record;
    Record.RuntimeName = RuntimeName;
    Record.BaseName = BaseName;
    Record.Transform = GetActorTransform();
    Record.Vertices = Vertices;
    Record.Triangles = Triangles;
    return Record;
}

void ARuntimeEditableMeshActor::LoadFromGeneratedMeshRecord(const FRuntimeGeneratedMeshRecord& Record)
{
    RuntimeName = Record.RuntimeName;
    BaseName = Record.BaseName.IsEmpty() ? Record.RuntimeName : Record.BaseName;
    if (BaseName.Contains(TEXT(";")))
    {
        FString IgnoredSuffix;
        BaseName.Split(TEXT(";"), &BaseName, &IgnoredSuffix, ESearchCase::CaseSensitive, ESearchDir::FromStart);
    }
    SetActorTransform(Record.Transform);
    Vertices = Record.Vertices;
    Triangles = Record.Triangles;
    CurrentFace.Empty();
    CurrentFaceTriangleStartIndex = Triangles.Num(); // Save-data triangles are already committed when the record is loaded.
    bHasPreviewVertex = false;
    PreviewVertex = FVector::ZeroVector;
    HighlightedVertexIndex = INDEX_NONE;
    bHighlightedVertexMoving = false;
    ConnectedVertexSourceIndex = INDEX_NONE;
    bFinalized = true;
    SetActorHiddenInGame(false);
    SetActorEnableCollision(true);
    RebuildMesh(true);
    RebuildPreviewMesh();
}
