// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Simulator/ModelDatabaseSubsystem.h"

#include "Async/ParallelFor.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "System/BinaryDataStore.h"
#include "System/FileFunctionLibrary.h"
#include "System/SafeFileIO.h"

namespace
{
    struct FDatabaseBuildResult
    {
        TMap<FGuid, FModelDefinition> Definitions;
        TMap<FGuid, FString> CachePaths;
        TMap<FString, FGuid> GlbToUUID;
        TArray<FModelDatabaseEntry> Rows;
        FString Error;
    };

    struct FParsedDefinitionResult
    {
        FModelDefinition Definition;
        FString Error;
        bool bValid = false;
    };

    FString RelativeToRoot(const FString& AbsolutePath, const FString& Root)
    {
        FString Result = AbsolutePath;
        FString Base = Root;
        if (!Base.EndsWith(TEXT("/"))) Base += TEXT("/");
        FPaths::MakePathRelativeTo(Result, *Base);
        FPaths::NormalizeFilename(Result);
        return Result;
    }

    FString MakeCachePath(const FString& WorldRoot, const FModelDefinition& Definition)
    {
        // Cache entries are individual extensionless files directly below /cache. Their names
        // come from the author-owned JSON filename, not Name, ModelType, or a category folder.
        return FPaths::Combine(
            WorldRoot,
            TEXT("cache"),
            FPaths::GetBaseFilename(Definition.JsonPath));
    }
}

void UModelDatabaseSubsystem::Deinitialize()
{
    Stop();
    Super::Deinitialize();
}

void UModelDatabaseSubsystem::InitializeForWorld(const FString& InWorldRoot, FModelDatabaseReady Completion)
{
    check(IsInGameThread());
    Stop();
    if (InWorldRoot.TrimStartAndEnd().IsEmpty())
    {
        UE_LOG(LogTemp, Error,
            TEXT("Model database initialization aborted because the explicit world root is empty."));
        Completion.ExecuteIfBound(false, TEXT("explicit world root is empty"));
        return;
    }
    WorldRoot = FSafeFileIO::NormalizeFilePath(InWorldRoot);
    if (WorldRoot.IsEmpty())
    {
        UE_LOG(LogTemp, Error,
            TEXT("Model database initialization aborted because the explicit world root is invalid."));
        Completion.ExecuteIfBound(false, TEXT("explicit world root is invalid"));
        return;
    }
    const FString RequestedRoot = WorldRoot;
    const uint64 RequestedGeneration = ++Generation;
    TWeakObjectPtr<UModelDatabaseSubsystem> WeakThis(this);

    const bool bQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, RequestedRoot, RequestedGeneration, Completion]() mutable
        {
            FDatabaseBuildResult Result;
            const FString ModelRoot = FPaths::Combine(RequestedRoot, TEXT("model"));
            const FString DatabasePath = FPaths::Combine(RequestedRoot, TEXT("data"), TEXT("db.dat"));
            IFileManager::Get().MakeDirectory(*ModelRoot, true);
            IFileManager::Get().MakeDirectory(*FPaths::Combine(RequestedRoot, TEXT("cache")), true);

            if (!ModelDefinitionJson::EnsureMissingDefinitions(ModelRoot))
            {
                Result.Error = TEXT("one or more missing model JSON templates could not be created");
            }

            TArray<FString> GlbFiles;
            IFileManager::Get().FindFilesRecursive(GlbFiles, *ModelRoot, TEXT("*.glb"), true, false, false);
            GlbFiles.Sort();
            TArray<FParsedDefinitionResult> ParsedDefinitions;
            ParsedDefinitions.SetNum(GlbFiles.Num());
            // Files are independent and each worker owns one pre-sized result slot. Consolidation
            // below remains deterministic and single-threaded so duplicate IDs/cache names cannot race.
            ParallelFor(GlbFiles.Num(), [&GlbFiles, &ParsedDefinitions](const int32 Index)
            {
                const FString& GlbPath = GlbFiles[Index];
                FParsedDefinitionResult& Parsed = ParsedDefinitions[Index];
                Parsed.bValid = ModelDefinitionJson::LoadDefinition(
                    FPaths::ChangeExtension(GlbPath, TEXT("json")),
                    GlbPath,
                    Parsed.Definition,
                    Parsed.Error);
            });

            TMap<FString, FString> CacheOwnerByPath;
            for (int32 Index = 0; Index < GlbFiles.Num(); ++Index)
            {
                const FString& GlbPath = GlbFiles[Index];
                const FString JsonPath = FPaths::ChangeExtension(GlbPath, TEXT("json"));
                FParsedDefinitionResult& Parsed = ParsedDefinitions[Index];
                if (!Parsed.bValid)
                {
                    const FString Message = FString::Printf(
                        TEXT("Model definition rejected and will not load. JSON=%s Reason=%s"),
                        *JsonPath, *Parsed.Error);
                    UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
                    UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("ModelUUID"), Message);
                    continue;
                }
                FModelDefinition& Definition = Parsed.Definition;
                if (Result.Definitions.Contains(Definition.UUID))
                {
                    const FString Message = FString::Printf(
                        TEXT("Duplicate model JSON UUID rejected. UUID=%s JSON=%s"),
                        *Definition.UUID.ToString(), *JsonPath);
                    UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
                    UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("ModelUUID"), Message);
                    continue;
                }

                const FString CachePath = MakeCachePath(RequestedRoot, Definition);
                const FString CacheKey = FSafeFileIO::NormalizeFilePath(CachePath).ToLower();
                if (const FString* ExistingJson = CacheOwnerByPath.Find(CacheKey))
                {
                    const FString Message = FString::Printf(
                        TEXT("Duplicate JSON filename rejected because extensionless cache paths must be unique. First=%s Rejected=%s Cache=%s"),
                        **ExistingJson, *JsonPath, *CachePath);
                    UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
                    UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("ModelDatabase"), Message);
                    continue;
                }
                CacheOwnerByPath.Add(CacheKey, JsonPath);
                FModelDatabaseEntry& Row = Result.Rows.AddDefaulted_GetRef();
                Row.UUID = Definition.UUID;
                Row.Cache = RelativeToRoot(CachePath, RequestedRoot);
                Row.Json = RelativeToRoot(JsonPath, RequestedRoot);
                Result.CachePaths.Add(Definition.UUID, CachePath);
                Result.GlbToUUID.Add(FSafeFileIO::NormalizeFilePath(GlbPath).ToLower(), Definition.UUID);
                Result.Definitions.Add(Definition.UUID, MoveTemp(Definition));
            }

            // The recursive JSON scan is authoritative. Rewriting db.dat also repairs an old UUID
            // row whose Json path no longer matches the definition carrying that UUID.
            const FSafeFileWriteResult Saved = FBinaryDataStore::SaveModelDatabaseBlocking(DatabasePath, Result.Rows);
            if (!Saved.IsSuccess())
            {
                Result.Error = Result.Error.IsEmpty() ? Saved.Error : Result.Error + TEXT("; ") + Saved.Error;
                UFileFunctionLibrary::WriteSimulatorLogAsync(
                    TEXT("ModelDatabase"),
                    FString::Printf(TEXT("db.dat save failed. Path=%s Reason=%s"), *DatabasePath, *Saved.Error));
            }

            FSafeFileIO::DispatchTrackedGameThread(
                [WeakThis, RequestedRoot, RequestedGeneration, Completion, Result = MoveTemp(Result)]() mutable
                {
                    UModelDatabaseSubsystem* StrongThis = WeakThis.Get();
                    if (!IsValid(StrongThis) || StrongThis->Generation != RequestedGeneration
                        || StrongThis->WorldRoot != RequestedRoot)
                    {
                        return;
                    }
                    StrongThis->Definitions = MoveTemp(Result.Definitions);
                    StrongThis->CachePaths = MoveTemp(Result.CachePaths);
                    StrongThis->GlbToUUID = MoveTemp(Result.GlbToUUID);
                    StrongThis->bReady = Result.Error.IsEmpty();
                    Completion.ExecuteIfBound(StrongThis->bReady, Result.Error);
                });
        });

    if (!bQueued)
    {
        bReady = false;
        Completion.ExecuteIfBound(false, TEXT("model database worker queue is shutting down"));
    }
}

void UModelDatabaseSubsystem::Stop()
{
    check(IsInGameThread());
    ++Generation;
    bReady = false;
    Definitions.Empty();
    CachePaths.Empty();
    GlbToUUID.Empty();
    WorldRoot.Reset();
}

bool UModelDatabaseSubsystem::Resolve(const FGuid& UUID, FModelDefinition& OutDefinition, FString& OutCachePath) const
{
    const FModelDefinition* Found = Definitions.Find(UUID);
    const FString* Cache = CachePaths.Find(UUID);
    if (!Found || !Cache) return false;
    OutDefinition = *Found;
    OutCachePath = *Cache;
    return true;
}

bool UModelDatabaseSubsystem::ResolveLoadable(const FGuid& UUID, FModelDefinition& OutDefinition, FString& OutCachePath) const
{
    return Resolve(UUID, OutDefinition, OutCachePath) && OutDefinition.IsLoadable();
}

bool UModelDatabaseSubsystem::FindUUIDForGlb(const FString& GlbPath, FGuid& OutUUID) const
{
    const FGuid* Found = GlbToUUID.Find(FSafeFileIO::NormalizeFilePath(GlbPath).ToLower());
    if (!Found) return false;
    OutUUID = *Found;
    return true;
}

bool UModelDatabaseSubsystem::FindPrefabUUIDByName(
    const FString& PrefabName,
    FGuid& OutUUID,
    FString& OutError) const
{
    OutUUID = FGuid();
    OutError.Reset();
    if (PrefabName.IsEmpty())
    {
        OutError = TEXT("placement node has an empty prefab name");
        return false;
    }

    for (const TPair<FGuid, FModelDefinition>& Pair : Definitions)
    {
        const FModelDefinition& Definition = Pair.Value;
        if (Definition.ModelType != EModelDefinitionType::Prefab ||
            !Definition.Name.Equals(PrefabName, ESearchCase::CaseSensitive))
        {
            continue;
        }
        if (OutUUID.IsValid())
        {
            OutUUID = FGuid();
            OutError = FString::Printf(TEXT("prefab name is ambiguous: %s"), *PrefabName);
            return false;
        }
        OutUUID = Definition.UUID;
    }

    if (!OutUUID.IsValid())
    {
        OutError = FString::Printf(TEXT("no prefab has the exact name '%s'"), *PrefabName);
        return false;
    }
    return true;
}

void UModelDatabaseSubsystem::GetDefinitions(TArray<FModelDefinition>& OutDefinitions) const
{
    Definitions.GenerateValueArray(OutDefinitions);
}
