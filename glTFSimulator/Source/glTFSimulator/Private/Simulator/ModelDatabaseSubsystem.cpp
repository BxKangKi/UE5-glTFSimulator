// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Simulator/ModelDatabaseSubsystem.h"

#include "Async/ParallelFor.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "System/BinaryDataStore.h"
#include "System/SafeFileIO.h"

namespace
{
    struct FDatabaseBuildResult
    {
        TMap<FGuid, FModelDefinition> Definitions;
        TMap<FGuid, FString> CachePaths;
        TMap<FString, FGuid> GlbToId;
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
    WorldRoot = FSafeFileIO::NormalizeFilePath(InWorldRoot);
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
                    UE_LOG(LogTemp, Error, TEXT("Model definition rejected and will not load. JSON=%s Reason=%s"),
                        *JsonPath, *Parsed.Error);
                    continue;
                }
                FModelDefinition& Definition = Parsed.Definition;
                if (Result.Definitions.Contains(Definition.Id))
                {
                    UE_LOG(LogTemp, Error, TEXT("Duplicate model JSON ID rejected. ID=%s JSON=%s"),
                        *Definition.Id.ToString(), *JsonPath);
                    continue;
                }

                const FString CachePath = MakeCachePath(RequestedRoot, Definition);
                const FString CacheKey = FSafeFileIO::NormalizeFilePath(CachePath).ToLower();
                if (const FString* ExistingJson = CacheOwnerByPath.Find(CacheKey))
                {
                    UE_LOG(LogTemp, Error,
                        TEXT("Duplicate JSON filename rejected because extensionless cache paths must be unique. First=%s Rejected=%s Cache=%s"),
                        **ExistingJson, *JsonPath, *CachePath);
                    continue;
                }
                CacheOwnerByPath.Add(CacheKey, JsonPath);
                FModelDatabaseEntry& Row = Result.Rows.AddDefaulted_GetRef();
                Row.UUID = Definition.Id;
                Row.Cache = RelativeToRoot(CachePath, RequestedRoot);
                Row.Json = RelativeToRoot(JsonPath, RequestedRoot);
                Result.CachePaths.Add(Definition.Id, CachePath);
                Result.GlbToId.Add(FSafeFileIO::NormalizeFilePath(GlbPath).ToLower(), Definition.Id);
                Result.Definitions.Add(Definition.Id, MoveTemp(Definition));
            }

            // The recursive JSON scan is authoritative. Rewriting db.dat also repairs an old UUID
            // row whose Json path no longer matches the definition carrying that ID.
            const FSafeFileWriteResult Saved = FBinaryDataStore::SaveModelDatabaseBlocking(DatabasePath, Result.Rows);
            if (!Saved.IsSuccess())
            {
                Result.Error = Result.Error.IsEmpty() ? Saved.Error : Result.Error + TEXT("; ") + Saved.Error;
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
                    StrongThis->GlbToId = MoveTemp(Result.GlbToId);
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
    GlbToId.Empty();
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

bool UModelDatabaseSubsystem::FindIdForGlb(const FString& GlbPath, FGuid& OutUUID) const
{
    const FGuid* Found = GlbToId.Find(FSafeFileIO::NormalizeFilePath(GlbPath).ToLower());
    if (!Found) return false;
    OutUUID = *Found;
    return true;
}

void UModelDatabaseSubsystem::GetDefinitions(TArray<FModelDefinition>& OutDefinitions) const
{
    Definitions.GenerateValueArray(OutDefinitions);
}
