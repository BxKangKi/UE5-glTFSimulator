// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Model/glTFSaveLibrary.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "System/MacroLibrary.h"
#include "System/SafeFileIO.h"

namespace GLTFSaveInternal
{
    // The manifest is external, user-editable data. Keep the JSON and generated-mesh budgets low
    // enough that validation cannot drive TArray, JSON DOM, renderer, or Chaos into huge allocations.
    constexpr int64 MaxSceneManifestBytes = 64ll * 1024ll * 1024ll;
    constexpr int32 MaxScenePlacedObjects = 100000;
    constexpr int32 MaxSceneGeneratedMeshes = 4096;
    constexpr int64 MaxSceneTotalVertices = 500000;
    constexpr int64 MaxSceneTotalTriangleIndices = 1500000;
    constexpr int32 MaxRecordNameCharacters = 1024;
    constexpr int32 MaxSourcePathCharacters = 32768;

    static FSafeJsonLimits MakeSceneJsonLimits()
    {
        FSafeJsonLimits Limits;
        Limits.MaxFileBytes = MaxSceneManifestBytes;
        Limits.MaxDepth = 32;
        Limits.MaxValues = 5000000;
        Limits.MaxContainerEntries = PlacementSafetyLimits::MaxGeneratedMeshTriangleIndices;
        Limits.MaxStringCharacters = MaxSourcePathCharacters;
        Limits.MaxPrimitiveCharacters = 128;
        Limits.bAllowBackupRecovery = true;
        return Limits;
    }

    static bool IsFiniteBoundedNumber(const double Value)
    {
        return FMath::IsFinite(Value) &&
            FMath::Abs(Value) <= static_cast<double>(WORLD_MAX_SIZE);
    }

    static bool IsFiniteBoundedVector(const FVector& Value)
    {
        return IsFiniteBoundedNumber(Value.X) &&
            IsFiniteBoundedNumber(Value.Y) &&
            IsFiniteBoundedNumber(Value.Z);
    }

    static bool IsFiniteTransform(const FTransform& Transform)
    {
        const FQuat Rotation = Transform.GetRotation();
        return IsFiniteBoundedVector(Transform.GetLocation()) &&
            IsFiniteBoundedVector(Transform.GetScale3D()) &&
            FMath::IsFinite(Rotation.X) &&
            FMath::IsFinite(Rotation.Y) &&
            FMath::IsFinite(Rotation.Z) &&
            FMath::IsFinite(Rotation.W) &&
            Rotation.IsNormalized();
    }

    static bool IsPlacedObjectRecordSafe(const FPlacedObjectRecord& Record)
    {
        return !Record.ObjectName.IsEmpty() &&
            Record.ObjectName.Len() <= MaxRecordNameCharacters &&
            Record.BaseName.Len() <= MaxRecordNameCharacters &&
            Record.SourceFile.Len() <= MaxSourcePathCharacters &&
            IsFiniteTransform(Record.Transform);
    }

    static bool IsGeneratedMeshRecordSafe(const FGeneratedMeshRecord& Record)
    {
        if (Record.ObjectName.IsEmpty() ||
            Record.ObjectName.Len() > MaxRecordNameCharacters ||
            Record.BaseName.Len() > MaxRecordNameCharacters ||
            !IsFiniteTransform(Record.Transform) ||
            Record.Vertices.Num() < 3 ||
            Record.Vertices.Num() > PlacementSafetyLimits::MaxGeneratedMeshVertices ||
            Record.Triangles.Num() < 3 ||
            Record.Triangles.Num() > PlacementSafetyLimits::MaxGeneratedMeshTriangleIndices ||
            (Record.Triangles.Num() % 3) != 0)
        {
            return false;
        }

        for (const FVector& Vertex : Record.Vertices)
        {
            if (!IsFiniteBoundedVector(Vertex))
            {
                return false;
            }
        }

        for (int32 TriangleOffset = 0;
            TriangleOffset < Record.Triangles.Num();
            TriangleOffset += 3)
        {
            const int32 A = Record.Triangles[TriangleOffset];
            const int32 B = Record.Triangles[TriangleOffset + 1];
            const int32 C = Record.Triangles[TriangleOffset + 2];
            if (!Record.Vertices.IsValidIndex(A) ||
                !Record.Vertices.IsValidIndex(B) ||
                !Record.Vertices.IsValidIndex(C) ||
                A == B || B == C || C == A)
            {
                return false;
            }

            const FVector Cross = FVector::CrossProduct(
                Record.Vertices[B] - Record.Vertices[A],
                Record.Vertices[C] - Record.Vertices[A]);
            const double CrossSizeSquared = Cross.SizeSquared();
            if (!FMath::IsFinite(CrossSizeSquared) || CrossSizeSquared <= 1.0)
            {
                return false;
            }
        }
        return true;
    }

    static bool ValidateSceneRecords(
        const TArray<FPlacedObjectRecord>& PlacedObjects,
        const TArray<FGeneratedMeshRecord>& GeneratedMeshes,
        FString& OutReason)
    {
        if (PlacedObjects.Num() > MaxScenePlacedObjects)
        {
            OutReason = FString::Printf(
                TEXT("placed-object count exceeds the scene limit (%d > %d)"),
                PlacedObjects.Num(),
                MaxScenePlacedObjects);
            return false;
        }
        if (GeneratedMeshes.Num() > MaxSceneGeneratedMeshes)
        {
            OutReason = FString::Printf(
                TEXT("generated-mesh count exceeds the scene limit (%d > %d)"),
                GeneratedMeshes.Num(),
                MaxSceneGeneratedMeshes);
            return false;
        }

        for (const FPlacedObjectRecord& Record : PlacedObjects)
        {
            if (!IsPlacedObjectRecordSafe(Record))
            {
                OutReason = FString::Printf(
                    TEXT("placed-object record is invalid: %s"),
                    *Record.ObjectName.Left(128));
                return false;
            }
        }

        int64 TotalVertices = 0;
        int64 TotalTriangleIndices = 0;
        for (const FGeneratedMeshRecord& Record : GeneratedMeshes)
        {
            if (!IsGeneratedMeshRecordSafe(Record))
            {
                OutReason = FString::Printf(
                    TEXT("generated-mesh record is invalid: %s"),
                    *Record.ObjectName.Left(128));
                return false;
            }

            TotalVertices += Record.Vertices.Num();
            TotalTriangleIndices += Record.Triangles.Num();
            if (TotalVertices > MaxSceneTotalVertices ||
                TotalTriangleIndices > MaxSceneTotalTriangleIndices)
            {
                OutReason = FString::Printf(
                    TEXT("generated-mesh totals exceed scene limits (vertices=%lld indices=%lld)"),
                    TotalVertices,
                    TotalTriangleIndices);
                return false;
            }
        }

        OutReason.Reset();
        return true;
    }

    static void EnsureDirectoryForFile(const FString& FilePath)
    {
        if (FilePath.IsEmpty())
        {
            return;
        }

        const FString Directory = FPaths::GetPath(FilePath);
        if (!Directory.IsEmpty())
        {
            IFileManager::Get().MakeDirectory(*Directory, true);
        }
    }

    static void DeleteFileIfPresent(const FString& FilePath)
    {
        if (!FilePath.IsEmpty() && IFileManager::Get().FileExists(*FilePath))
        {
            IFileManager::Get().Delete(*FilePath, false, true);
        }
    }

    static bool IsEntityGltfSidecarName(const FString& FilePath)
    {
        const FString CleanFilename = FPaths::GetCleanFilename(FilePath);
        return CleanFilename.Equals(TEXT("entities.glb"), ESearchCase::IgnoreCase)
            || CleanFilename.Equals(TEXT("entities.gltf"), ESearchCase::IgnoreCase)
            || CleanFilename.Equals(TEXT("runtime_installed.glb"), ESearchCase::IgnoreCase)
            || CleanFilename.Equals(TEXT("runtime_installed.gltf"), ESearchCase::IgnoreCase);
    }

    static void DeleteEntityGltfSidecars(const FString& ManifestPath, const FString& ExplicitGltfPath)
    {
        TArray<FString> Candidates;
        if (!ExplicitGltfPath.IsEmpty() && IsEntityGltfSidecarName(ExplicitGltfPath))
        {
            Candidates.AddUnique(ExplicitGltfPath);
        }

        if (!ManifestPath.IsEmpty())
        {
            const FString ManifestDirectory = FPaths::GetPath(ManifestPath);
            Candidates.AddUnique(FPaths::ChangeExtension(ManifestPath, TEXT("glb")));
            Candidates.AddUnique(FPaths::ChangeExtension(ManifestPath, TEXT("gltf")));
            Candidates.AddUnique(FPaths::Combine(ManifestDirectory, TEXT("entities.glb")));
            Candidates.AddUnique(FPaths::Combine(ManifestDirectory, TEXT("entities.gltf")));
            Candidates.AddUnique(FPaths::Combine(ManifestDirectory, TEXT("runtime_installed.glb")));
            Candidates.AddUnique(FPaths::Combine(ManifestDirectory, TEXT("runtime_installed.gltf")));
        }

        for (const FString& Candidate : Candidates)
        {
            DeleteFileIfPresent(Candidate);
        }
    }
}

TSharedRef<FJsonObject> UGLTFSaveLibrary::BuildManifestJson(
    const TArray<FPlacedObjectRecord>& PlacedObjects,
    const TArray<FGeneratedMeshRecord>& GeneratedMeshes)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(JSON_VERSION_FIELD, JSON_SCHEMA_VERSION);
    Root->SetNumberField(TEXT("ManifestFormat"), 2);
    Root->SetStringField(TEXT("StorageMode"), TEXT("JsonSourceReferenceOnly"));
    Root->SetBoolField(TEXT("WritesEntitiesGlb"), false);
    Root->SetStringField(TEXT("PlacedObjectMeshStorage"), TEXT("OriginalGlbAndNodeReference"));
    Root->SetStringField(TEXT("GeneratedMeshStorage"), TEXT("JsonProceduralMesh"));
    Root->SetStringField(TEXT("SavedAt"), FDateTime::Now().ToIso8601());

    TArray<TSharedPtr<FJsonValue>> Objects;
    Objects.Reserve(PlacedObjects.Num());
    for (const FPlacedObjectRecord& Object : PlacedObjects)
    {
        Objects.Add(MakeShared<FJsonValueObject>(Object.ToJson()));
    }
    Root->SetArrayField(TEXT("Objects"), Objects);

    TSharedRef<FJsonObject> EntitiesByFullName = MakeShared<FJsonObject>();
    for (const FPlacedObjectRecord& Object : PlacedObjects)
    {
        if (!Object.ObjectName.IsEmpty())
        {
            EntitiesByFullName->SetObjectField(Object.ObjectName, Object.ToJson());
        }
    }
    Root->SetObjectField(TEXT("Entities"), EntitiesByFullName);

    TArray<TSharedPtr<FJsonValue>> Meshes;
    Meshes.Reserve(GeneratedMeshes.Num());
    for (const FGeneratedMeshRecord& Mesh : GeneratedMeshes)
    {
        Meshes.Add(MakeShared<FJsonValueObject>(Mesh.ToJson()));
    }
    Root->SetArrayField(TEXT("GeneratedMeshes"), Meshes);
    return Root;
}

bool UGLTFSaveLibrary::SaveScene(
    UObject* WorldContextObject,
    const TArray<FPlacedObjectRecord>& PlacedObjects,
    const TArray<FGeneratedMeshRecord>& GeneratedMeshes,
    const FString& ManifestPath,
    const FString& GltfPath)
{
    (void)WorldContextObject;

    FString ValidationReason;
    if (!GLTFSaveInternal::ValidateSceneRecords(
            PlacedObjects, GeneratedMeshes, ValidationReason))
    {
        UE_LOG(
            LogTemp,
            Warning,
            TEXT("GLTFSaveLibrary refused an unsafe scene save. Path=%s Reason=%s"),
            *ManifestPath,
            *ValidationReason);
        return false;
    }

    GLTFSaveInternal::EnsureDirectoryForFile(ManifestPath);

    const TSharedRef<FJsonObject> Manifest = BuildManifestJson(PlacedObjects, GeneratedMeshes);
    const FSafeFileWriteResult SaveResult = FSafeFileIO::SaveJsonBlocking(
        Manifest,
        ManifestPath,
        GLTFSaveInternal::MaxSceneManifestBytes);
    if (!SaveResult.IsSuccess())
    {
        UE_LOG(
            LogTemp,
            Warning,
            TEXT("GLTFSaveLibrary scene save failed. Path=%s Reason=%s"),
            *ManifestPath,
            *SaveResult.Error);
        return false;
    }

    // Persistence is JSON-only. Remove stale runtime GLTF/GLB sidecars so future scans cannot accidentally
    // load an old mesh snapshot as if it were a normal source asset.
    GLTFSaveInternal::DeleteEntityGltfSidecars(ManifestPath, GltfPath);
    return true;
}

bool UGLTFSaveLibrary::LoadScene(
    const FString& ManifestPath,
    TArray<FPlacedObjectRecord>& OutPlacedObjects,
    TArray<FGeneratedMeshRecord>& OutGeneratedMeshes)
{
    OutPlacedObjects.Empty();
    OutGeneratedMeshes.Empty();

    const FSafeJsonLoadResult LoadResult = FSafeFileIO::LoadJsonBlocking(
        ManifestPath,
        GLTFSaveInternal::MakeSceneJsonLimits());
    if (!LoadResult.IsSuccess())
    {
        // Missing manifests are an expected first-run condition; malformed existing files are not.
        if (LoadResult.Status != ESafeFileIOStatus::Missing)
        {
            UE_LOG(
                LogTemp,
                Warning,
                TEXT("GLTFSaveLibrary scene load failed. Path=%s Reason=%s"),
                *ManifestPath,
                *LoadResult.Error);
        }
        return false;
    }

    const TSharedPtr<FJsonObject>& Root = LoadResult.JsonObject;
    if (!Root.IsValid())
    {
        return false;
    }

    TArray<FPlacedObjectRecord> ParsedPlacedObjects;
    TArray<FGeneratedMeshRecord> ParsedGeneratedMeshes;

    const TArray<TSharedPtr<FJsonValue>>* Objects = nullptr;
    if (Root->HasField(TEXT("Objects")))
    {
        if (!Root->TryGetArrayField(TEXT("Objects"), Objects) ||
            !Objects ||
            Objects->Num() > GLTFSaveInternal::MaxScenePlacedObjects)
        {
            UE_LOG(
                LogTemp,
                Warning,
                TEXT("GLTFSaveLibrary rejected an invalid Objects array. Path=%s"),
                *ManifestPath);
            return false;
        }

        ParsedPlacedObjects.Reserve(Objects->Num());
        for (const TSharedPtr<FJsonValue>& Value : *Objects)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object)
            {
                UE_LOG(
                    LogTemp,
                    Warning,
                    TEXT("GLTFSaveLibrary rejected a non-object entry in Objects. Path=%s"),
                    *ManifestPath);
                return false;
            }

            FPlacedObjectRecord Record;
            if (!Record.FromJson(Value->AsObject()) ||
                !GLTFSaveInternal::IsPlacedObjectRecordSafe(Record))
            {
                UE_LOG(
                    LogTemp,
                    Warning,
                    TEXT("GLTFSaveLibrary rejected an invalid placed-object record. Path=%s"),
                    *ManifestPath);
                return false;
            }
            ParsedPlacedObjects.Add(MoveTemp(Record));
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* Meshes = nullptr;
    if (Root->HasField(TEXT("GeneratedMeshes")))
    {
        if (!Root->TryGetArrayField(TEXT("GeneratedMeshes"), Meshes) ||
            !Meshes ||
            Meshes->Num() > GLTFSaveInternal::MaxSceneGeneratedMeshes)
        {
            UE_LOG(
                LogTemp,
                Warning,
                TEXT("GLTFSaveLibrary rejected an invalid GeneratedMeshes array. Path=%s"),
                *ManifestPath);
            return false;
        }

        ParsedGeneratedMeshes.Reserve(Meshes->Num());
        int64 TotalVertices = 0;
        int64 TotalTriangleIndices = 0;
        for (const TSharedPtr<FJsonValue>& Value : *Meshes)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object)
            {
                UE_LOG(
                    LogTemp,
                    Warning,
                    TEXT("GLTFSaveLibrary rejected a non-object entry in GeneratedMeshes. Path=%s"),
                    *ManifestPath);
                return false;
            }

            FGeneratedMeshRecord Record;
            if (!Record.FromJson(Value->AsObject()) ||
                !GLTFSaveInternal::IsGeneratedMeshRecordSafe(Record))
            {
                UE_LOG(
                    LogTemp,
                    Warning,
                    TEXT("GLTFSaveLibrary rejected an invalid generated-mesh record. Path=%s"),
                    *ManifestPath);
                return false;
            }

            TotalVertices += Record.Vertices.Num();
            TotalTriangleIndices += Record.Triangles.Num();
            if (TotalVertices > GLTFSaveInternal::MaxSceneTotalVertices ||
                TotalTriangleIndices > GLTFSaveInternal::MaxSceneTotalTriangleIndices)
            {
                UE_LOG(
                    LogTemp,
                    Warning,
                    TEXT("GLTFSaveLibrary rejected generated-mesh totals above scene limits. Path=%s Vertices=%lld Indices=%lld"),
                    *ManifestPath,
                    TotalVertices,
                    TotalTriangleIndices);
                return false;
            }
            ParsedGeneratedMeshes.Add(MoveTemp(Record));
        }
    }

    OutPlacedObjects = MoveTemp(ParsedPlacedObjects);
    OutGeneratedMeshes = MoveTemp(ParsedGeneratedMeshes);
    return true;
}

bool UGLTFSaveLibrary::ExportSceneAsGltf(
    const TArray<FPlacedObjectRecord>& PlacedObjects,
    const TArray<FGeneratedMeshRecord>& GeneratedMeshes,
    const FString& GltfPath)
{
    (void)PlacedObjects;
    (void)GeneratedMeshes;

    if (GLTFSaveInternal::IsEntityGltfSidecarName(GltfPath))
    {
        GLTFSaveInternal::DeleteFileIfPresent(GltfPath);
    }
    UE_LOG(LogTemp, Warning, TEXT("GLTFSaveLibrary: runtime GLTF/GLB export is disabled; entities are saved in entities.json as source GLB/node references plus fallbacks."));
    return false;
}
