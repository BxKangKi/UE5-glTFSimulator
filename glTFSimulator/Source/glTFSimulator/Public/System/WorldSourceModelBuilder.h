// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldSourceModelBuilder.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "System/WorldArchive.h"
#include "UObject/Object.h"
#include "WorldSourceModelBuilder.generated.h"

class UglTFRuntimeAsset;
class UWorldSourceModelBuilder;
class UMaterialInterface;
class UTexture2D;
class FglTFRuntimeParser;
struct FglTFRuntimeConfig;
struct FglTFRuntimeMeshLOD;

/** Native completion signal for one source GLB build. Always emitted on the game thread. */
DECLARE_MULTICAST_DELEGATE_TwoParams(
    FWorldSourceModelBuildFinished,
    UWorldSourceModelBuilder*,
    bool);

/**
 * One-shot build worker for a single authoring GLB.
 *
 * This object is the only gameplay-module type allowed to open a source GLB. It asks
 * glTFRuntime to decode the document once, copies the resulting render data into the stable
 * FGWorldBaked* structures, and releases the parser before publishing its result. It never
 * spawns an actor and it cannot be used by the runtime gwd:// resolver.
 *
 * The world builder owns this UObject through a UPROPERTY while work is active. Parser creation
 * happens on a worker thread; every UObject and glTFRuntime decode/finalization call happens on
 * the game thread under FglTFRuntimeSafety's process-wide gate.
 */
UCLASS(Transient)
class GLTFSIMULATOR_API UWorldSourceModelBuilder final : public UObject
{
    GENERATED_BODY()

public:
    /** Starts a new one-shot build. DefinitionJson must be the already validated JSON snapshot. */
    bool Start(const FModelDefinition& InDefinition, const FString& InDefinitionJson);

    /** Cancels queued work and releases the source parser at the next safe native boundary. */
    void Cancel();

    float GetProgress() const { return ProgressValue; }
    const FString& GetError() const { return FailureReason; }
    const FString& GetSourcePath() const { return Definition.GlbPath; }

    /** Moves the completed build payload out without duplicating vertex or texture arrays. */
    FGWorldBuildModel TakeBuildModel();

    FWorldSourceModelBuildFinished OnFinished;

    virtual void BeginDestroy() override;

private:
    UPROPERTY(Transient)
    TObjectPtr<UglTFRuntimeAsset> SourceAsset;

    FModelDefinition Definition;
    FString DefinitionJson;
    FGWorldBuildModel CompletedModel;
    FGWorldBakedModel BakedData;
    TMap<TWeakObjectPtr<UMaterialInterface>, int32> MaterialIds;
    TMap<TWeakObjectPtr<UTexture2D>, int32> TextureIds;
    TArray<FString> SourceMeshNames;
    /** Source mesh -> compact baked mesh index; INDEX_NONE means the payload is an ;INST-only alias. */
    TArray<int32> SourceToBakedMeshIndex;
    /** Per source node runtime mesh index. ;INST rows point at their canonical base/LOD payload. */
    TMap<int32, int32> NodeToBakedMeshIndex;
    TSet<int32> SourceMeshesToCapture;
    TMap<int32, FVector> SourceMeshSizes;
    TMap<int32, TSet<int32>> SkinMeshIndices;

    int64 SourceFileSize = -1;
    FDateTime SourceTimestamp;
    uint64 RequestGeneration = 0;
    int32 CurrentMeshIndex = 0;
    float ProgressValue = 0.0f;
    FString FailureReason;
    bool bRunning = false;
    bool bCancelled = false;
    bool bCompletionSent = false;

    void BeginParserLoad(uint64 Generation);
    void CompleteParserLoad(
        uint64 Generation,
        const TSharedPtr<FglTFRuntimeParser>& Parser,
        const FglTFRuntimeConfig& Config,
        const FString& Error);
    void BeginDecodedCapture();
    FTSTicker::FDelegateHandle NextMeshTicker;
    void CaptureNextMesh();
    void CaptureNextMeshUnderGate();
    void CaptureSkinsAndMetadata();
    bool CaptureMeshBounds(
        int32 MeshIndex,
        const FglTFRuntimeMeshLOD& RuntimeLOD,
        FString& OutError);
    bool BuildMetadata(FGWorldModelMetadata& OutMetadata, FString& OutError) const;
    void Complete(bool bSuccess, const FString& Error = FString());
    void ReleaseSourceAsset();
    void ScheduleNextMesh();
};
