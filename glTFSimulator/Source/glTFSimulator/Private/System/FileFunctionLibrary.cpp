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


#pragma region File IO
// static 멤버 변수 정의
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
    // 확장자가 포함된 검색 패턴 생성 (예: "*.png")
    FString FilePattern = FString::Printf(TEXT("*.%s"), *Extension);
    // IFileManager로 파일 검색 (재귀 옵션으로 하위 폴더 포함 가능)
    IFileManager &FileManager = IFileManager::Get();
    // 디렉터리 경로가 절대 경로가 아니면 절대 경로로 변환
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
    // 다시 파일에 저장 (덮어쓰기)
    return AppendStringToFileInternal(Line, *FilePath);
}

void UFileFunctionLibrary::AppendLineToFileAsync(const FString &Line, const FString &FilePath)
{
    Async(EAsyncExecution::ThreadPool, [Line, FilePath]()
          {
              FScopeLock Lock(&FileWriteCriticalSection);
              bool bSuccess = AppendStringToFileInternal(Line + LINE_TERMINATOR, FilePath);
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

// CoreSystem/Source/CoreSystem/Private/FileFunctionLibrary.cpp
bool UFileFunctionLibrary::GetSubFolders(const FString& ParentFolderPath, TArray<FString>& OutSubFolders)
{
    OutSubFolders.Empty();
    
    IFileManager& FileManager = IFileManager::Get();
    
    // UE5.7 기준: FindFilesRecursive 또는 DirectoryExists + FindFiles 사용
    TArray<FString> AllItems;
    FileManager.FindFiles(AllItems, *(ParentFolderPath + TEXT("*")), true, true);

    // 디렉토리만 필터링
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

    // 폴더 경로 추출 및 존재하지 않을 경우 생성
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

// 파일 경로와 키 이름을 받아서 문자열 값 추출
// 성공 시 OutValue에 값 저장, true 반환
bool UFileFunctionLibrary::LoadJsonStringValue(
    const FString &JsonFilePath,
    const FString &KeyName,
    FString &OutValue)
{
    OutValue.Reset();

    // 파일 존재 확인 및 내용 읽기
    FString JsonRaw;
    if (!FFileHelper::LoadFileToString(JsonRaw, *JsonFilePath))
    {
        return false;
    }

    // JSON 파싱
    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonRaw);

    if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
    {
        return false;
    }

    // 키 값 추출
    return JsonObject->TryGetStringField(*KeyName, OutValue);
}


#pragma region Private
// private functions

bool UFileFunctionLibrary::AppendStringToFileInternal(const FString &Line, const FString &FilePath)
{
    GenerateDirectory(FilePath);
    return FFileHelper::SaveStringToFile(Line, *FilePath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
}