// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file ModelDatabaseSubsystem.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Simulator/ModelDatabaseSubsystem.h"

#include "Async/ParallelFor.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "System/FileFunctionLibrary.h"
#include "System/GlbValidation.h"
#include "System/MacroLibrary.h"
#include "System/ProjectConfig.h"
#include "System/SafeFileIO.h"
#include "Simulator/SoundDefinitionJson.h"

namespace ModelDatabasePrivate
{
    /** Fixed-width GUID lexical order without allocating two temporary strings per comparison. */
    bool IsGuidLess(const FGuid& Left, const FGuid& Right)
    {
        if (Left.A != Right.A) return Left.A < Right.A;
        if (Left.B != Right.B) return Left.B < Right.B;
        if (Left.C != Right.C) return Left.C < Right.C;
        return Left.D < Right.D;
    }

    struct FDatabaseBuildResult
    {
        TMap<FGuid, FModelDefinition> Definitions;
        TMap<FGuid, FSoundDefinition> SoundDefinitions;
        TMap<FGuid, FGWorldModelSummary> Summaries;
        TMap<FGuid, FString> SourceDefinitionJson;
        TMap<FGuid, FString> SourceSoundDefinitionJson;
        TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> ArchiveReader;
        TMap<FGuid, TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe>> ModelArchiveReaders;
        TMap<FGuid, TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe>> SoundArchiveReaders;
        TArray<TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe>> ExternalArchiveReaders;
        FString Error;
    };

    struct FParsedDefinitionResult
    {
        FModelDefinition Definition;
        FString DefinitionJson;
        FString Error;
        bool bValid = false;
    };

    struct FParsedSoundDefinitionResult
    {
        FSoundDefinition Definition;
        FString DefinitionJson;
        FString Error;
        bool bValid = false;
    };

    void AppendError(FString& InOutError, const FString& Error)
    {
        if (Error.IsEmpty()) return;
        if (!InOutError.IsEmpty()) InOutError += TEXT("; ");
        InOutError += Error;
    }

    void LoadBuiltArchive(const FString& RequestedRoot, FDatabaseBuildResult& Result)
    {
        const FString ArchivePath = FGWorldArchive::MakeArchivePath(RequestedRoot);
        Result.ArchiveReader = FGWorldArchiveReader::Open(ArchivePath, Result.Error);
        if (!Result.ArchiveReader.IsValid()) return;

        TArray<FGWorldModelRecord> Records;
        TArray<FGWorldSoundRecord> SoundRecords;
        Result.ArchiveReader->GetRecords(Records);
        Result.ArchiveReader->GetSoundRecords(SoundRecords);
        if (Records.IsEmpty() && SoundRecords.IsEmpty())
        {
            Result.ArchiveReader.Reset();
            Result.Error = TEXT("the .gworld archive contains no assets");
            return;
        }
        for (FGWorldModelRecord& Record : Records)
        {
            const FGuid UUID = Record.Definition.UUID;
            Result.Summaries.Add(UUID, Record.Summary);
            Result.Definitions.Add(UUID, MoveTemp(Record.Definition));
            Result.ModelArchiveReaders.Add(UUID, Result.ArchiveReader);
        }
        for (FGWorldSoundRecord& Record : SoundRecords)
        {
            const FGuid UUID = Record.Definition.UUID;
            if (Result.Definitions.Contains(UUID) || Result.SoundDefinitions.Contains(UUID))
            {
                Result.Error = FString::Printf(TEXT("duplicate asset UUID in .gworld archive: %s"), *UUID.ToString());
                Result.ArchiveReader.Reset();
                return;
            }
            Result.SoundDefinitions.Add(UUID, MoveTemp(Record.Definition));
            Result.SoundArchiveReaders.Add(UUID, Result.ArchiveReader);
        }
    }

    bool ParseArchiveProjectConfig(
        const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe>& Reader,
        const FString& FallbackName,
        FGlTFSimulatorProjectConfig& OutConfig,
        FString& OutError)
    {
        OutConfig = FGlTFSimulatorProjectConfig();
        OutError.Reset();
        if (!Reader.IsValid())
        {
            OutError = TEXT("Archive reader is invalid.");
            return false;
        }
        FString ConfigText;
        if (!Reader->ReadWorldConfig(ConfigText, OutError)) return false;
        FSafeJsonLimits Limits;
        Limits.MaxFileBytes = 64ll * 1024ll * 1024ll;
        Limits.MaxDepth = 32;
        Limits.MaxValues = 131072;
        Limits.MaxContainerEntries = 65536;
        Limits.MaxStringCharacters = 32768;
        Limits.bAllowBackupRecovery = false;
        const FSafeJsonLoadResult Parsed = FSafeFileIO::ParseJsonText(
            ConfigText, Reader->GetArchivePath() + TEXT("#config.json"), Limits);
        if (!Parsed.IsSuccess() || !Parsed.JsonObject.IsValid())
        {
            OutError = Parsed.Error.IsEmpty() ? TEXT("Archive config.json is invalid.") : Parsed.Error;
            return false;
        }
        return GlTFSimulatorProjectConfig::Parse(
            Parsed.JsonObject, FallbackName, OutConfig, OutError);
    }

    void LoadAllowedExternalAssetArchives(FDatabaseBuildResult& Result)
    {
        if (!Result.ArchiveReader.IsValid()) return;

        FGlTFSimulatorProjectConfig WorldConfig;
        FString ConfigError;
        if (!ParseArchiveProjectConfig(
                Result.ArchiveReader,
                FPaths::GetBaseFilename(Result.ArchiveReader->GetArchivePath()),
                WorldConfig,
                ConfigError))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("External asset discovery skipped because the world archive config is invalid: %s"),
                *ConfigError);
            return;
        }
        if (WorldConfig.ProjectType != EGlTFSimulatorProjectType::World
            || !WorldConfig.bAllowExternalAssets)
        {
            return;
        }

        const FString ResourcesRoot = FSafeFileIO::NormalizeFilePath(PATH_EXTERNAL_RESOURCES);
        if (ResourcesRoot.IsEmpty() || !IFileManager::Get().DirectoryExists(*ResourcesRoot)) return;

        TArray<FString> ArchiveFiles;
        IFileManager::Get().FindFilesRecursive(
            ArchiveFiles, *ResourcesRoot, TEXT("*.gasset"), true, false, false);
        ArchiveFiles.Sort();
        if (ArchiveFiles.Num() > 512)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("External asset discovery found %d packs; only the first 512 sorted paths are considered."),
                ArchiveFiles.Num());
            ArchiveFiles.SetNum(512, EAllowShrinking::No);
        }

        for (const FString& CandidatePath : ArchiveFiles)
        {
            FString OpenError;
            TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Reader =
                FGWorldArchiveReader::Open(CandidatePath, OpenError);
            if (!Reader.IsValid())
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("External asset pack ignored. Path=%s Reason=%s"),
                    *CandidatePath, *OpenError);
                continue;
            }

            FGlTFSimulatorProjectConfig PackConfig;
            FString PackConfigError;
            if (!ParseArchiveProjectConfig(
                    Reader, FPaths::GetBaseFilename(CandidatePath), PackConfig, PackConfigError)
                || !GlTFSimulatorProjectConfig::IsExternalAssetProject(PackConfig.ProjectType))
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("External asset pack ignored because its ProjectType/config is invalid. Path=%s Reason=%s"),
                    *CandidatePath, *PackConfigError);
                continue;
            }

            TArray<FGWorldModelRecord> Records;
            TArray<FGWorldSoundRecord> SoundRecords;
            Reader->GetRecords(Records);
            Reader->GetSoundRecords(SoundRecords);
            if (Records.IsEmpty() && SoundRecords.IsEmpty())
            {
                UE_LOG(LogTemp, Warning, TEXT("External asset pack is empty and was ignored: %s"), *CandidatePath);
                continue;
            }

            const EModelDefinitionType RequiredModelType =
                PackConfig.ProjectType == EGlTFSimulatorProjectType::Character
                    ? EModelDefinitionType::Character
                    : EModelDefinitionType::Dynamic;
            bool bPackValid = true;
            FString RejectReason;
            for (const FGWorldModelRecord& Record : Records)
            {
                if (!Record.Definition.UUID.IsValid()
                    || Record.Definition.ModelType != RequiredModelType)
                {
                    bPackValid = false;
                    RejectReason = TEXT("the pack contains a model whose type does not match its ProjectType");
                    break;
                }
                if (Result.Definitions.Contains(Record.Definition.UUID))
                {
                    bPackValid = false;
                    RejectReason = FString::Printf(
                        TEXT("model UUID %s collides with the world or an earlier external pack"),
                        *Record.Definition.UUID.ToString());
                    break;
                }
            }
            if (bPackValid)
            {
                for (const FGWorldSoundRecord& SoundRecord : SoundRecords)
                {
                    const FGuid UUID = SoundRecord.Definition.UUID;
                    if (!UUID.IsValid() || Result.Definitions.Contains(UUID)
                        || Result.SoundDefinitions.Contains(UUID))
                    {
                        bPackValid = false;
                        RejectReason = FString::Printf(TEXT("sound UUID %s collides with an existing asset"), *UUID.ToString());
                        break;
                    }
                }
            }
            if (Result.Definitions.Num() + Records.Num() > FGWorldArchive::MaxModels
                || Result.SoundDefinitions.Num() + SoundRecords.Num() > FGWorldArchive::MaxSounds)
            {
                bPackValid = false;
                RejectReason = TEXT("combined asset count exceeds the archive safety limit");
            }
            if (!bPackValid)
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("External asset pack ignored. Path=%s Reason=%s"),
                    *CandidatePath, *RejectReason);
                continue;
            }

            for (FGWorldModelRecord& Record : Records)
            {
                const FGuid UUID = Record.Definition.UUID;
                Result.Summaries.Add(UUID, Record.Summary);
                Result.Definitions.Add(UUID, MoveTemp(Record.Definition));
                Result.ModelArchiveReaders.Add(UUID, Reader);
            }
            for (FGWorldSoundRecord& Record : SoundRecords)
            {
                const FGuid UUID = Record.Definition.UUID;
                Result.SoundDefinitions.Add(UUID, MoveTemp(Record.Definition));
                Result.SoundArchiveReaders.Add(UUID, Reader);
            }
            Result.ExternalArchiveReaders.Add(Reader);
            UE_LOG(LogTemp, Display,
                TEXT("Mounted external asset pack: %s (%d model(s), %d sound(s), type=%s)"),
                *CandidatePath, Records.Num(), SoundRecords.Num(), *GlTFSimulatorProjectConfig::ToString(PackConfig.ProjectType));
        }
    }

    void ScanAuthoringSources(const FString& RequestedRoot, FDatabaseBuildResult& Result,
        const TSharedRef<TAtomic<bool>, ESPMode::ThreadSafe>& Cancellation)
    {
        // The resources tree is intentionally category-agnostic. A model may live directly under
        // resources, under resources/model, or in any nested authoring folder.
        if (Cancellation->Load()) return;
        const FString ModelRoot = FPaths::Combine(RequestedRoot, DIRECTORY_PROJECT_RESOURCES);
        if (!IFileManager::Get().DirectoryExists(*ModelRoot))
        {
            Result.Error = FString::Printf(
                TEXT("The required authoring resources directory does not exist: %s"), *ModelRoot);
            return;
        }

        // Reject cross-category sibling-JSON collisions before generating any file. One basename
        // has exactly one JSON owner, and the rule is case-insensitive so the same project behaves
        // identically on Windows, macOS and Linux.
        TArray<FString> PreflightFiles;
        IFileManager::Get().FindFilesRecursive(PreflightFiles, *ModelRoot, TEXT("*.*"), true, false, false);
        TSet<FString> ModelJsonKeys;
        TSet<FString> SoundJsonKeys;
        for (const FString& Candidate : PreflightFiles)
        {
            const FString Extension = FPaths::GetExtension(Candidate).ToLower();
            if (Extension != TEXT("glb") && Extension != TEXT("wav")) continue;
            const FString JsonKey = FPaths::ChangeExtension(
                FSafeFileIO::NormalizeFilePath(Candidate), TEXT("json")).ToLower();
            if (JsonKey.IsEmpty()) continue;
            if (Extension == TEXT("glb")) ModelJsonKeys.Add(JsonKey);
            else SoundJsonKeys.Add(JsonKey);
        }
        for (const FString& JsonKey : SoundJsonKeys)
        {
            if (ModelJsonKeys.Contains(JsonKey))
            {
                Result.Error = FString::Printf(
                    TEXT("Asset basename collision: a Model GLB and Sound WAV cannot share one sibling JSON: %s"),
                    *JsonKey);
                return;
            }
        }
        PreflightFiles.Reset();
        ModelJsonKeys.Reset();
        SoundJsonKeys.Reset();

        // Discovery and missing-JSON generation use recursive category scans. Runtime mode never
        // enters this function and therefore never touches the project resources directory.
        TArray<FString> GlbFiles;
        TArray<FString> CreatedDefinitions;
        if (!ModelDefinitionJson::EnsureMissingDefinitions(
                ModelRoot, &CreatedDefinitions, &GlbFiles))
        {
            Result.Error = FString::Printf(
                TEXT("One or more GLB definition files could not be generated under: %s"),
                *ModelRoot);
            return;
        }
        TArray<FString> WavFiles;
        TArray<FString> CreatedSoundDefinitions;
        if (!SoundDefinitionJson::EnsureMissingDefinitions(ModelRoot, &CreatedSoundDefinitions, &WavFiles))
        {
            Result.Error = FString::Printf(TEXT("One or more Sound definition files could not be generated under: %s"), *ModelRoot);
            return;
        }

        if (GlbFiles.IsEmpty() && WavFiles.IsEmpty())
        {
            Result.Error = FString::Printf(TEXT("No Model GLB or Sound WAV assets were found while recursively scanning: %s"), *ModelRoot);
            return;
        }
        if (GlbFiles.Num() > FGWorldArchive::MaxModels || WavFiles.Num() > FGWorldArchive::MaxSounds)
        {
            Result.Error = TEXT("Authoring asset count exceeds the .gworld safety limit");
            return;
        }
        if (Cancellation->Load()) return;
        GlbFiles.Sort();

        UE_LOG(LogTemp, Display,
            TEXT("World source discovery found %d GLB file(s) recursively under '%s' (%d JSON file(s) generated)."),
            GlbFiles.Num(), *ModelRoot, CreatedDefinitions.Num());
        for (const FString& GlbPath : GlbFiles)
        {
            UE_LOG(LogTemp, Verbose, TEXT("World source GLB: %s"), *GlbPath);
        }

        TArray<FParsedDefinitionResult> Parsed;
        Parsed.SetNum(GlbFiles.Num());
        // At most four simultaneous JSON/GLB readers, even on many-core workstations. Each lane
        // owns disjoint output slots; the deterministic merge runs after ParallelFor joins.
        const int32 LaneCount = FMath::Min(4, GlbFiles.Num());
        ParallelFor(LaneCount, [&GlbFiles, &Parsed, Cancellation, LaneCount](const int32 Lane)
        {
            for (int32 Index = Lane; Index < GlbFiles.Num(); Index += LaneCount)
            {
                if (Cancellation->Load()) return;
                const FString& GlbPath = GlbFiles[Index];
                FParsedDefinitionResult& Item = Parsed[Index];
                if (!GlbValidation::ValidateFile(GlbPath, Item.Error)) continue;
                const FString JsonPath = FPaths::ChangeExtension(GlbPath, TEXT("json"));
                Item.bValid = ModelDefinitionJson::LoadDefinition(
                    JsonPath, GlbPath, Item.Definition, Item.Error, &Item.DefinitionJson);
            }
        });
        if (Cancellation->Load()) return;

        for (int32 Index = 0; Index < GlbFiles.Num(); ++Index)
        {
            FParsedDefinitionResult& Item = Parsed[Index];
            const FString JsonPath = FPaths::ChangeExtension(GlbFiles[Index], TEXT("json"));
            if (!Item.bValid)
            {
                const FString Message = FString::Printf(
                    TEXT("Model definition rejected. JSON=%s Reason=%s"),
                    *JsonPath, *Item.Error);
                UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
                UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("ModelDatabase"), Message);
                AppendError(Result.Error, Message);
                continue;
            }
            if (Result.Definitions.Contains(Item.Definition.UUID))
            {
                const FString Message = FString::Printf(
                    TEXT("Duplicate model UUID rejected. UUID=%s JSON=%s"),
                    *Item.Definition.UUID.ToString(), *JsonPath);
                UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
                UFileFunctionLibrary::WriteSimulatorLogAsync(TEXT("ModelDatabase"), Message);
                AppendError(Result.Error, Message);
                continue;
            }

            const FGuid UUID = Item.Definition.UUID;
            Result.SourceDefinitionJson.Add(UUID, MoveTemp(Item.DefinitionJson));
            Result.Definitions.Add(UUID, MoveTemp(Item.Definition));
        }

        TArray<FParsedSoundDefinitionResult> ParsedSounds;
        ParsedSounds.SetNum(WavFiles.Num());
        const int32 SoundLaneCount = FMath::Min(4, WavFiles.Num());
        ParallelFor(SoundLaneCount, [&WavFiles, &ParsedSounds, Cancellation, SoundLaneCount](const int32 Lane)
        {
            for (int32 Index = Lane; Index < WavFiles.Num(); Index += SoundLaneCount)
            {
                if (Cancellation->Load()) return;
                const FString& WavPath = WavFiles[Index];
                FParsedSoundDefinitionResult& Item = ParsedSounds[Index];
                const FString JsonPath = FPaths::ChangeExtension(WavPath, TEXT("json"));
                Item.bValid = SoundDefinitionJson::LoadDefinition(
                    JsonPath, WavPath, Item.Definition, Item.Error, &Item.DefinitionJson);
            }
        });
        if (Cancellation->Load()) return;
        for (int32 Index = 0; Index < WavFiles.Num(); ++Index)
        {
            FParsedSoundDefinitionResult& Item = ParsedSounds[Index];
            const FString JsonPath = FPaths::ChangeExtension(WavFiles[Index], TEXT("json"));
            if (!Item.bValid)
            {
                AppendError(Result.Error, FString::Printf(TEXT("Sound definition rejected. JSON=%s Reason=%s"), *JsonPath, *Item.Error));
                continue;
            }
            const FGuid UUID = Item.Definition.UUID;
            if (Result.Definitions.Contains(UUID) || Result.SoundDefinitions.Contains(UUID))
            {
                AppendError(Result.Error, FString::Printf(TEXT("Duplicate asset UUID rejected. UUID=%s JSON=%s"), *UUID.ToString(), *JsonPath));
                continue;
            }
            Result.SourceSoundDefinitionJson.Add(UUID, MoveTemp(Item.DefinitionJson));
            Result.SoundDefinitions.Add(UUID, MoveTemp(Item.Definition));
        }
    }
}

void UModelDatabaseSubsystem::Deinitialize()
{
    Stop();
    Super::Deinitialize();
}

void UModelDatabaseSubsystem::InitializeForWorld(
    const FString& InWorldRoot,
    FModelDatabaseReady Completion)
{
    check(IsInGameThread());
    Stop();

    WorldRoot = FSafeFileIO::NormalizeFilePath(InWorldRoot);
    const FString ExternalResourcesRoot = FSafeFileIO::NormalizeFilePath(PATH_EXTERNAL_RESOURCES);
    if (!ExternalResourcesRoot.IsEmpty())
    {
        IFileManager::Get().MakeDirectory(*ExternalResourcesRoot, true);
    }
    if (WorldRoot.IsEmpty())
    {
        Completion.ExecuteIfBound(false, TEXT("explicit world root is empty or invalid"));
        return;
    }

    const auto Cancellation = MakeShared<TAtomic<bool>, ESPMode::ThreadSafe>(false);
    ScanCancellation = Cancellation;
    const FString RequestedRoot = WorldRoot;
    const uint64 RequestedGeneration = ++Generation;
    TWeakObjectPtr<UModelDatabaseSubsystem> WeakThis(this);
    const bool bQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, RequestedRoot, RequestedGeneration, Completion, Cancellation]() mutable
    {
        if (Cancellation->Load()) return;
        using namespace ModelDatabasePrivate;
        FDatabaseBuildResult Result;
        LoadBuiltArchive(RequestedRoot, Result);
        if (Result.ArchiveReader.IsValid() && Result.Error.IsEmpty())
        {
            LoadAllowedExternalAssetArchives(Result);
        }
        if (Cancellation->Load()) return;

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
            StrongThis->SoundDefinitions = MoveTemp(Result.SoundDefinitions);
            StrongThis->Summaries = MoveTemp(Result.Summaries);
            StrongThis->SourceDefinitionJson.Reset();
            StrongThis->SourceSoundDefinitionJson.Reset();
            StrongThis->ArchiveReader = MoveTemp(Result.ArchiveReader);
            StrongThis->ModelArchiveReaders = MoveTemp(Result.ModelArchiveReaders);
            StrongThis->SoundArchiveReaders = MoveTemp(Result.SoundArchiveReaders);
            StrongThis->ExternalArchiveReaders = MoveTemp(Result.ExternalArchiveReaders);
            StrongThis->bReady = StrongThis->ArchiveReader.IsValid() && Result.Error.IsEmpty();
            Completion.ExecuteIfBound(StrongThis->bReady, Result.Error);
        });
    });
    if (!bQueued)
    {
        bReady = false;
        Completion.ExecuteIfBound(false, TEXT("model database worker queue is shutting down"));
    }
}

void UModelDatabaseSubsystem::InitializeForAuthoringProject(
    const FString& InProjectRoot,
    FModelDatabaseReady Completion)
{
    check(IsInGameThread());
    Stop();

    WorldRoot = FSafeFileIO::NormalizeFilePath(InProjectRoot);
    if (WorldRoot.IsEmpty())
    {
        Completion.ExecuteIfBound(false, TEXT("explicit project root is empty or invalid"));
        return;
    }

    const auto Cancellation = MakeShared<TAtomic<bool>, ESPMode::ThreadSafe>(false);
    ScanCancellation = Cancellation;
    const FString RequestedRoot = WorldRoot;
    const uint64 RequestedGeneration = ++Generation;
    TWeakObjectPtr<UModelDatabaseSubsystem> WeakThis(this);
    const bool bQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, RequestedRoot, RequestedGeneration, Completion, Cancellation]() mutable
    {
        if (Cancellation->Load()) return;
        using namespace ModelDatabasePrivate;
        FDatabaseBuildResult Result;
        ScanAuthoringSources(RequestedRoot, Result, Cancellation);
        if (Cancellation->Load()) return;
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
            StrongThis->SoundDefinitions = MoveTemp(Result.SoundDefinitions);
            StrongThis->Summaries.Reset();
            StrongThis->SourceDefinitionJson = MoveTemp(Result.SourceDefinitionJson);
            StrongThis->SourceSoundDefinitionJson = MoveTemp(Result.SourceSoundDefinitionJson);
            StrongThis->ArchiveReader.Reset();
            StrongThis->ModelArchiveReaders.Reset();
            StrongThis->SoundArchiveReaders.Reset();
            StrongThis->ExternalArchiveReaders.Reset();
            StrongThis->bReady = Result.Error.IsEmpty()
                && (!StrongThis->Definitions.IsEmpty() || !StrongThis->SoundDefinitions.IsEmpty());
            Completion.ExecuteIfBound(StrongThis->bReady, Result.Error);
        });
    });
    if (!bQueued)
    {
        bReady = false;
        Completion.ExecuteIfBound(false, TEXT("project database worker queue is shutting down"));
    }
}

void UModelDatabaseSubsystem::Stop()
{
    check(IsInGameThread());
    ++Generation;
    if (ScanCancellation.IsValid()) ScanCancellation->Store(true);
    ScanCancellation.Reset();
    bReady = false;
    Definitions.Empty();
    SoundDefinitions.Empty();
    Summaries.Empty();
    SourceDefinitionJson.Empty();
    SourceSoundDefinitionJson.Empty();
    ArchiveReader.Reset();
    ModelArchiveReaders.Reset();
    SoundArchiveReaders.Reset();
    ExternalArchiveReaders.Reset();
    DefinitionDetailsCache.Empty();
    CachedDefinitionBytes = 0;
    DefinitionAccessSerial = 0;
    WorldRoot.Reset();
}


TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe>
UModelDatabaseSubsystem::GetArchiveReaderForModel(const FGuid& UUID) const
{
    if (const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe>* Found = ModelArchiveReaders.Find(UUID))
    {
        return *Found;
    }
    return nullptr;
}

TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe>
UModelDatabaseSubsystem::GetArchiveReaderForSound(const FGuid& UUID) const
{
    if (const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe>* Found = SoundArchiveReaders.Find(UUID))
    {
        return *Found;
    }
    return nullptr;
}

bool UModelDatabaseSubsystem::Resolve(
    const FGuid& UUID,
    FModelDefinition& OutDefinition,
    FString& OutRuntimeReference) const
{
    if (!ArchiveReader.IsValid()) return false;
    const FModelDefinition* Found = Definitions.Find(UUID);
    if (!Found) return false;
    OutDefinition = *Found;
    OutRuntimeReference = FGWorldArchive::MakeModelReference(UUID);
    return true;
}

bool UModelDatabaseSubsystem::ResolveLoadable(
    const FGuid& UUID,
    FModelDefinition& OutDefinition,
    FString& OutRuntimeReference) const
{
    return Resolve(UUID, OutDefinition, OutRuntimeReference) && OutDefinition.IsLoadable();
}

bool UModelDatabaseSubsystem::FindUUIDForReference(
    const FString& ModelReference,
    FGuid& OutUUID) const
{
    return ArchiveReader.IsValid()
        && FGWorldArchive::ParseModelReference(ModelReference, OutUUID)
        && Definitions.Contains(OutUUID);
}

void UModelDatabaseSubsystem::GetDefinitions(TArray<FModelDefinition>& OutDefinitions) const
{
    Definitions.GenerateValueArray(OutDefinitions);
    OutDefinitions.Sort([](const FModelDefinition& A, const FModelDefinition& B)
    {
        return ModelDatabasePrivate::IsGuidLess(A.UUID, B.UUID);
    });
}

void UModelDatabaseSubsystem::GetSoundDefinitions(TArray<FSoundDefinition>& OutDefinitions) const
{
    SoundDefinitions.GenerateValueArray(OutDefinitions);
    OutDefinitions.Sort([](const FSoundDefinition& A, const FSoundDefinition& B)
    {
        return ModelDatabasePrivate::IsGuidLess(A.UUID, B.UUID);
    });
}

bool UModelDatabaseSubsystem::ResolveSound(
    const FGuid& UUID,
    FSoundDefinition& OutDefinition,
    FString& OutRuntimeReference) const
{
    const FSoundDefinition* Found = SoundDefinitions.Find(UUID);
    if (!Found) return false;
    OutDefinition = *Found;
    OutRuntimeReference = ArchiveReader.IsValid() ? FGWorldArchive::MakeSoundReference(UUID) : Found->WavPath;
    return true;
}

bool UModelDatabaseSubsystem::GetSoundDefinitionJson(
    const FGuid& UUID,
    FString& OutJson,
    FString* OutError) const
{
    OutJson.Reset();
    if (const FString* Source = SourceSoundDefinitionJson.Find(UUID))
    {
        OutJson = *Source;
        if (OutError) OutError->Reset();
        return !OutJson.IsEmpty();
    }
    const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Reader = GetArchiveReaderForSound(UUID);
    FString Error;
    const bool bOk = Reader.IsValid() && Reader->ReadSoundDefinition(UUID, OutJson, Error);
    if (OutError) *OutError = Error;
    return bOk;
}

bool UModelDatabaseSubsystem::GetSummary(
    const FGuid& UUID,
    FGWorldModelSummary& OutSummary) const
{
    const FGWorldModelSummary* Found = Summaries.Find(UUID);
    if (!Found) return false;
    OutSummary = *Found;
    return true;
}

bool UModelDatabaseSubsystem::GetMetadata(
    const FGuid& UUID,
    FGWorldModelMetadata& OutMetadata,
    FString* OutError) const
{
    OutMetadata = FGWorldModelMetadata();
    if (OutError) OutError->Reset();
    FString Error;
    const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Reader = GetArchiveReaderForModel(UUID);
    const bool bSuccess = Reader.IsValid()
        && Reader->ReadModelMetadata(UUID, OutMetadata, Error);
    if (!bSuccess && OutError) *OutError = MoveTemp(Error);
    return bSuccess;
}

bool UModelDatabaseSubsystem::GetDefinitionJson(
    const FGuid& UUID,
    FString& OutJson,
    FString* OutError) const
{
    check(IsInGameThread());
    OutJson.Reset();
    if (OutError) OutError->Reset();
    if (const FString* Found = SourceDefinitionJson.Find(UUID))
    {
        OutJson = *Found;
        return true;
    }
    FModelDefinition Definition;
    FString Error;
    const bool bSuccess = LoadDefinitionDetails(UUID, Definition, OutJson, Error);
    if (!bSuccess && OutError) *OutError = MoveTemp(Error);
    return bSuccess;
}

bool UModelDatabaseSubsystem::LoadDefinitionDetails(
    const FGuid& UUID,
    FModelDefinition& InOutDefinition,
    FString& OutJson,
    FString& OutError) const
{
    check(IsInGameThread());
    OutJson.Reset();
    OutError.Reset();
    const FModelDefinition* BaseDefinition = Definitions.Find(UUID);
    if (!BaseDefinition)
    {
        OutError = TEXT("The model UUID is absent from the active model index");
        return false;
    }
    InOutDefinition = *BaseDefinition;
    if (const FString* SourceJson = SourceDefinitionJson.Find(UUID))
    {
        OutJson = *SourceJson;
        return true;
    }
    if (TryGetCachedDefinitionDetails(UUID, InOutDefinition.Bones, OutJson))
    {
        return true;
    }
    const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Reader = GetArchiveReaderForModel(UUID);
    if (!Reader.IsValid())
    {
        OutError = TEXT("No built archive owns this model UUID");
        return false;
    }

    TMap<FString, FString> Bones;
    FString Json;
    if (!Reader->ReadModelDefinition(UUID, Bones, Json, OutError))
    {
        return false;
    }
    CacheDefinitionDetails(UUID, Bones, Json);
    InOutDefinition.Bones = MoveTemp(Bones);
    OutJson = MoveTemp(Json);
    return true;
}

bool UModelDatabaseSubsystem::TryGetCachedDefinitionDetails(
    const FGuid& UUID,
    TMap<FString, FString>& OutBones,
    FString& OutJson) const
{
    check(IsInGameThread());
    FCachedDefinitionDetails* Cached = DefinitionDetailsCache.Find(UUID);
    if (!Cached) return false;
    Cached->LastAccess = ++DefinitionAccessSerial;
    OutBones = Cached->Bones;
    OutJson = Cached->Json;
    return true;
}

void UModelDatabaseSubsystem::CacheDefinitionDetails(
    const FGuid& UUID,
    const TMap<FString, FString>& Bones,
    const FString& Json) const
{
    check(IsInGameThread());
    int64 EstimatedBytes = static_cast<int64>(Json.GetAllocatedSize());
    for (const TPair<FString, FString>& Bone : Bones)
    {
        EstimatedBytes += static_cast<int64>(Bone.Key.GetAllocatedSize());
        EstimatedBytes += static_cast<int64>(Bone.Value.GetAllocatedSize());
        EstimatedBytes += 64; // Conservative map/string bookkeeping allowance.
    }
    if (EstimatedBytes <= 0 || EstimatedBytes > MaxCachedDefinitionBytes) return;

    if (FCachedDefinitionDetails* Existing = DefinitionDetailsCache.Find(UUID))
    {
        CachedDefinitionBytes = FMath::Max<int64>(
            0, CachedDefinitionBytes - Existing->EstimatedBytes);
        DefinitionDetailsCache.Remove(UUID);
    }

    while (!DefinitionDetailsCache.IsEmpty()
        && (DefinitionDetailsCache.Num() >= MaxCachedDefinitionCount
            || CachedDefinitionBytes > MaxCachedDefinitionBytes - EstimatedBytes))
    {
        FGuid OldestUUID;
        uint64 OldestAccess = MAX_uint64;
        for (const TPair<FGuid, FCachedDefinitionDetails>& Pair : DefinitionDetailsCache)
        {
            if (Pair.Value.LastAccess < OldestAccess)
            {
                OldestAccess = Pair.Value.LastAccess;
                OldestUUID = Pair.Key;
            }
        }
        const FCachedDefinitionDetails* Oldest = DefinitionDetailsCache.Find(OldestUUID);
        if (!Oldest) break;
        CachedDefinitionBytes = FMath::Max<int64>(
            0, CachedDefinitionBytes - Oldest->EstimatedBytes);
        DefinitionDetailsCache.Remove(OldestUUID);
    }

    FCachedDefinitionDetails& Cached = DefinitionDetailsCache.Add(UUID);
    Cached.Bones = Bones;
    Cached.Json = Json;
    Cached.LastAccess = ++DefinitionAccessSerial;
    Cached.EstimatedBytes = EstimatedBytes;
    CachedDefinitionBytes += EstimatedBytes;
}
