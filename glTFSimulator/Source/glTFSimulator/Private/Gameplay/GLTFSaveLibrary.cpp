// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Gameplay/GLTFSaveLibrary.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace GLTFSaveInternal
{
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
    Root->SetNumberField(TEXT("Version"), 2);
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

    GLTFSaveInternal::EnsureDirectoryForFile(ManifestPath);

    FString ManifestString;
    const TSharedRef<FJsonObject> Manifest = BuildManifestJson(PlacedObjects, GeneratedMeshes);
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ManifestString);
    if (!FJsonSerializer::Serialize(Manifest, Writer))
    {
        return false;
    }

    if (!FFileHelper::SaveStringToFile(ManifestString, *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
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

    FString JsonString;
    if (!FFileHelper::LoadFileToString(JsonString, *ManifestPath))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Objects = nullptr;
    if (Root->TryGetArrayField(TEXT("Objects"), Objects) && Objects)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Objects)
        {
            if (Value.IsValid() && Value->Type == EJson::Object)
            {
                FPlacedObjectRecord Record;
                if (Record.FromJson(Value->AsObject()))
                {
                    OutPlacedObjects.Add(Record);
                }
            }
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* Meshes = nullptr;
    if (Root->TryGetArrayField(TEXT("GeneratedMeshes"), Meshes) && Meshes)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Meshes)
        {
            if (Value.IsValid() && Value->Type == EJson::Object)
            {
                FGeneratedMeshRecord Record;
                if (Record.FromJson(Value->AsObject()))
                {
                    OutGeneratedMeshes.Add(Record);
                }
            }
        }
    }

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
