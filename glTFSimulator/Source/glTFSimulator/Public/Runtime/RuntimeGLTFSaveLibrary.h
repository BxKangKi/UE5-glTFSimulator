// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Runtime/RuntimePlacementTypes.h"
#include "RuntimeGLTFSaveLibrary.generated.h"

UCLASS()
class GLTFSIMULATOR_API URuntimeGLTFSaveLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category="Runtime glTF")
    static bool SaveRuntimeScene(
        UObject* WorldContextObject,
        const TArray<FRuntimePlacedObjectRecord>& PlacedObjects,
        const TArray<FRuntimeGeneratedMeshRecord>& GeneratedMeshes,
        const FString& ManifestPath,
        const FString& GltfPath);

    UFUNCTION(BlueprintCallable, Category="Runtime glTF")
    static bool LoadRuntimeScene(
        const FString& ManifestPath,
        TArray<FRuntimePlacedObjectRecord>& OutPlacedObjects,
        TArray<FRuntimeGeneratedMeshRecord>& OutGeneratedMeshes);

    UFUNCTION(BlueprintCallable, Category="Runtime glTF")
    static bool ExportRuntimeSceneAsGltf(
        const TArray<FRuntimePlacedObjectRecord>& PlacedObjects,
        const TArray<FRuntimeGeneratedMeshRecord>& GeneratedMeshes,
        const FString& GltfPath);

private:
    static TSharedRef<FJsonObject> BuildManifestJson(
        const TArray<FRuntimePlacedObjectRecord>& PlacedObjects,
        const TArray<FRuntimeGeneratedMeshRecord>& GeneratedMeshes);
};
