// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file ModelDatabaseSubsystem.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Templates/Atomic.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Simulator/ModelDefinitionJson.h"
#include "System/WorldArchive.h"
#include "ModelDatabaseSubsystem.generated.h"

DECLARE_DELEGATE_TwoParams(FModelDatabaseReady, bool, const FString&);

/**
 * Immutable per-world model index.
 *
 * Runtime mode opens WorldName.gwd and reads only its bounded directory. Authoring files are
 * deliberately not inspected in that mode. Project authoring is an explicit, separate operation
 * that scans Projects/<Project>/resources only when the user chooses Build.
 */
UCLASS()
class GLTFSIMULATOR_API UModelDatabaseSubsystem final : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Deinitialize() override;

    /** Opens only the immutable built archive. Runtime never falls back to source GLBs. */
    void InitializeForWorld(
        const FString& InWorldRoot,
        FModelDatabaseReady Completion = FModelDatabaseReady());

    /** Explicit project-build mode. Scans Projects/<Project>/resources and never opens a runtime .gwd. */
    void InitializeForAuthoringProject(
        const FString& InProjectRoot,
        FModelDatabaseReady Completion = FModelDatabaseReady());
    void Stop();

    /** Resolves a built UUID to its canonical gwd:// reference. Source mode always returns false. */
    bool Resolve(
        const FGuid& UUID,
        FModelDefinition& OutDefinition,
        FString& OutRuntimeReference) const;
    bool ResolveLoadable(
        const FGuid& UUID,
        FModelDefinition& OutDefinition,
        FString& OutRuntimeReference) const;

    /** Accepts only a gwd://UUID reference belonging to the open archive. */
    bool FindUUIDForReference(const FString& ModelReference, FGuid& OutUUID) const;
    void GetDefinitions(TArray<FModelDefinition>& OutDefinitions) const;

    /** Always-resident coarse index row; it never contains node or mesh maps. */
    bool GetSummary(const FGuid& UUID, FGWorldModelSummary& OutSummary) const;

    /** On-demand immutable members; repeated definition reads use a bounded runtime LRU. */
    bool GetMetadata(
        const FGuid& UUID,
        FGWorldModelMetadata& OutMetadata,
        FString* OutError = nullptr) const;
    bool GetDefinitionJson(
        const FGuid& UUID,
        FString& OutJson,
        FString* OutError = nullptr) const;
    bool LoadDefinitionDetails(
        const FGuid& UUID,
        FModelDefinition& InOutDefinition,
        FString& OutJson,
        FString& OutError) const;

    /** Thread-safe reader captured by workers that need exact decoded .dat ranges. */
    TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> GetArchiveReader() const
    {
        return ArchiveReader;
    }

    /** Returns the archive that physically owns this UUID (base .gwd or an allowed external .gasset). */
    TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> GetArchiveReaderForModel(const FGuid& UUID) const;

    bool IsReady() const { return bReady; }
    bool IsBuiltWorld() const { return bReady && ArchiveReader.IsValid(); }
    const FString& GetWorldRoot() const { return WorldRoot; }

private:
    struct FCachedDefinitionDetails
    {
        TMap<FString, FString> Bones;
        FString Json;
        uint64 LastAccess = 0;
        int64 EstimatedBytes = 0;
    };

    static constexpr int32 MaxCachedDefinitionCount = 64;
    static constexpr int64 MaxCachedDefinitionBytes = 64ll * 1024ll * 1024ll;

    FString WorldRoot;
    TMap<FGuid, FModelDefinition> Definitions;
    TMap<FGuid, FGWorldModelSummary> Summaries;
    /** Populated only while authoring sources are being baked; built JSON remains on disk. */
    TMap<FGuid, FString> SourceDefinitionJson;
    TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> ArchiveReader;
    /** Per-model ownership keeps external packs independent from the base world archive. */
    TMap<FGuid, TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe>> ModelArchiveReaders;
    TArray<TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe>> ExternalArchiveReaders;
    /** Bounded runtime-only LRU; source mode already owns its temporary authoring snapshots. */
    mutable TMap<FGuid, FCachedDefinitionDetails> DefinitionDetailsCache;
    mutable int64 CachedDefinitionBytes = 0;
    mutable uint64 DefinitionAccessSerial = 0;
    TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> ScanCancellation;
    uint64 Generation = 0;
    bool bReady = false;

    bool TryGetCachedDefinitionDetails(
        const FGuid& UUID,
        TMap<FString, FString>& OutBones,
        FString& OutJson) const;
    void CacheDefinitionDetails(
        const FGuid& UUID,
        const TMap<FString, FString>& Bones,
        const FString& Json) const;
};
