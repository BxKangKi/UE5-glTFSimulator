// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file SafeFileIO.cpp
 * @brief Implements bounded JSON parsing, asynchronous file jobs, and recoverable atomic commits.
 */
#include "System/SafeFileIO.h"
#include "HAL/CriticalSection.h"

#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/StringConv.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/Crc.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace SafeFileIOPrivate
{
    /** One diagnostic append waiting to be coalesced with adjacent writes to the same file. */
    struct FQueuedAppendRequest
    {
        FString Text;
        FSafeFileIO::FWriteCallback Callback;
    };

    /** Shared process-local state. It contains no UObjects and is safe to use on worker threads. */
    struct FState
    {
        FCriticalSection StateLock;
        TMap<FString, TSharedPtr<FCriticalSection, ESPMode::ThreadSafe>> PathLocks;
        TMap<FString, TArray<FQueuedAppendRequest>> PendingAppends;
        TSet<FString> ActiveAppendWorkers;
        int64 PendingAppendCharacters = 0;
        FThreadSafeCounter PendingOperations;
        FThreadSafeCounter TemporarySequence;
        FThreadSafeCounter ShutdownFlag;
        uint64 NextWriteSequence = 1;
        TMap<FString, uint64> LatestWriteSequenceByPath;
    };

    FState& GetState()
    {
        static FState State;
        return State;
    }

    /**
     * Tracks a scheduled job from queue submission until the worker has completely returned.
     *
     * Incrementing before Async() is essential: otherwise module shutdown could observe zero work
     * while a lambda that still contains module code is waiting in the thread-pool queue.
     */
    class FTrackedOperation
    {
    public:
        FTrackedOperation()
        {
            GetState().PendingOperations.Increment();
        }

        ~FTrackedOperation()
        {
            GetState().PendingOperations.Decrement();
        }
    };

    bool IsShuttingDown()
    {
        return GetState().ShutdownFlag.GetValue() != 0;
    }

    TSharedRef<FCriticalSection, ESPMode::ThreadSafe> GetPathLock(const FString& NormalizedPath)
    {
        FState& State = GetState();
        FScopeLock StateScope(&State.StateLock);

        TSharedPtr<FCriticalSection, ESPMode::ThreadSafe>& ExistingLock = State.PathLocks.FindOrAdd(NormalizedPath);
        if (!ExistingLock.IsValid())
        {
            ExistingLock = MakeShared<FCriticalSection, ESPMode::ThreadSafe>();
        }
        return ExistingLock.ToSharedRef();
    }

    /** Assigns a monotonic sequence so an older async save can never overwrite a newer snapshot. */
    uint64 ReserveWriteSequence(const FString& NormalizedPath)
    {
        FState& State = GetState();
        FScopeLock StateScope(&State.StateLock);
        const uint64 Sequence = State.NextWriteSequence++;
        State.LatestWriteSequenceByPath.Add(NormalizedPath, Sequence);
        return Sequence;
    }

    bool IsLatestWriteSequence(const FString& NormalizedPath, const uint64 Sequence)
    {
        if (Sequence == 0)
        {
            return true;
        }

        FState& State = GetState();
        FScopeLock StateScope(&State.StateLock);
        const uint64* Latest = State.LatestWriteSequenceByPath.Find(NormalizedPath);
        return Latest && *Latest == Sequence;
    }

    FString GetBackupPath(const FString& Path)
    {
        return Path + TEXT(".bak");
    }

    FString GetTemporaryPath(const FString& Path)
    {
        const int32 Sequence = GetState().TemporarySequence.Increment();
        return FString::Printf(
            TEXT("%s.tmp.%u.%d"),
            *Path,
            FPlatformProcess::GetCurrentProcessId(),
            Sequence);
    }

    bool IsPathUsable(const FString& Path)
    {
        return !Path.IsEmpty() && !FPaths::GetCleanFilename(Path).IsEmpty();
    }

    /** Reads exactly one file into memory after checking both the configured and int32 array limits. */
    FSafeBinaryLoadResult ReadBytesUnlocked(const FString& Path, const int64 MaxBytes)
    {
        FSafeBinaryLoadResult Result;
        Result.Path = Path;

        if (!IsPathUsable(Path) || MaxBytes < 0)
        {
            Result.Status = ESafeFileIOStatus::InvalidPath;
            Result.Error = TEXT("The path or maximum byte count is invalid");
            return Result;
        }

        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        if (!PlatformFile.FileExists(*Path))
        {
            Result.Status = ESafeFileIOStatus::Missing;
            Result.Error = TEXT("The file does not exist");
            return Result;
        }

        TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*Path, false));
        if (!Handle.IsValid())
        {
            Result.Status = ESafeFileIOStatus::ReadFailed;
            Result.Error = TEXT("OpenRead failed");
            return Result;
        }

        const int64 FileSize = Handle->Size();
        if (FileSize < 0)
        {
            Result.Status = ESafeFileIOStatus::ReadFailed;
            Result.Error = TEXT("The file reported a negative size");
            return Result;
        }
        if (FileSize > MaxBytes || FileSize > static_cast<int64>(MAX_int32))
        {
            Result.Status = ESafeFileIOStatus::TooLarge;
            Result.Error = FString::Printf(
                TEXT("The file is %lld bytes, above the %lld-byte limit"),
                FileSize,
                MaxBytes);
            return Result;
        }

        Result.Data.SetNumUninitialized(static_cast<int32>(FileSize));
        if (FileSize > 0 && !Handle->Read(Result.Data.GetData(), FileSize))
        {
            Result.Data.Reset();
            Result.Status = ESafeFileIOStatus::ReadFailed;
            Result.Error = TEXT("The file could not be read completely");
            return Result;
        }

        Result.Status = ESafeFileIOStatus::Success;
        return Result;
    }

    /** Rejects pathological depth, container size, strings, primitives, and controls before DOM allocation. */
    bool PreScanJsonText(const FString& Text, const FSafeJsonLimits& Limits, FString& OutError)
    {
        OutError.Reset();
        if (Text.IsEmpty())
        {
            OutError = TEXT("JSON text is empty");
            return false;
        }
        if (Limits.MaxFileBytes < 0 || Limits.MaxDepth <= 0 || Limits.MaxValues <= 0 ||
            Limits.MaxContainerEntries <= 0 || Limits.MaxStringCharacters <= 0 ||
            Limits.MaxPrimitiveCharacters <= 0)
        {
            OutError = TEXT("JSON safety limits are invalid");
            return false;
        }

        struct FContainerScanState
        {
            TCHAR OpeningCharacter = 0;
            int32 CommaCount = 0;
            bool bHasContent = false;
        };

        TArray<FContainerScanState> ContainerStack;
        ContainerStack.Reserve(FMath::Min(Limits.MaxDepth, 256));

        bool bInString = false;
        bool bEscaped = false;
        bool bInPrimitive = false;
        bool bSawRoot = false;
        bool bRootClosed = false;
        int32 CurrentStringCharacters = 0;
        int32 CurrentPrimitiveCharacters = 0;
        int64 ApproximateTokenCount = 0;
        const int64 MaximumApproximateTokenCount = static_cast<int64>(Limits.MaxValues) * 2ll;

        auto MarkContainerContent = [&ContainerStack]()
        {
            if (!ContainerStack.IsEmpty())
            {
                ContainerStack.Last().bHasContent = true;
            }
        };

        auto ValidateContainerEntryCount = [&Limits, &OutError](const FContainerScanState& State)
        {
            const int64 EntryCount = State.bHasContent
                ? static_cast<int64>(State.CommaCount) + 1ll
                : 0ll;
            if (EntryCount > Limits.MaxContainerEntries)
            {
                OutError = TEXT("A JSON container exceeds the configured entry limit before deserialization");
                return false;
            }
            return true;
        };

        for (int32 Index = 0; Index < Text.Len(); ++Index)
        {
            const TCHAR Character = Text[Index];

            if (bInString)
            {
                if (bEscaped)
                {
                    bEscaped = false;
                    if (++CurrentStringCharacters > Limits.MaxStringCharacters)
                    {
                        OutError = TEXT("A JSON string exceeds the configured character limit");
                        return false;
                    }
                    continue;
                }
                if (Character == TEXT('\\'))
                {
                    bEscaped = true;
                    continue;
                }
                if (Character == TEXT('"'))
                {
                    bInString = false;
                    continue;
                }
                if (Character < 0x20)
                {
                    OutError = TEXT("A JSON string contains an unescaped control character");
                    return false;
                }
                if (++CurrentStringCharacters > Limits.MaxStringCharacters)
                {
                    OutError = TEXT("A JSON string exceeds the configured character limit");
                    return false;
                }
                continue;
            }

            if (FChar::IsWhitespace(Character))
            {
                bInPrimitive = false;
                CurrentPrimitiveCharacters = 0;
                continue;
            }

            if (bRootClosed)
            {
                OutError = TEXT("JSON contains non-whitespace data after the root object");
                return false;
            }

            if (!bSawRoot)
            {
                bSawRoot = true;
                if (Character != TEXT('{'))
                {
                    OutError = TEXT("The JSON root must be an object");
                    return false;
                }
            }

            if (Character == TEXT('"'))
            {
                MarkContainerContent();
                bInString = true;
                bEscaped = false;
                bInPrimitive = false;
                CurrentStringCharacters = 0;
                CurrentPrimitiveCharacters = 0;
                if (++ApproximateTokenCount > MaximumApproximateTokenCount)
                {
                    OutError = TEXT("JSON token count exceeds the configured limit");
                    return false;
                }
                continue;
            }

            if (Character == TEXT('{') || Character == TEXT('['))
            {
                MarkContainerContent();
                FContainerScanState State;
                State.OpeningCharacter = Character;
                ContainerStack.Add(State);
                bInPrimitive = false;
                CurrentPrimitiveCharacters = 0;
                if (ContainerStack.Num() > Limits.MaxDepth)
                {
                    OutError = TEXT("JSON nesting depth exceeds the configured limit");
                    return false;
                }
                if (++ApproximateTokenCount > MaximumApproximateTokenCount)
                {
                    OutError = TEXT("JSON token count exceeds the configured limit");
                    return false;
                }
                continue;
            }

            if (Character == TEXT('}') || Character == TEXT(']'))
            {
                if (ContainerStack.IsEmpty())
                {
                    OutError = TEXT("JSON closes a container that was never opened");
                    return false;
                }

                const FContainerScanState ClosingState = ContainerStack.Pop(EAllowShrinking::No);
                const bool bMatchingPair =
                    (ClosingState.OpeningCharacter == TEXT('{') && Character == TEXT('}')) ||
                    (ClosingState.OpeningCharacter == TEXT('[') && Character == TEXT(']'));
                if (!bMatchingPair)
                {
                    OutError = TEXT("JSON object and array delimiters are mismatched");
                    return false;
                }
                if (!ValidateContainerEntryCount(ClosingState))
                {
                    return false;
                }

                bInPrimitive = false;
                CurrentPrimitiveCharacters = 0;
                if (ContainerStack.IsEmpty())
                {
                    bRootClosed = true;
                }
                continue;
            }

            if (Character == TEXT(','))
            {
                if (ContainerStack.IsEmpty())
                {
                    OutError = TEXT("JSON contains a comma outside a container");
                    return false;
                }
                FContainerScanState& State = ContainerStack.Last();
                ++State.CommaCount;
                if (static_cast<int64>(State.CommaCount) + 1ll > Limits.MaxContainerEntries)
                {
                    OutError = TEXT("A JSON container exceeds the configured entry limit before deserialization");
                    return false;
                }
                bInPrimitive = false;
                CurrentPrimitiveCharacters = 0;
                continue;
            }

            if (Character == TEXT(':'))
            {
                bInPrimitive = false;
                CurrentPrimitiveCharacters = 0;
                continue;
            }

            MarkContainerContent();
            if (!bInPrimitive)
            {
                bInPrimitive = true;
                CurrentPrimitiveCharacters = 0;
                if (++ApproximateTokenCount > MaximumApproximateTokenCount)
                {
                    OutError = TEXT("JSON token count exceeds the configured limit");
                    return false;
                }
            }
            if (++CurrentPrimitiveCharacters > Limits.MaxPrimitiveCharacters)
            {
                OutError = TEXT("A JSON primitive token exceeds the configured character limit");
                return false;
            }
        }

        if (bInString || bEscaped)
        {
            OutError = TEXT("JSON ends inside a quoted string");
            return false;
        }
        if (!ContainerStack.IsEmpty())
        {
            OutError = TEXT("JSON ends before all objects and arrays are closed");
            return false;
        }
        if (!bSawRoot || !bRootClosed)
        {
            OutError = TEXT("JSON does not contain one complete root object");
            return false;
        }
        return true;
    }

    /** Recursively validates the parsed DOM with a strictly bounded visit budget. */
    bool ValidateJsonValue(
        const TSharedPtr<FJsonValue>& Value,
        const FSafeJsonLimits& Limits,
        const int32 Depth,
        int32& RemainingValues,
        FString& OutError)
    {
        if (!Value.IsValid())
        {
            OutError = TEXT("JSON contains an invalid value pointer");
            return false;
        }
        if (Depth > Limits.MaxDepth)
        {
            OutError = TEXT("JSON DOM nesting depth exceeds the configured limit");
            return false;
        }
        if (--RemainingValues < 0)
        {
            OutError = TEXT("JSON DOM contains too many values");
            return false;
        }

        switch (Value->Type)
        {
        case EJson::Null:
        case EJson::Boolean:
            return true;

        case EJson::String:
            if (Value->AsString().Len() > Limits.MaxStringCharacters)
            {
                OutError = TEXT("A JSON string value exceeds the configured character limit");
                return false;
            }
            return true;

        case EJson::Number:
            if (!FMath::IsFinite(Value->AsNumber()))
            {
                OutError = TEXT("JSON contains NaN or an infinite numeric value");
                return false;
            }
            return true;

        case EJson::Array:
        {
            const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
            if (Array.Num() > Limits.MaxContainerEntries)
            {
                OutError = TEXT("A JSON array exceeds the configured entry limit");
                return false;
            }
            for (const TSharedPtr<FJsonValue>& Child : Array)
            {
                if (!ValidateJsonValue(Child, Limits, Depth + 1, RemainingValues, OutError))
                {
                    return false;
                }
            }
            return true;
        }

        case EJson::Object:
        {
            const TSharedPtr<FJsonObject> Object = Value->AsObject();
            if (!Object.IsValid())
            {
                OutError = TEXT("JSON contains an invalid object");
                return false;
            }
            if (Object->Values.Num() > Limits.MaxContainerEntries)
            {
                OutError = TEXT("A JSON object exceeds the configured entry limit");
                return false;
            }
            for (const auto& Pair : Object->Values)
            {
                const FString JsonKey(Pair.Key);
                if (JsonKey.IsEmpty() || JsonKey.Len() > Limits.MaxStringCharacters)
                {
                    OutError = TEXT("A JSON object key is empty or exceeds the configured character limit");
                    return false;
                }
                if (!ValidateJsonValue(Pair.Value, Limits, Depth + 1, RemainingValues, OutError))
                {
                    return false;
                }
            }
            return true;
        }

        default:
            OutError = TEXT("JSON contains an unsupported value type");
            return false;
        }
    }

    FSafeJsonLoadResult ParseJsonTextInternal(
        const FString& JsonText,
        const FString& SourceLabel,
        const FSafeJsonLimits& Limits)
    {
        FSafeJsonLoadResult Result;
        Result.Path = SourceLabel;

        if (Limits.MaxFileBytes < 0 || JsonText.Len() > Limits.MaxFileBytes)
        {
            Result.Status = ESafeFileIOStatus::TooLarge;
            Result.Error = TEXT("JSON text exceeds the configured byte limit");
            return Result;
        }
        FTCHARToUTF8 EncodedLengthProbe(*JsonText);
        if (EncodedLengthProbe.Length() > Limits.MaxFileBytes)
        {
            Result.Status = ESafeFileIOStatus::TooLarge;
            Result.Error = TEXT("UTF-8 JSON payload exceeds the configured byte limit");
            return Result;
        }

        FString PreScanError;
        if (!PreScanJsonText(JsonText, Limits, PreScanError))
        {
            Result.Status = ESafeFileIOStatus::InvalidJson;
            Result.Error = MoveTemp(PreScanError);
            return Result;
        }

        TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
        TSharedPtr<FJsonObject> JsonObject;
        if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
        {
            Result.Status = ESafeFileIOStatus::InvalidJson;
            Result.Error = FString::Printf(TEXT("JSON deserialization failed: %s"), *Reader->GetErrorMessage());
            return Result;
        }

        int32 RemainingValues = Limits.MaxValues;
        const TSharedPtr<FJsonValue> RootValue = MakeShared<FJsonValueObject>(JsonObject);
        FString ValidationError;
        if (!ValidateJsonValue(RootValue, Limits, 0, RemainingValues, ValidationError))
        {
            Result.Status = ESafeFileIOStatus::ValidationFailed;
            Result.Error = MoveTemp(ValidationError);
            return Result;
        }

        Result.Status = ESafeFileIOStatus::Success;
        Result.JsonObject = MoveTemp(JsonObject);
        return Result;
    }

    /** Validates strict UTF-8 so malformed external bytes never reach TCHAR/JSON code. */
    bool DecodeUtf8Strict(const TArray<uint8>& Bytes, FString& OutText, FString& OutError)
    {
        OutText.Reset();
        OutError.Reset();

        int32 Offset = 0;
        if (Bytes.Num() >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF)
        {
            Offset = 3;
        }

        for (int32 Index = Offset; Index < Bytes.Num();)
        {
            const uint8 First = Bytes[Index];
            int32 Length = 0;
            uint32 CodePoint = 0;
            uint32 MinimumCodePoint = 0;

            if (First <= 0x7F)
            {
                Length = 1;
                CodePoint = First;
            }
            else if ((First & 0xE0) == 0xC0)
            {
                Length = 2;
                CodePoint = First & 0x1F;
                MinimumCodePoint = 0x80;
            }
            else if ((First & 0xF0) == 0xE0)
            {
                Length = 3;
                CodePoint = First & 0x0F;
                MinimumCodePoint = 0x800;
            }
            else if ((First & 0xF8) == 0xF0)
            {
                Length = 4;
                CodePoint = First & 0x07;
                MinimumCodePoint = 0x10000;
            }
            else
            {
                OutError = FString::Printf(TEXT("Invalid UTF-8 lead byte at byte %d"), Index);
                return false;
            }

            if (Index + Length > Bytes.Num())
            {
                OutError = TEXT("UTF-8 text ends inside a multi-byte sequence");
                return false;
            }

            for (int32 ContinuationIndex = 1; ContinuationIndex < Length; ++ContinuationIndex)
            {
                const uint8 Continuation = Bytes[Index + ContinuationIndex];
                if ((Continuation & 0xC0) != 0x80)
                {
                    OutError = FString::Printf(
                        TEXT("Invalid UTF-8 continuation byte at byte %d"),
                        Index + ContinuationIndex);
                    return false;
                }
                CodePoint = (CodePoint << 6) | (Continuation & 0x3F);
            }

            if ((Length > 1 && CodePoint < MinimumCodePoint) ||
                CodePoint > 0x10FFFF ||
                (CodePoint >= 0xD800 && CodePoint <= 0xDFFF) ||
                CodePoint == 0)
            {
                OutError = FString::Printf(TEXT("Invalid UTF-8 code point at byte %d"), Index);
                return false;
            }
            Index += Length;
        }

        if (Bytes.Num() == Offset)
        {
            return true;
        }

        const ANSICHAR* Utf8Data = reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset);
        const int32 Utf8Length = Bytes.Num() - Offset;
        FUTF8ToTCHAR Converter(Utf8Data, Utf8Length);
        OutText.AppendChars(Converter.Get(), Converter.Length());
        return true;
    }

    /** Validates a JSON DOM before serialization, including finite-number checks. */
    bool ValidateJsonObjectForSave(
        const TSharedRef<FJsonObject>& JsonObject,
        const int64 MaxOutputBytes,
        FString& OutError)
    {
        FSafeJsonLimits Limits;
        Limits.MaxFileBytes = MaxOutputBytes;
        Limits.MaxDepth = 64;
        Limits.MaxValues = 1000000;
        Limits.MaxContainerEntries = 250000;
        Limits.MaxStringCharacters = 4 * 1024 * 1024;
        Limits.MaxPrimitiveCharacters = 1024;

        int32 RemainingValues = Limits.MaxValues;
        const TSharedPtr<FJsonValue> RootValue = MakeShared<FJsonValueObject>(JsonObject);
        return ValidateJsonValue(RootValue, Limits, 0, RemainingValues, OutError);
    }

    /** Returns newest transaction files first so an interrupted first save can be recovered. */
    TArray<FString> FindTemporaryCandidatesUnlocked(const FString& TargetPath)
    {
        TArray<FString> FileNames;
        const FString Directory = FPaths::GetPath(TargetPath);
        const FString Wildcard = FPaths::Combine(
            Directory,
            FPaths::GetCleanFilename(TargetPath) + TEXT(".tmp.*"));
        IFileManager::Get().FindFiles(FileNames, *Wildcard, true, false);

        TArray<FString> Candidates;
        Candidates.Reserve(FileNames.Num());
        for (const FString& FileName : FileNames)
        {
            Candidates.Add(FPaths::Combine(Directory, FileName));
        }
        Candidates.Sort([](const FString& A, const FString& B)
        {
            return IFileManager::Get().GetTimeStamp(*A) > IFileManager::Get().GetTimeStamp(*B);
        });
        return Candidates;
    }

    FSafeJsonLoadResult ReadJsonCandidateUnlocked(
        const FString& CandidatePath,
        const FString& ReportedPath,
        const FSafeJsonLimits& Limits)
    {
        FSafeJsonLoadResult Result;
        Result.Path = ReportedPath;

        FSafeBinaryLoadResult BinaryResult = ReadBytesUnlocked(CandidatePath, Limits.MaxFileBytes);
        if (!BinaryResult.IsSuccess())
        {
            Result.Status = BinaryResult.Status;
            Result.Error = BinaryResult.Error;
            return Result;
        }

        FString JsonText;
        FString DecodeError;
        if (!DecodeUtf8Strict(BinaryResult.Data, JsonText, DecodeError))
        {
            Result.Status = ESafeFileIOStatus::DecodeFailed;
            Result.Error = MoveTemp(DecodeError);
            return Result;
        }

        Result = ParseJsonTextInternal(JsonText, ReportedPath, Limits);
        return Result;
    }

    /** Writes bytes, fully flushes them, verifies the temporary file, and commits recoverably. */
    FSafeFileWriteResult CommitBytesUnlocked(
        const TArray<uint8>& Data,
        const FString& Path,
        const int64 MaxOutputBytes,
        const uint64 WriteSequence = 0)
    {
        FSafeFileWriteResult Result;
        Result.Path = Path;

        // Async jobs can be scheduled out of order by the worker pool. Only the newest snapshot
        // for a path is allowed to enter the durable commit transaction.
        if (!IsLatestWriteSequence(Path, WriteSequence))
        {
            Result.Status = ESafeFileIOStatus::Superseded;
            Result.Error = TEXT("A newer save request superseded this snapshot");
            return Result;
        }

        if (!IsPathUsable(Path) || MaxOutputBytes < 0)
        {
            Result.Status = ESafeFileIOStatus::InvalidPath;
            Result.Error = TEXT("The path or maximum output size is invalid");
            return Result;
        }
        if (Data.Num() > MaxOutputBytes)
        {
            Result.Status = ESafeFileIOStatus::TooLarge;
            Result.Error = FString::Printf(
                TEXT("The serialized payload is %d bytes, above the %lld-byte limit"),
                Data.Num(),
                MaxOutputBytes);
            return Result;
        }

        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        const FString Directory = FPaths::GetPath(Path);
        if (!Directory.IsEmpty() && !PlatformFile.CreateDirectoryTree(*Directory))
        {
            Result.Status = ESafeFileIOStatus::WriteFailed;
            Result.Error = TEXT("The parent directory could not be created");
            return Result;
        }

        if (!IsLatestWriteSequence(Path, WriteSequence))
        {
            Result.Status = ESafeFileIOStatus::Superseded;
            Result.Error = TEXT("A newer save request superseded this snapshot before disk write");
            return Result;
        }

        const FString TemporaryPath = GetTemporaryPath(Path);
        const FString PreviousTargetPath = TemporaryPath + TEXT(".previous");
        const FString BackupPath = GetBackupPath(Path);
        PlatformFile.DeleteFile(*TemporaryPath);
        PlatformFile.DeleteFile(*PreviousTargetPath);

        {
            TUniquePtr<IFileHandle> Handle(PlatformFile.OpenWrite(*TemporaryPath, false, false));
            if (!Handle.IsValid())
            {
                Result.Status = ESafeFileIOStatus::WriteFailed;
                Result.Error = TEXT("The temporary file could not be opened for writing");
                return Result;
            }

            if (Data.Num() > 0 && !Handle->Write(Data.GetData(), Data.Num()))
            {
                PlatformFile.DeleteFile(*TemporaryPath);
                Result.Status = ESafeFileIOStatus::WriteFailed;
                Result.Error = TEXT("The temporary file could not be written completely");
                return Result;
            }

            // A full flush asks the platform to push the temporary transaction to durable storage.
            if (!Handle->Flush(true))
            {
                PlatformFile.DeleteFile(*TemporaryPath);
                Result.Status = ESafeFileIOStatus::WriteFailed;
                Result.Error = TEXT("The temporary file could not be flushed to storage");
                return Result;
            }
        }

        FSafeBinaryLoadResult Verification = ReadBytesUnlocked(TemporaryPath, MaxOutputBytes);
        const uint32 ExpectedCrc = Data.IsEmpty() ? 0u : FCrc::MemCrc32(Data.GetData(), Data.Num());
        const uint32 ActualCrc = Verification.Data.IsEmpty()
            ? 0u
            : FCrc::MemCrc32(Verification.Data.GetData(), Verification.Data.Num());
        const bool bVerified = Verification.IsSuccess() &&
            Verification.Data.Num() == Data.Num() &&
            ActualCrc == ExpectedCrc;
        if (!bVerified)
        {
            PlatformFile.DeleteFile(*TemporaryPath);
            Result.Status = ESafeFileIOStatus::WriteFailed;
            Result.Error = TEXT("The temporary file failed post-flush verification");
            return Result;
        }

        // A newer request may have been scheduled while this worker flushed and verified its temp
        // file. Refuse to rotate the primary when this snapshot is no longer the newest one.
        if (!IsLatestWriteSequence(Path, WriteSequence))
        {
            PlatformFile.DeleteFile(*TemporaryPath);
            Result.Status = ESafeFileIOStatus::Superseded;
            Result.Error = TEXT("A newer save request superseded this snapshot before commit");
            return Result;
        }

        const bool bTargetExisted = PlatformFile.FileExists(*Path);
        if (bTargetExisted && !PlatformFile.MoveFile(*PreviousTargetPath, *Path))
        {
            PlatformFile.DeleteFile(*TemporaryPath);
            Result.Status = ESafeFileIOStatus::CommitFailed;
            Result.Error = TEXT("The previous target could not be moved into the transaction journal");
            return Result;
        }

        if (!PlatformFile.MoveFile(*Path, *TemporaryPath))
        {
            // Keep the old .bak untouched until the new primary exists. This guarantees that a
            // rename failure cannot destroy the last known-good generation.
            if (bTargetExisted && PlatformFile.FileExists(*PreviousTargetPath) && !PlatformFile.FileExists(*Path))
            {
                PlatformFile.MoveFile(*Path, *PreviousTargetPath);
            }
            PlatformFile.DeleteFile(*TemporaryPath);
            Result.Status = ESafeFileIOStatus::CommitFailed;
            Result.Error = TEXT("The verified temporary file could not be committed as the target");
            return Result;
        }

        if (bTargetExisted && PlatformFile.FileExists(*PreviousTargetPath))
        {
            // The new primary is already durable. Rotate the previous primary into .bak only now,
            // so a crash during the earlier rename window always leaves a recoverable generation.
            PlatformFile.DeleteFile(*BackupPath);
            if (!PlatformFile.MoveFile(*BackupPath, *PreviousTargetPath))
            {
                // Do not fail a successful primary commit. Leave the journal file for startup
                // cleanup/recovery and report the degraded backup rotation in the log.
                UE_LOG(LogTemp, Warning,
                    TEXT("Committed primary data but could not rotate its previous generation to backup. Path=%s Journal=%s"),
                    *Path,
                    *PreviousTargetPath);
            }
        }

        Result.Status = ESafeFileIOStatus::Success;
        Result.BytesWritten = Data.Num();
        return Result;
    }

    void DispatchJsonCallback(
        FSafeFileIO::FJsonLoadCallback Callback,
        FSafeJsonLoadResult Result,
        TSharedPtr<FTrackedOperation, ESPMode::ThreadSafe> OperationLifetime = nullptr)
    {
        if (!Callback || IsShuttingDown())
        {
            return;
        }
        AsyncTask(ENamedThreads::GameThread,
            [Callback = MoveTemp(Callback), Result = MoveTemp(Result),
                OperationLifetime = MoveTemp(OperationLifetime)]() mutable
            {
                // Holding OperationLifetime keeps module shutdown from unloading callback code while
                // this game-thread task is still queued. Shutdown pumps the queue and suppresses it.
                if (!IsShuttingDown())
                {
                    Callback(MoveTemp(Result));
                }
            });
    }

    void DispatchWriteCallback(
        FSafeFileIO::FWriteCallback Callback,
        FSafeFileWriteResult Result,
        TSharedPtr<FTrackedOperation, ESPMode::ThreadSafe> OperationLifetime = nullptr)
    {
        if (!Callback || IsShuttingDown())
        {
            return;
        }
        AsyncTask(ENamedThreads::GameThread,
            [Callback = MoveTemp(Callback), Result = MoveTemp(Result),
                OperationLifetime = MoveTemp(OperationLifetime)]() mutable
            {
                if (!IsShuttingDown())
                {
                    Callback(MoveTemp(Result));
                }
            });
    }

    void DispatchBinaryCallback(
        FSafeFileIO::FBinaryLoadCallback Callback,
        FSafeBinaryLoadResult Result,
        TSharedPtr<FTrackedOperation, ESPMode::ThreadSafe> OperationLifetime = nullptr)
    {
        if (!Callback || IsShuttingDown())
        {
            return;
        }
        AsyncTask(ENamedThreads::GameThread,
            [Callback = MoveTemp(Callback), Result = MoveTemp(Result),
                OperationLifetime = MoveTemp(OperationLifetime)]() mutable
            {
                if (!IsShuttingDown())
                {
                    Callback(MoveTemp(Result));
                }
            });
    }
}

bool FSafeFileIO::RunTrackedWorker(FTrackedTask Task)
{
    if (!Task || SafeFileIOPrivate::IsShuttingDown())
    {
        return false;
    }

    // Reserve the lifetime before queue submission so module shutdown cannot miss queued work.
    const TSharedRef<SafeFileIOPrivate::FTrackedOperation, ESPMode::ThreadSafe> TrackedOperation =
        MakeShared<SafeFileIOPrivate::FTrackedOperation, ESPMode::ThreadSafe>();
    Async(EAsyncExecution::ThreadPool,
        [Task = MoveTemp(Task), TrackedOperation]() mutable
        {
            if (!SafeFileIOPrivate::IsShuttingDown())
            {
                Task();
            }
        });
    return true;
}

bool FSafeFileIO::DispatchTrackedGameThread(FTrackedTask Task)
{
    if (!Task || SafeFileIOPrivate::IsShuttingDown())
    {
        return false;
    }

    const TSharedRef<SafeFileIOPrivate::FTrackedOperation, ESPMode::ThreadSafe> TrackedOperation =
        MakeShared<SafeFileIOPrivate::FTrackedOperation, ESPMode::ThreadSafe>();
    if (IsInGameThread())
    {
        Task();
        return true;
    }

    AsyncTask(ENamedThreads::GameThread,
        [Task = MoveTemp(Task), TrackedOperation]() mutable
        {
            if (!SafeFileIOPrivate::IsShuttingDown())
            {
                Task();
            }
        });
    return true;
}

bool FSafeFileIO::IsShuttingDown()
{
    return SafeFileIOPrivate::IsShuttingDown();
}

FString FSafeFileIO::NormalizeFilePath(const FString& Path)
{
    FString NormalizedPath = Path.TrimStartAndEnd();
    if (NormalizedPath.IsEmpty())
    {
        return FString();
    }

    NormalizedPath = FPaths::ConvertRelativePathToFull(NormalizedPath);
    FPaths::NormalizeFilename(NormalizedPath);
    FPaths::CollapseRelativeDirectories(NormalizedPath);
    return NormalizedPath;
}

FSafeJsonLoadResult FSafeFileIO::ParseJsonText(
    const FString& JsonText,
    const FString& SourceLabel,
    const FSafeJsonLimits& Limits)
{
    return SafeFileIOPrivate::ParseJsonTextInternal(JsonText, SourceLabel, Limits);
}

FSafeJsonLoadResult FSafeFileIO::ParseJsonUtf8Bytes(
    const TArray<uint8>& Utf8Bytes,
    const FString& SourceLabel,
    const FSafeJsonLimits& Limits)
{
    FSafeJsonLoadResult Result;
    Result.Path = SourceLabel;
    if (Limits.MaxFileBytes < 0 || Utf8Bytes.Num() > Limits.MaxFileBytes)
    {
        Result.Status = ESafeFileIOStatus::TooLarge;
        Result.Error = TEXT("UTF-8 JSON payload exceeds the configured byte limit");
        return Result;
    }

    FString JsonText;
    FString DecodeError;
    if (!SafeFileIOPrivate::DecodeUtf8Strict(Utf8Bytes, JsonText, DecodeError))
    {
        Result.Status = ESafeFileIOStatus::DecodeFailed;
        Result.Error = MoveTemp(DecodeError);
        return Result;
    }
    return SafeFileIOPrivate::ParseJsonTextInternal(JsonText, SourceLabel, Limits);
}

FSafeJsonLoadResult FSafeFileIO::LoadJsonBlocking(const FString& Path, const FSafeJsonLimits& Limits)
{
    const FString NormalizedPath = NormalizeFilePath(Path);
    FSafeJsonLoadResult Result;
    Result.Path = NormalizedPath;

    if (!SafeFileIOPrivate::IsPathUsable(NormalizedPath))
    {
        Result.Status = ESafeFileIOStatus::InvalidPath;
        Result.Error = TEXT("The JSON path is empty or invalid");
        return Result;
    }

    const TSharedRef<FCriticalSection, ESPMode::ThreadSafe> PathLock =
        SafeFileIOPrivate::GetPathLock(NormalizedPath);
    FScopeLock ScopeLock(&PathLock.Get());

    Result = SafeFileIOPrivate::ReadJsonCandidateUnlocked(NormalizedPath, NormalizedPath, Limits);
    if (Result.IsSuccess() || !Limits.bAllowBackupRecovery)
    {
        return Result;
    }

    const FString BackupPath = SafeFileIOPrivate::GetBackupPath(NormalizedPath);
    FSafeJsonLoadResult BackupResult =
        SafeFileIOPrivate::ReadJsonCandidateUnlocked(BackupPath, NormalizedPath, Limits);
    if (BackupResult.IsSuccess())
    {
        BackupResult.Status = ESafeFileIOStatus::RecoveredFromBackup;
        BackupResult.bRecoveredFromBackup = true;
        BackupResult.Error = FString::Printf(
            TEXT("Primary JSON was unavailable or invalid; recovered from %s"),
            *BackupPath);
        return BackupResult;
    }

    // A hard termination can occur after a verified temporary file is flushed but before its
    // final rename. Recover the newest valid transaction rather than discarding the only good copy.
    for (const FString& TemporaryPath : SafeFileIOPrivate::FindTemporaryCandidatesUnlocked(NormalizedPath))
    {
        FSafeJsonLoadResult TemporaryResult =
            SafeFileIOPrivate::ReadJsonCandidateUnlocked(TemporaryPath, NormalizedPath, Limits);
        if (!TemporaryResult.IsSuccess())
        {
            continue;
        }

        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        if (PlatformFile.FileExists(*NormalizedPath))
        {
            PlatformFile.DeleteFile(*NormalizedPath);
        }
        if (PlatformFile.MoveFile(*NormalizedPath, *TemporaryPath))
        {
            TemporaryResult.Error = FString::Printf(
                TEXT("Recovered and promoted interrupted transaction %s"),
                *TemporaryPath);
        }
        else
        {
            TemporaryResult.Error = FString::Printf(
                TEXT("Recovered interrupted transaction %s in memory; promotion failed"),
                *TemporaryPath);
        }
        TemporaryResult.Status = ESafeFileIOStatus::RecoveredFromBackup;
        TemporaryResult.bRecoveredFromBackup = true;
        return TemporaryResult;
    }

    Result.Error = FString::Printf(
        TEXT("Primary load failed (%s); backup load failed (%s); no valid transaction file was found"),
        *Result.Error,
        *BackupResult.Error);
    return Result;
}

void FSafeFileIO::LoadJsonAsync(
    const FString& Path,
    FJsonLoadCallback Callback,
    const FSafeJsonLimits& Limits)
{
    if (SafeFileIOPrivate::IsShuttingDown())
    {
        FSafeJsonLoadResult Result;
        Result.Status = ESafeFileIOStatus::ShuttingDown;
        Result.Path = Path;
        Result.Error = TEXT("The file service is shutting down");
        SafeFileIOPrivate::DispatchJsonCallback(MoveTemp(Callback), MoveTemp(Result));
        return;
    }

    const TSharedRef<SafeFileIOPrivate::FTrackedOperation, ESPMode::ThreadSafe> TrackedOperation =
        MakeShared<SafeFileIOPrivate::FTrackedOperation, ESPMode::ThreadSafe>();
    Async(EAsyncExecution::ThreadPool,
        [Path, Limits, TrackedOperation, Callback = MoveTemp(Callback)]() mutable
        {
            FSafeJsonLoadResult Result = FSafeFileIO::LoadJsonBlocking(Path, Limits);
            SafeFileIOPrivate::DispatchJsonCallback(
                MoveTemp(Callback), MoveTemp(Result), TrackedOperation);
        });
}

FSafeFileWriteResult FSafeFileIO::SaveJsonBlocking(
    const TSharedRef<FJsonObject>& JsonObject,
    const FString& Path,
    const int64 MaxOutputBytes)
{
    FSafeFileWriteResult Result;
    const FString NormalizedPath = NormalizeFilePath(Path);
    Result.Path = NormalizedPath;

    if (!SafeFileIOPrivate::IsPathUsable(NormalizedPath) || MaxOutputBytes < 0)
    {
        Result.Status = ESafeFileIOStatus::InvalidPath;
        Result.Error = TEXT("The JSON path or maximum output size is invalid");
        return Result;
    }

    FString ValidationError;
    if (!SafeFileIOPrivate::ValidateJsonObjectForSave(JsonObject, MaxOutputBytes, ValidationError))
    {
        Result.Status = ESafeFileIOStatus::ValidationFailed;
        Result.Error = MoveTemp(ValidationError);
        return Result;
    }

    FString OutputString;
    TSharedRef<TJsonWriter<TCHAR>> Writer = TJsonWriterFactory<TCHAR>::Create(&OutputString);
    if (!FJsonSerializer::Serialize(JsonObject, Writer))
    {
        Result.Status = ESafeFileIOStatus::SerializeFailed;
        Result.Error = TEXT("The JSON DOM could not be serialized");
        return Result;
    }

    FTCHARToUTF8 Utf8(*OutputString);
    TArray<uint8> Bytes;
    if (Utf8.Length() > 0)
    {
        Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    }

    const uint64 WriteSequence = SafeFileIOPrivate::ReserveWriteSequence(NormalizedPath);
    const TSharedRef<FCriticalSection, ESPMode::ThreadSafe> PathLock =
        SafeFileIOPrivate::GetPathLock(NormalizedPath);
    FScopeLock ScopeLock(&PathLock.Get());
    return SafeFileIOPrivate::CommitBytesUnlocked(Bytes, NormalizedPath, MaxOutputBytes, WriteSequence);
}

FSafeFileWriteResult FSafeFileIO::CreateJsonIfMissingBlocking(
    const TSharedRef<FJsonObject>& JsonObject,
    const FString& Path,
    const int64 MaxOutputBytes)
{
    FSafeFileWriteResult Result;
    const FString NormalizedPath = NormalizeFilePath(Path);
    Result.Path = NormalizedPath;

    if (!SafeFileIOPrivate::IsPathUsable(NormalizedPath) || MaxOutputBytes < 0)
    {
        Result.Status = ESafeFileIOStatus::InvalidPath;
        Result.Error = TEXT("The JSON template path or maximum output size is invalid");
        return Result;
    }

    FString ValidationError;
    if (!SafeFileIOPrivate::ValidateJsonObjectForSave(JsonObject, MaxOutputBytes, ValidationError))
    {
        Result.Status = ESafeFileIOStatus::ValidationFailed;
        Result.Error = MoveTemp(ValidationError);
        return Result;
    }

    FString OutputString;
    TSharedRef<TJsonWriter<TCHAR>> Writer = TJsonWriterFactory<TCHAR>::Create(&OutputString);
    if (!FJsonSerializer::Serialize(JsonObject, Writer))
    {
        Result.Status = ESafeFileIOStatus::SerializeFailed;
        Result.Error = TEXT("The JSON template DOM could not be serialized");
        return Result;
    }

    FTCHARToUTF8 Utf8(*OutputString);
    TArray<uint8> Bytes;
    if (Utf8.Length() > 0)
    {
        Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    }

    const uint64 WriteSequence = SafeFileIOPrivate::ReserveWriteSequence(NormalizedPath);
    const TSharedRef<FCriticalSection, ESPMode::ThreadSafe> PathLock =
        SafeFileIOPrivate::GetPathLock(NormalizedPath);
    FScopeLock ScopeLock(&PathLock.Get());

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (PlatformFile.FileExists(*NormalizedPath))
    {
        Result.Status = ESafeFileIOStatus::Success;
        Result.BytesWritten = 0;
        return Result;
    }

    // CommitBytesUnlocked only rotates a backup when a target existed before this transaction.
    // The existence check is protected by the same per-path lock, so template creation never
    // overwrites external JSON and therefore never creates a JSON .bak generation.
    return SafeFileIOPrivate::CommitBytesUnlocked(
        Bytes, NormalizedPath, MaxOutputBytes, WriteSequence);
}

void FSafeFileIO::SaveJsonAsync(
    const TSharedRef<FJsonObject>& JsonObject,
    const FString& Path,
    FWriteCallback Callback,
    const int64 MaxOutputBytes)
{
    if (SafeFileIOPrivate::IsShuttingDown())
    {
        FSafeFileWriteResult Result;
        Result.Status = ESafeFileIOStatus::ShuttingDown;
        Result.Path = Path;
        Result.Error = TEXT("The file service is shutting down");
        SafeFileIOPrivate::DispatchWriteCallback(MoveTemp(Callback), MoveTemp(Result));
        return;
    }

    const FString NormalizedPath = NormalizeFilePath(Path);
    if (!SafeFileIOPrivate::IsPathUsable(NormalizedPath) || MaxOutputBytes < 0)
    {
        FSafeFileWriteResult Result;
        Result.Status = ESafeFileIOStatus::InvalidPath;
        Result.Path = NormalizedPath;
        Result.Error = TEXT("The JSON path or maximum output size is invalid");
        SafeFileIOPrivate::DispatchWriteCallback(MoveTemp(Callback), MoveTemp(Result));
        return;
    }
    const uint64 WriteSequence = SafeFileIOPrivate::ReserveWriteSequence(NormalizedPath);

    // Call sites pass a newly created immutable JSON snapshot. Keeping the shared reference alive
    // lets serialization happen off the game thread without copying a potentially large DOM.
    const TSharedRef<SafeFileIOPrivate::FTrackedOperation, ESPMode::ThreadSafe> TrackedOperation =
        MakeShared<SafeFileIOPrivate::FTrackedOperation, ESPMode::ThreadSafe>();
    Async(EAsyncExecution::ThreadPool,
        [JsonObject, NormalizedPath, MaxOutputBytes, WriteSequence, TrackedOperation,
            Callback = MoveTemp(Callback)]() mutable
        {

            FSafeFileWriteResult Result;
            Result.Path = NormalizedPath;
            FString ValidationError;
            if (!SafeFileIOPrivate::IsPathUsable(NormalizedPath) || MaxOutputBytes < 0)
            {
                Result.Status = ESafeFileIOStatus::InvalidPath;
                Result.Error = TEXT("The JSON path or maximum output size is invalid");
            }
            else if (!SafeFileIOPrivate::ValidateJsonObjectForSave(
                JsonObject, MaxOutputBytes, ValidationError))
            {
                Result.Status = ESafeFileIOStatus::ValidationFailed;
                Result.Error = MoveTemp(ValidationError);
            }
            else
            {
                FString OutputString;
                TSharedRef<TJsonWriter<TCHAR>> Writer = TJsonWriterFactory<TCHAR>::Create(&OutputString);
                if (!FJsonSerializer::Serialize(JsonObject, Writer))
                {
                    Result.Status = ESafeFileIOStatus::SerializeFailed;
                    Result.Error = TEXT("The JSON DOM could not be serialized");
                }
                else
                {
                    FTCHARToUTF8 Utf8(*OutputString);
                    TArray<uint8> Bytes;
                    if (Utf8.Length() > 0)
                    {
                        Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
                    }

                    const TSharedRef<FCriticalSection, ESPMode::ThreadSafe> PathLock =
                        SafeFileIOPrivate::GetPathLock(NormalizedPath);
                    FScopeLock ScopeLock(&PathLock.Get());
                    Result = SafeFileIOPrivate::CommitBytesUnlocked(
                        Bytes, NormalizedPath, MaxOutputBytes, WriteSequence);
                }
            }
            SafeFileIOPrivate::DispatchWriteCallback(
                MoveTemp(Callback), MoveTemp(Result), TrackedOperation);
        });
}

FSafeBinaryLoadResult FSafeFileIO::LoadBinaryBlocking(const FString& Path, const int64 MaxBytes)
{
    const FString NormalizedPath = NormalizeFilePath(Path);
    if (!SafeFileIOPrivate::IsPathUsable(NormalizedPath) || MaxBytes < 0)
    {
        FSafeBinaryLoadResult Result;
        Result.Status = ESafeFileIOStatus::InvalidPath;
        Result.Path = NormalizedPath;
        Result.Error = TEXT("The binary path or maximum byte count is invalid");
        return Result;
    }
    const TSharedRef<FCriticalSection, ESPMode::ThreadSafe> PathLock =
        SafeFileIOPrivate::GetPathLock(NormalizedPath);
    FScopeLock ScopeLock(&PathLock.Get());

    FSafeBinaryLoadResult Primary = SafeFileIOPrivate::ReadBytesUnlocked(NormalizedPath, MaxBytes);
    if (Primary.IsSuccess())
    {
        return Primary;
    }

    // Binary payloads cannot be semantically validated like JSON, so only the last fully committed
    // backup is eligible for recovery; uncommitted temporary bytes are never trusted.
    FSafeBinaryLoadResult Backup = SafeFileIOPrivate::ReadBytesUnlocked(
        SafeFileIOPrivate::GetBackupPath(NormalizedPath),
        MaxBytes);
    if (Backup.IsSuccess())
    {
        Backup.Path = NormalizedPath;
        Backup.Status = ESafeFileIOStatus::RecoveredFromBackup;
        Backup.Error = FString::Printf(TEXT("Recovered binary payload from backup after primary failure: %s"),
            *Primary.Error);
        return Backup;
    }
    return Primary;
}

void FSafeFileIO::LoadBinaryAsync(
    const FString& Path,
    const int64 MaxBytes,
    FBinaryLoadCallback Callback)
{
    if (SafeFileIOPrivate::IsShuttingDown())
    {
        FSafeBinaryLoadResult Result;
        Result.Status = ESafeFileIOStatus::ShuttingDown;
        Result.Path = Path;
        Result.Error = TEXT("The file service is shutting down");
        SafeFileIOPrivate::DispatchBinaryCallback(MoveTemp(Callback), MoveTemp(Result));
        return;
    }

    const TSharedRef<SafeFileIOPrivate::FTrackedOperation, ESPMode::ThreadSafe> TrackedOperation =
        MakeShared<SafeFileIOPrivate::FTrackedOperation, ESPMode::ThreadSafe>();
    Async(EAsyncExecution::ThreadPool,
        [Path, MaxBytes, TrackedOperation, Callback = MoveTemp(Callback)]() mutable
        {
            FSafeBinaryLoadResult Result = FSafeFileIO::LoadBinaryBlocking(Path, MaxBytes);
            SafeFileIOPrivate::DispatchBinaryCallback(
                MoveTemp(Callback), MoveTemp(Result), TrackedOperation);
        });
}

FSafeFileWriteResult FSafeFileIO::SaveBinaryBlocking(
    const TArray<uint8>& Data,
    const FString& Path,
    const int64 MaxOutputBytes)
{
    const FString NormalizedPath = NormalizeFilePath(Path);
    if (!SafeFileIOPrivate::IsPathUsable(NormalizedPath) || MaxOutputBytes < 0)
    {
        FSafeFileWriteResult Result;
        Result.Status = ESafeFileIOStatus::InvalidPath;
        Result.Path = NormalizedPath;
        Result.Error = TEXT("The binary path or maximum output size is invalid");
        return Result;
    }
    const uint64 WriteSequence = SafeFileIOPrivate::ReserveWriteSequence(NormalizedPath);
    const TSharedRef<FCriticalSection, ESPMode::ThreadSafe> PathLock =
        SafeFileIOPrivate::GetPathLock(NormalizedPath);
    FScopeLock ScopeLock(&PathLock.Get());
    return SafeFileIOPrivate::CommitBytesUnlocked(Data, NormalizedPath, MaxOutputBytes, WriteSequence);
}

void FSafeFileIO::SaveBinaryAsync(
    const TArray<uint8>& Data,
    const FString& Path,
    const int64 MaxOutputBytes,
    FWriteCallback Callback)
{
    if (SafeFileIOPrivate::IsShuttingDown())
    {
        FSafeFileWriteResult Result;
        Result.Status = ESafeFileIOStatus::ShuttingDown;
        Result.Path = Path;
        Result.Error = TEXT("The file service is shutting down");
        SafeFileIOPrivate::DispatchWriteCallback(MoveTemp(Callback), MoveTemp(Result));
        return;
    }

    const FString NormalizedPath = NormalizeFilePath(Path);
    if (!SafeFileIOPrivate::IsPathUsable(NormalizedPath) || MaxOutputBytes < 0)
    {
        FSafeFileWriteResult Result;
        Result.Status = ESafeFileIOStatus::InvalidPath;
        Result.Path = NormalizedPath;
        Result.Error = TEXT("The binary path or maximum output size is invalid");
        SafeFileIOPrivate::DispatchWriteCallback(MoveTemp(Callback), MoveTemp(Result));
        return;
    }
    const uint64 WriteSequence = SafeFileIOPrivate::ReserveWriteSequence(NormalizedPath);
    const TSharedRef<SafeFileIOPrivate::FTrackedOperation, ESPMode::ThreadSafe> TrackedOperation =
        MakeShared<SafeFileIOPrivate::FTrackedOperation, ESPMode::ThreadSafe>();
    Async(EAsyncExecution::ThreadPool,
        [Data, NormalizedPath, MaxOutputBytes, WriteSequence, TrackedOperation,
            Callback = MoveTemp(Callback)]() mutable
        {
            const TSharedRef<FCriticalSection, ESPMode::ThreadSafe> PathLock =
                SafeFileIOPrivate::GetPathLock(NormalizedPath);
            FScopeLock ScopeLock(&PathLock.Get());
            FSafeFileWriteResult Result = SafeFileIOPrivate::CommitBytesUnlocked(
                Data, NormalizedPath, MaxOutputBytes, WriteSequence);
            SafeFileIOPrivate::DispatchWriteCallback(
                MoveTemp(Callback), MoveTemp(Result), TrackedOperation);
        });
}

void FSafeFileIO::AppendTextAsync(
    const FString& Text,
    const FString& Path,
    FWriteCallback Callback)
{
    if (SafeFileIOPrivate::IsShuttingDown())
    {
        FSafeFileWriteResult Result;
        Result.Status = ESafeFileIOStatus::ShuttingDown;
        Result.Path = Path;
        Result.Error = TEXT("The file service is shutting down");
        SafeFileIOPrivate::DispatchWriteCallback(MoveTemp(Callback), MoveTemp(Result));
        return;
    }

    constexpr int32 MaxSingleAppendCharacters = 1024 * 1024;
    constexpr int64 MaxQueuedAppendCharacters = 16ll * 1024ll * 1024ll;
    const FString NormalizedPath = NormalizeFilePath(Path);
    if (!SafeFileIOPrivate::IsPathUsable(NormalizedPath))
    {
        FSafeFileWriteResult Result;
        Result.Status = ESafeFileIOStatus::InvalidPath;
        Result.Path = NormalizedPath;
        Result.Error = TEXT("The append path is invalid");
        SafeFileIOPrivate::DispatchWriteCallback(MoveTemp(Callback), MoveTemp(Result));
        return;
    }
    if (Text.Len() > MaxSingleAppendCharacters)
    {
        FSafeFileWriteResult Result;
        Result.Status = ESafeFileIOStatus::TooLarge;
        Result.Path = NormalizedPath;
        Result.Error = TEXT("One append exceeds the one-megacharacter safety limit");
        SafeFileIOPrivate::DispatchWriteCallback(MoveTemp(Callback), MoveTemp(Result));
        return;
    }

    bool bStartWorker = false;
    bool bQueueFull = false;
    {
        SafeFileIOPrivate::FState& State = SafeFileIOPrivate::GetState();
        FScopeLock StateScope(&State.StateLock);
        if (State.PendingAppendCharacters + Text.Len() > MaxQueuedAppendCharacters)
        {
            bQueueFull = true;
        }
        else
        {
            SafeFileIOPrivate::FQueuedAppendRequest Request;
            Request.Text = Text;
            Request.Callback = MoveTemp(Callback);
            State.PendingAppends.FindOrAdd(NormalizedPath).Add(MoveTemp(Request));
            State.PendingAppendCharacters += Text.Len();
            bStartWorker = !State.ActiveAppendWorkers.Contains(NormalizedPath);
            if (bStartWorker)
            {
                State.ActiveAppendWorkers.Add(NormalizedPath);
            }
        }
    }

    if (bQueueFull)
    {
        FSafeFileWriteResult Result;
        Result.Status = ESafeFileIOStatus::TooLarge;
        Result.Path = NormalizedPath;
        Result.Error = TEXT("The diagnostic append queue reached its bounded memory limit");
        SafeFileIOPrivate::DispatchWriteCallback(MoveTemp(Callback), MoveTemp(Result));
        return;
    }
    if (!bStartWorker)
    {
        return;
    }

    const TSharedRef<SafeFileIOPrivate::FTrackedOperation, ESPMode::ThreadSafe> TrackedOperation =
        MakeShared<SafeFileIOPrivate::FTrackedOperation, ESPMode::ThreadSafe>();
    Async(EAsyncExecution::ThreadPool,
        [NormalizedPath, TrackedOperation]() mutable
        {
            for (;;)
            {
                TArray<SafeFileIOPrivate::FQueuedAppendRequest> Requests;
                {
                    SafeFileIOPrivate::FState& State = SafeFileIOPrivate::GetState();
                    FScopeLock StateScope(&State.StateLock);
                    TArray<SafeFileIOPrivate::FQueuedAppendRequest>* Queued =
                        State.PendingAppends.Find(NormalizedPath);
                    if (!Queued || Queued->IsEmpty())
                    {
                        State.PendingAppends.Remove(NormalizedPath);
                        State.ActiveAppendWorkers.Remove(NormalizedPath);
                        break;
                    }

                    Requests = MoveTemp(*Queued);
                    State.PendingAppends.Remove(NormalizedPath);
                    for (const SafeFileIOPrivate::FQueuedAppendRequest& Request : Requests)
                    {
                        State.PendingAppendCharacters =
                            FMath::Max<int64>(0, State.PendingAppendCharacters - Request.Text.Len());
                    }
                }

                int64 BatchCharacters = 0;
                for (const SafeFileIOPrivate::FQueuedAppendRequest& Request : Requests)
                {
                    BatchCharacters += Request.Text.Len();
                }
                FString Batch;
                Batch.Reserve(static_cast<int32>(FMath::Min<int64>(BatchCharacters, MAX_int32)));
                for (const SafeFileIOPrivate::FQueuedAppendRequest& Request : Requests)
                {
                    Batch.Append(Request.Text);
                }

                FSafeFileWriteResult Result;
                Result.Path = NormalizedPath;
                const TSharedRef<FCriticalSection, ESPMode::ThreadSafe> PathLock =
                    SafeFileIOPrivate::GetPathLock(Result.Path);
                {
                    FScopeLock ScopeLock(&PathLock.Get());

                    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
                    const FString Directory = FPaths::GetPath(Result.Path);
                    if (!Directory.IsEmpty())
                    {
                        PlatformFile.CreateDirectoryTree(*Directory);
                    }

                    FTCHARToUTF8 Utf8(*Batch);
                    TUniquePtr<IFileHandle> Handle(PlatformFile.OpenWrite(*Result.Path, true, false));
                    if (Handle.IsValid() &&
                        (Utf8.Length() == 0 || Handle->Write(
                            reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length())) &&
                        Handle->Flush(false))
                    {
                        Result.Status = ESafeFileIOStatus::Success;
                        Result.BytesWritten = Utf8.Length();
                    }
                    else
                    {
                        Result.Status = ESafeFileIOStatus::WriteFailed;
                        Result.Error = TEXT("The batched append operation failed");
                    }
                }

                for (SafeFileIOPrivate::FQueuedAppendRequest& Request : Requests)
                {
                    SafeFileIOPrivate::DispatchWriteCallback(
                        MoveTemp(Request.Callback), Result, TrackedOperation);
                }
            }
        });
}

void FSafeFileIO::DeleteFilesAsync(
    const TArray<FString>& Paths,
    FWriteCallback Callback)
{
    if (SafeFileIOPrivate::IsShuttingDown())
    {
        FSafeFileWriteResult Result;
        Result.Status = ESafeFileIOStatus::ShuttingDown;
        Result.Error = TEXT("The file service is shutting down");
        SafeFileIOPrivate::DispatchWriteCallback(MoveTemp(Callback), MoveTemp(Result));
        return;
    }

    TArray<FString> NormalizedPaths;
    NormalizedPaths.Reserve(FMath::Min(Paths.Num(), 4096));
    for (int32 Index = 0; Index < Paths.Num() && Index < 4096; ++Index)
    {
        const FString NormalizedPath = NormalizeFilePath(Paths[Index]);
        if (SafeFileIOPrivate::IsPathUsable(NormalizedPath))
        {
            NormalizedPaths.AddUnique(NormalizedPath);
        }
    }
    NormalizedPaths.Sort();

    const TSharedRef<SafeFileIOPrivate::FTrackedOperation, ESPMode::ThreadSafe> TrackedOperation =
        MakeShared<SafeFileIOPrivate::FTrackedOperation, ESPMode::ThreadSafe>();
    Async(EAsyncExecution::ThreadPool,
        [NormalizedPaths = MoveTemp(NormalizedPaths), TrackedOperation,
            Callback = MoveTemp(Callback)]() mutable
        {
            FSafeFileWriteResult Result;
            Result.Status = ESafeFileIOStatus::Success;
            Result.Path = NormalizedPaths.IsEmpty() ? FString() : NormalizedPaths[0];
            IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

            for (const FString& Path : NormalizedPaths)
            {
                const TSharedRef<FCriticalSection, ESPMode::ThreadSafe> PathLock =
                    SafeFileIOPrivate::GetPathLock(Path);
                FScopeLock ScopeLock(&PathLock.Get());
                if (PlatformFile.FileExists(*Path) && !PlatformFile.DeleteFile(*Path))
                {
                    Result.Status = ESafeFileIOStatus::WriteFailed;
                    Result.Error = FString::Printf(TEXT("Could not delete file: %s"), *Path);
                    break;
                }
            }

            SafeFileIOPrivate::DispatchWriteCallback(
                MoveTemp(Callback), MoveTemp(Result), TrackedOperation);
        });
}

void FSafeFileIO::BeginShutdown()
{
    if (SafeFileIOPrivate::GetState().ShutdownFlag.GetValue() == 0)
    {
        SafeFileIOPrivate::GetState().ShutdownFlag.Increment();
    }
}

bool FSafeFileIO::FlushPendingOperations(const double TimeoutSeconds)
{
    const double StartSeconds = FPlatformTime::Seconds();
    while (GetPendingOperationCount() > 0)
    {
        if (TimeoutSeconds >= 0.0 && FPlatformTime::Seconds() - StartSeconds >= TimeoutSeconds)
        {
            return false;
        }

        // Module shutdown normally runs on the game thread. Pump queued completions so their
        // lifetime trackers can release before DLL code is unloaded.
        if (IsInGameThread())
        {
            FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        }
        FPlatformProcess::SleepNoStats(0.005f);
    }
    return true;
}

int32 FSafeFileIO::GetPendingOperationCount()
{
    return SafeFileIOPrivate::GetState().PendingOperations.GetValue();
}

void FSafeFileIO::CleanupStaleTemporaryFiles(const FString& RootDirectory, const double OlderThanHours)
{
    const FString NormalizedRoot = NormalizeFilePath(RootDirectory);
    if (NormalizedRoot.IsEmpty() || !FPaths::DirectoryExists(NormalizedRoot))
    {
        return;
    }

    TArray<FString> TemporaryFiles;
    IFileManager::Get().FindFilesRecursive(
        TemporaryFiles,
        *NormalizedRoot,
        TEXT("*.tmp.*"),
        true,
        false,
        false);

    const FDateTime Cutoff = FDateTime::UtcNow() - FTimespan::FromHours(FMath::Max(0.0, OlderThanHours));
    for (const FString& TemporaryFile : TemporaryFiles)
    {
        const FDateTime Timestamp = IFileManager::Get().GetTimeStamp(*TemporaryFile);
        if (Timestamp > Cutoff)
        {
            continue;
        }

        // Preserve an orphan transaction when neither the primary nor backup exists. It may be
        // the only fully flushed copy left after a power loss during the first save.
        const int32 TmpMarker = TemporaryFile.Find(TEXT(".tmp."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
        const FString TargetPath = TmpMarker == INDEX_NONE ? FString() : TemporaryFile.Left(TmpMarker);
        if (TargetPath.IsEmpty() ||
            (!IFileManager::Get().FileExists(*TargetPath) &&
             !IFileManager::Get().FileExists(*(TargetPath + TEXT(".bak")))))
        {
            continue;
        }
        IFileManager::Get().Delete(*TemporaryFile, false, true, true);
    }
}
