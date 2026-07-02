// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Gameplay/PlacementTypes.h"
#include "GLTFSaveLibrary.generated.h"

UCLASS()
class GLTFSIMULATOR_API UGLTFSaveLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category="glTF")
    static bool SaveScene(
        UObject* WorldContextObject,
        const TArray<FPlacedObjectRecord>& PlacedObjects,
        const TArray<FGeneratedMeshRecord>& GeneratedMeshes,
        const FString& ManifestPath,
        const FString& GltfPath);

    UFUNCTION(BlueprintCallable, Category="glTF")
    static bool LoadScene(
        const FString& ManifestPath,
        TArray<FPlacedObjectRecord>& OutPlacedObjects,
        TArray<FGeneratedMeshRecord>& OutGeneratedMeshes);

    UFUNCTION(BlueprintCallable, Category="glTF")
    static bool ExportSceneAsGltf(
        const TArray<FPlacedObjectRecord>& PlacedObjects,
        const TArray<FGeneratedMeshRecord>& GeneratedMeshes,
        const FString& GltfPath);

private:
    static TSharedRef<FJsonObject> BuildManifestJson(
        const TArray<FPlacedObjectRecord>& PlacedObjects,
        const TArray<FGeneratedMeshRecord>& GeneratedMeshes);
};
