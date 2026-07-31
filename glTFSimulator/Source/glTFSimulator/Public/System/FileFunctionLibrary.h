// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Serialization/BufferArchive.h"
#include "Templates/SharedPointer.h"
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

    // Returns the daily simulator log file under the user-facing Logs directory.
    static FString GetSimulatorLogFilePath();

    // Writes a categorized simulator log line to the daily Logs/log_YYYYMMDD.txt file.
    static bool WriteSimulatorLog(const FString& Category, const FString& Message);

    // Thread-pool version of WriteSimulatorLog for heavy streaming systems.
    static void WriteSimulatorLogAsync(const FString& Category, const FString& Message);
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
    // Internal helper that appends text to a file.
    static bool AppendStringToFileInternal(const FString &Line, const FString &FilePath);
};
