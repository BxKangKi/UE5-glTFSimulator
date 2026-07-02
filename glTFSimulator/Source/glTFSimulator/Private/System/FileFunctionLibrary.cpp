// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "System/FileFunctionLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Async/Async.h"
#include "Misc/ScopeLock.h"
#include "HAL/PlatformFilemanager.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "System/MacroLibrary.h"


#pragma region File IO
// Static member definitions.
FCriticalSection UFileFunctionLibrary::FileWriteCriticalSection;

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

TArray<FString> UFileFunctionLibrary::GetFileNamesWithExtension(const FString &Directory, const FString &Extension)
{
    TArray<FString> FoundFiles;
    // Build a search pattern that includes the extension, for example "*.png".
    FString FilePattern = FString::Printf(TEXT("*.%s"), *Extension);
    // Search files with IFileManager, optionally including child folders recursively.
    IFileManager &FileManager = IFileManager::Get();
    // Convert the directory path to an absolute path when needed.
    FString AbsoluteDirectory = FPaths::ConvertRelativePathToFull(Directory);
    FileManager.FindFilesRecursive(FoundFiles, *AbsoluteDirectory, *FilePattern, true, false, false);
    return FoundFiles;
}

bool UFileFunctionLibrary::ToBinary(FBufferArchive Ar, const FString &FilePath)
{
    return FFileHelper::SaveArrayToFile(Ar, *FilePath);
}

void UFileFunctionLibrary::ToBinaryAsync(FBufferArchive Ar, const FString &FilePath)
{
    Async(EAsyncExecution::ThreadPool, [Ar, FilePath]()
          {
              FScopeLock Lock(&FileWriteCriticalSection);
              bool bSuccess = ToBinary(Ar, FilePath);
              if (bSuccess)
              {
#if WITH_EDITOR
                  UE_LOG(LogTemp, Log, TEXT("Successfully saved Binary to %s"), *FilePath);
#endif
              }
              else
              {
                  UE_LOG(LogTemp, Error, TEXT("Failed to save Binary to : %s"), *FilePath);
              } });
}

bool UFileFunctionLibrary::FromBinary(TArray<uint8> &FileData, const FString &FilePath)
{
    if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to load binary file : %s"), *FilePath);
        return false;
    }
    return true;
}

bool UFileFunctionLibrary::AppendLineToFile(const FString &Line, const FString &FilePath)
{
    // Append exactly one log-style line and create the parent directory when needed.
    const FString TextToAppend = Line.EndsWith(LINE_TERMINATOR) ? Line : Line + LINE_TERMINATOR;
    return AppendStringToFileInternal(TextToAppend, FilePath);
}

void UFileFunctionLibrary::AppendLineToFileAsync(const FString &Line, const FString &FilePath)
{
    Async(EAsyncExecution::ThreadPool, [Line, FilePath]()
          {
              FScopeLock Lock(&FileWriteCriticalSection);
              const FString TextToAppend = Line.EndsWith(LINE_TERMINATOR) ? Line : Line + LINE_TERMINATOR;
              bool bSuccess = AppendStringToFileInternal(TextToAppend, FilePath);
#if WITH_EDITOR
              if (bSuccess)
              {
                  UE_LOG(LogTemp, Log, TEXT("Successfully appended line to %s"), *FilePath);
              }
              else
              {
                  UE_LOG(LogTemp, Error, TEXT("Failed to append line to %s"), *FilePath);
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
    Async(EAsyncExecution::ThreadPool, [Category, Message]()
    {
        FScopeLock Lock(&FileWriteCriticalSection);
        const bool bSuccess = WriteSimulatorLog(Category, Message);
#if WITH_EDITOR
        if (!bSuccess)
        {
            UE_LOG(LogTemp, Error, TEXT("Failed to write simulator log. Category=%s Message=%s"), *Category, *Message);
        }
#endif
    });
}

// CoreSystem/Source/CoreSystem/Private/FileFunctionLibrary.cpp
bool UFileFunctionLibrary::GetSubFolders(const FString& ParentFolderPath, TArray<FString>& OutSubFolders)
{
    OutSubFolders.Empty();
    
    IFileManager& FileManager = IFileManager::Get();
    
    // UE5.7 path: use FindFilesRecursive or DirectoryExists plus FindFiles.
    TArray<FString> AllItems;
    FileManager.FindFiles(AllItems, *(ParentFolderPath + TEXT("*")), true, true);

    // Keep only directories.
    for (const FString& Item : AllItems)
    {
        FString FullPath = ParentFolderPath + Item;
        if (FPaths::DirectoryExists(FullPath))
        {
            OutSubFolders.Add(Item);
        }
    }
    
    return !OutSubFolders.IsEmpty();
}


#pragma region Json File

void UFileFunctionLibrary::ToJsonAsync(TSharedRef<FJsonObject> Json, const FString &Path)
{
    Async(EAsyncExecution::ThreadPool, [Json, Path]()
          {
            FScopeLock Lock(&FileWriteCriticalSection);
            bool bSuccess = ToJson(Json, Path);
            if (bSuccess)
            {
#if WITH_EDITOR
                UE_LOG(LogTemp, Log, TEXT("Successfully saved JSON to %s"), *Path);
#endif
            }
            else
            {
                UE_LOG(LogTemp, Error, TEXT("Failed to saved JSON to %s"), *Path);
            } });
}

bool UFileFunctionLibrary::ToJson(TSharedRef<FJsonObject> Json, const FString &FilePath)
{
    FString OutputString;
    if (!FJsonSerializer::Serialize(Json, TJsonWriterFactory<TCHAR>::Create(&OutputString)))
    {
        UE_LOG(LogTemp, Error, TEXT("JSON serialization failed"));
        return false;
    }

    // Extract the folder path and create it when missing.
    GenerateDirectory(FilePath);

    if (!FFileHelper::SaveStringToFile(OutputString, *FilePath))
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to save file: %s"), *FilePath);
        return false;
    }

    return true;
}

TSharedPtr<FJsonObject> UFileFunctionLibrary::FromJson(const FString &Path)
{
    FString FileContent;
    if (!FFileHelper::LoadFileToString(FileContent, *Path))
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to load file: %s"), *Path);
        return nullptr;
    }

    TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(FileContent);
    TSharedPtr<FJsonObject> JsonObject;
    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to deserialize JSON from file: %s"), *Path);
        return nullptr;
    }

    return JsonObject;
}

// Extracts a string value from a file path and key name.
// Stores the value in OutValue and returns true on success.
bool UFileFunctionLibrary::LoadJsonStringValue(
    const FString &JsonFilePath,
    const FString &KeyName,
    FString &OutValue)
{
    OutValue.Reset();

    // Check whether the file exists and read its contents.
    FString JsonRaw;
    if (!FFileHelper::LoadFileToString(JsonRaw, *JsonFilePath))
    {
        return false;
    }

    // Parse JSON.
    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonRaw);

    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
    {
        return false;
    }

    // Extract the key value.
    return JsonObject->TryGetStringField(*KeyName, OutValue);
}


#pragma region Private
// private functions

bool UFileFunctionLibrary::AppendStringToFileInternal(const FString &Line, const FString &FilePath)
{
    GenerateDirectory(FilePath);
    return FFileHelper::SaveStringToFile(Line, *FilePath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
}