// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file FileFunctionLibrary.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "System/FileFunctionLibrary.h"
#include "System/SafeFileIO.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "System/MacroLibrary.h"

namespace
{
    constexpr int64 MAX_SAFE_JSON_FILE_BYTES = 64ll * 1024ll * 1024ll;
}

#pragma region File IO
bool UFileFunctionLibrary::CheckFile(const FString &FilePath)
{
    if (!GenerateDirectory(FilePath) || !IFileManager::Get().FileExists(*FilePath))
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to check file: %s"), *FilePath);
        return false;
    }
    return true;
}

bool UFileFunctionLibrary::GenerateDirectory(const FString &FilePath)
{
    FString Directory = FPaths::GetPath(FilePath);
    if (!IFileManager::Get().DirectoryExists(*Directory))
    {
        if (!IFileManager::Get().MakeDirectory(*Directory, true))
        {
            UE_LOG(LogTemp, Error, TEXT("Failed to create directory %s"), *Directory);
            return false;
        }
    }
    return true;
}

FString UFileFunctionLibrary::GetPathWithoutExtension(const FString &Path)
{
    FString Directory = FPaths::GetPath(Path);
    FString BaseName = FPaths::GetBaseFilename(Path);
    return FPaths::Combine(Directory, BaseName);
}

bool UFileFunctionLibrary::AppendLineToFile(const FString &Line, const FString &FilePath)
{
    // Append exactly one log-style line and create the parent directory when needed.
    const FString TextToAppend = Line.EndsWith(LINE_TERMINATOR) ? Line : Line + LINE_TERMINATOR;
    return AppendStringToFileInternal(TextToAppend, FilePath);
}

void UFileFunctionLibrary::AppendLineToFileAsync(const FString &Line, const FString &FilePath)
{
    const FString TextToAppend = Line.EndsWith(LINE_TERMINATOR) ? Line : Line + LINE_TERMINATOR;
    FSafeFileIO::AppendTextAsync(
        TextToAppend,
        FilePath,
        [FilePath](FSafeFileWriteResult Result)
        {
#if WITH_EDITOR
            if (!Result.IsSuccess() && Result.Status != ESafeFileIOStatus::ShuttingDown)
            {
                UE_LOG(LogTemp, Error, TEXT("Failed to append line to %s: %s"), *FilePath, *Result.Error);
            }
#endif
        });
}

FString UFileFunctionLibrary::GetSimulatorLogFilePath()
{
    const FString LogDirectory = FPaths::Combine(DIRECTORY_USER, DIRECTORY_GAME, DIRECTORY_LOG);
    const FString LogFileName = FString::Printf(TEXT("log_%s.txt"), *FDateTime::Now().ToString(TEXT("%Y%m%d")));
    return FPaths::Combine(LogDirectory, LogFileName);
}

bool UFileFunctionLibrary::WriteSimulatorLog(const FString& Category, const FString& Message)
{
    const FString SafeCategory = Category.IsEmpty() ? TEXT("General") : Category;
    const FString Line = FString::Printf(TEXT("[%s][%s] %s"), *FDateTime::Now().ToString(), *SafeCategory, *Message);
    return AppendLineToFile(Line, GetSimulatorLogFilePath());
}

void UFileFunctionLibrary::WriteSimulatorLogAsync(const FString& Category, const FString& Message)
{
    const FString SafeCategory = Category.IsEmpty() ? TEXT("General") : Category;
    const FString Line = FString::Printf(
        TEXT("[%s][%s] %s%s"),
        *FDateTime::Now().ToString(),
        *SafeCategory,
        *Message,
        LINE_TERMINATOR);
    const FString SimulatorLogPath = GetSimulatorLogFilePath();

    FSafeFileIO::AppendTextAsync(
        Line,
        SimulatorLogPath,
        [SafeCategory](FSafeFileWriteResult Result)
        {
#if WITH_EDITOR
        if (!Result.IsSuccess() && Result.Status != ESafeFileIOStatus::ShuttingDown)
        {
            UE_LOG(
                LogTemp,
                Error,
                TEXT("Failed to write simulator log. Category=%s Error=%s"),
                *SafeCategory,
                *Result.Error);
        }
#endif
        });
}

// CoreSystem/Source/CoreSystem/Private/FileFunctionLibrary.cpp
bool UFileFunctionLibrary::GetSubFolders(const FString& ParentFolderPath, TArray<FString>& OutSubFolders)
{
    OutSubFolders.Empty();

    IFileManager& FileManager = IFileManager::Get();

    TArray<FString> AllItems;
    // Ask the platform layer for directories only; enumerating every .gwd/.dat/config file
    // merely to discard it made world-selection refresh scale with all deployment artifacts.
    FileManager.FindFiles(
        AllItems,
        *FPaths::Combine(ParentFolderPath, TEXT("*")),
        false,
        true);

    // Keep only directories.
    for (const FString& Item : AllItems)
    {
        const FString FullPath = FPaths::Combine(ParentFolderPath, Item);
        if (FPaths::DirectoryExists(FullPath))
        {
            OutSubFolders.Add(Item);
        }
    }

    OutSubFolders.Sort([](const FString& A, const FString& B)
    {
        return A.Compare(B, ESearchCase::IgnoreCase) < 0;
    });
    return !OutSubFolders.IsEmpty();
}


#pragma region Json File

void UFileFunctionLibrary::ToJsonAsync(TSharedRef<FJsonObject> Json, const FString &Path)
{
    FSafeFileIO::SaveJsonAsync(
        Json,
        Path,
        [Path](FSafeFileWriteResult Result)
        {
            if (Result.IsSuccess())
            {
#if WITH_EDITOR
                UE_LOG(LogTemp, Verbose, TEXT("Successfully saved JSON to %s"), *Path);
#endif
            }
            else if (Result.Status != ESafeFileIOStatus::ShuttingDown &&
                Result.Status != ESafeFileIOStatus::Superseded)
            {
                UE_LOG(LogTemp, Error, TEXT("Failed to save JSON to %s: %s"), *Path, *Result.Error);
            }
        },
        MAX_SAFE_JSON_FILE_BYTES);
}

bool UFileFunctionLibrary::ToJson(TSharedRef<FJsonObject> Json, const FString &FilePath)
{
    const FSafeFileWriteResult Result =
        FSafeFileIO::SaveJsonBlocking(Json, FilePath, MAX_SAFE_JSON_FILE_BYTES);
    if (!Result.IsSuccess())
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to save JSON to %s: %s"), *FilePath, *Result.Error);
        return false;
    }
    return true;
}

TSharedPtr<FJsonObject> UFileFunctionLibrary::FromJson(const FString &Path)
{
    FSafeJsonLimits Limits;
    Limits.MaxFileBytes = MAX_SAFE_JSON_FILE_BYTES;
    const FSafeJsonLoadResult Result = FSafeFileIO::LoadJsonBlocking(Path, Limits);
    if (Result.IsSuccess())
    {
        if (Result.bRecoveredFromBackup)
        {
            UE_LOG(LogTemp, Warning, TEXT("Recovered JSON from backup: %s"), *Path);
        }
        return Result.JsonObject;
    }

    if (Result.Status == ESafeFileIOStatus::Missing)
    {
        // Several callers intentionally probe optional save files before creating defaults.
        UE_LOG(LogTemp, Verbose, TEXT("Optional JSON file does not exist: %s"), *Path);
        return nullptr;
    }

    UE_LOG(LogTemp, Error, TEXT("Failed to load JSON from %s: %s"), *Path, *Result.Error);
    return nullptr;
}

// Extracts a string value from a file path and key name.
// Stores the value in OutValue and returns true on success.
bool UFileFunctionLibrary::LoadJsonStringValue(
    const FString &JsonFilePath,
    const FString &KeyName,
    FString &OutValue)
{
    OutValue.Reset();

    // Route menu discovery through the same bounded parser as every other external JSON read.
    // This closes the size-check/read race and applies depth, value-count and string-length limits
    // before a malformed config can allocate an unbounded DOM on the game thread.
    FSafeJsonLimits Limits;
    Limits.MaxFileBytes = MAX_SAFE_JSON_FILE_BYTES;
    Limits.bAllowBackupRecovery = false;
    const FSafeJsonLoadResult Result =
        FSafeFileIO::LoadJsonBlocking(JsonFilePath, Limits);
    return Result.IsSuccess()
        && Result.JsonObject->TryGetStringField(*KeyName, OutValue);
}


#pragma region Private
// private functions

bool UFileFunctionLibrary::AppendStringToFileInternal(const FString &Line, const FString &FilePath)
{
    GenerateDirectory(FilePath);
    return FFileHelper::SaveStringToFile(Line, *FilePath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
}
