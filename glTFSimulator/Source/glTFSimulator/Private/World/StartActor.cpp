// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "World/StartActor.h"
#include "System/MacroLibrary.h"
#include "World/WorldData.h"
#include "System/FileFunctionLibrary.h"

void AStartActor::BeginPlay()
{
    Super::BeginPlay();
    BuildLevelFolderNameMap();
}


void AStartActor::BuildLevelFolderNameMap()
{
    FolderNameMap.Empty();

    // 프로젝트 Content 디렉토리 기준 절대 경로 생성
    FString RootPath = PATH_ROOT;
    // 헬퍼 함수들로 한 줄씩 처리
    TArray<FString> SubFolders;
    if (!UFileFunctionLibrary::GetSubFolders(RootPath, SubFolders))
    {
        UE_LOG(LogTemp, Warning, TEXT("No subfolders found in %s"), *RootPath);
        return;
    }

    // 각 하위 폴더 처리
    for (const FString &SubFolderName : SubFolders)
    {
        FString FullSubFolderPath = RootPath + SubFolderName;

        if (FPaths::DirectoryExists(FullSubFolderPath))
        {
            FString LevelJsonPath = FullSubFolderPath / LEVEL_FILE_NAME;
            FString NameValue;
            if (UFileFunctionLibrary::LoadJsonStringValue(LevelJsonPath, LEVELNAME, NameValue))
            {
                FolderNameMap.Add(SubFolderName, NameValue);
                UE_LOG(LogTemp, Log, TEXT("Loaded Level: Folder=%s, Name=%s"),
                        *SubFolderName, *NameValue);
            }
        }
    }
}