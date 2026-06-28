// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Serialization/BufferArchive.h"
#include "Templates/SharedPointer.h"
#include "HAL/CriticalSection.h"
#include "FileFunctionLibrary.generated.h"

UCLASS()
class GLTFSIMULATOR_API UFileFunctionLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
#pragma region File IO
    UFUNCTION(BlueprintCallable)
    static bool CheckFile(const FString &FilePath);

    UFUNCTION(BlueprintCallable)
    static bool GenerateDirectory(const FString &FilePath);

    UFUNCTION(BlueprintCallable)
    static TArray<FString> GetFileNamesWithExtension(const FString &Directory,
                                                    const FString &Extension);

    UFUNCTION(BlueprintCallable)
    static FString GetPathWithoutExtension(const FString &Path);

    static bool ToBinary(FBufferArchive Ar, const FString &FilePath);
    static void ToBinaryAsync(FBufferArchive Ar, const FString &FilePath);
    static bool FromBinary(TArray<uint8> &FileData, const FString &FilePath);

    // add line function
    UFUNCTION(BlueprintCallable)
    static bool AppendLineToFile(const FString &Line, const FString &FilePath);

    // async add line function
    UFUNCTION(BlueprintCallable)
    static void AppendLineToFileAsync(const FString &Line, const FString &FilePath);
#pragma region Json File
    // Json related functions
    static void ToJsonAsync(TSharedRef<FJsonObject> Json, const FString &Path);
    static bool ToJson(TSharedRef<FJsonObject> Json, const FString &FilePath);
    static TSharedPtr<FJsonObject> FromJson(const FString &Path);

    UFUNCTION(BlueprintCallable)
    static bool LoadJsonStringValue(const FString &JsonFilePath,
                                    const FString &KeyName,
                                    FString &OutValue);
    UFUNCTION(BlueprintCallable)
    static bool GetSubFolders(const FString &ParentFolderPath, TArray<FString> &OutSubFolders);

private:
    static FCriticalSection FileWriteCriticalSection;
    // 파일에 문자열을 추가 저장하는 내부 함수
    static bool AppendStringToFileInternal(const FString &Line, const FString &FilePath);
};