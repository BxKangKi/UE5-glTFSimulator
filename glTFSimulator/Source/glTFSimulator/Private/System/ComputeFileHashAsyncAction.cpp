// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "System/ComputeFileHashAsyncAction.h"
#include "Async/Async.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/SecureHash.h"

UComputeFileHashAsyncAction *UComputeFileHashAsyncAction::ComputeFileHashAsync(UObject *WorldContextObject, const FString &FilePath)
{
    UComputeFileHashAsyncAction *Action = NewObject<UComputeFileHashAsyncAction>();
    Action->WorldContextObject = WorldContextObject;
    Action->TargetFilePath = FilePath;
    return Action;
}

void UComputeFileHashAsyncAction::Activate()
{
    RegisterWithGameInstance(WorldContextObject);
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this]()
              {
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        TUniquePtr<IFileHandle> FileHandle(PlatformFile.OpenRead(*TargetFilePath));
        if (!FileHandle.IsValid())
        {
            AsyncTask(ENamedThreads::GameThread, [this]()
                      {
                UE_LOG(LogTemp, Error, TEXT("ComputeFileHashAsync::File Not Found"));
                OnCompleted.Broadcast(TEXT("File Not Found")); });
                SetReadyToDestroy();
            return;
        }

        constexpr int32 BufferSize = 256 * 1024; // 256KB
        TArray<uint8> Buffer;
        Buffer.SetNumUninitialized(BufferSize);

        FSHA1 Sha;
        const int64 TotalSize = FileHandle->Size();
        int64 ReadBytes = 0;

        while (ReadBytes < TotalSize)
        {
            int32 ToRead = FMath::Min<int64>(BufferSize, TotalSize - ReadBytes);
            if (!FileHandle->Read(Buffer.GetData(), ToRead))
            {
                AsyncTask(ENamedThreads::GameThread, [this]()
                          {
                    UE_LOG(LogTemp, Error, TEXT("ComputeFileHashAsync::Read Error"));
                    OnCompleted.Broadcast(TEXT("Read Error"));
                    SetReadyToDestroy(); });
                return;
            }
            Sha.Update(Buffer.GetData(), ToRead);
            ReadBytes += ToRead;
        }

        Sha.Final();
        uint8 Hash[20];
        Sha.GetHash(Hash);
        FString HashStr = BytesToHex(Hash, 20);

        AsyncTask(ENamedThreads::GameThread, [this, HashStr]()
        {
            OnCompleted.Broadcast(HashStr);
            UE_LOG(LogTemp, Log, TEXT("Hash Value: %s"), *HashStr);
            SetReadyToDestroy();
        }); });
}