// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Immutable model-definition index and db.dat owner.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Simulator/ModelDefinitionJson.h"
#include "ModelDatabaseSubsystem.generated.h"

DECLARE_DELEGATE_TwoParams(FModelDatabaseReady, bool, const FString&);

/**
 * Recursively indexes WorldName/model on a worker, creates missing JSON templates, validates every
 * definition, and publishes an immutable UUID lookup on the game thread. Invalid JSON and duplicate
 * IDs are logged and excluded. ModelType=None remains indexed but is never returned as loadable.
 */
UCLASS()
class GLTFSIMULATOR_API UModelDatabaseSubsystem final : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Deinitialize() override;
    void InitializeForWorld(const FString& InWorldRoot, FModelDatabaseReady Completion = FModelDatabaseReady());
    void Stop();

    bool Resolve(const FGuid& UUID, FModelDefinition& OutDefinition, FString& OutCachePath) const;
    bool ResolveLoadable(const FGuid& UUID, FModelDefinition& OutDefinition, FString& OutCachePath) const;
    bool FindUUIDForGlb(const FString& GlbPath, FGuid& OutUUID) const;
    /** Resolves one exact, unique prefab Name used by <PrefabName>;INST placement nodes. */
    bool FindPrefabUUIDByName(const FString& PrefabName, FGuid& OutUUID, FString& OutError) const;
    void GetDefinitions(TArray<FModelDefinition>& OutDefinitions) const;
    bool IsReady() const { return bReady; }

private:
    FString WorldRoot;
    TMap<FGuid, FModelDefinition> Definitions;
    TMap<FGuid, FString> CachePaths;
    TMap<FString, FGuid> GlbToUUID;
    uint64 Generation = 0;
    bool bReady = false;
};
