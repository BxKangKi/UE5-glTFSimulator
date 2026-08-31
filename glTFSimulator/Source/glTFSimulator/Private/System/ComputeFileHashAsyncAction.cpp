// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#include "System/ComputeFileHashAsyncAction.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/SecureHash.h"
#include "System/SafeFileIO.h"

UComputeFileHashAsyncAction *UComputeFileHashAsyncAction::ComputeFileHashAsync(UObject *WorldContextObject, const FString &FilePath)
{
    if (!ensureMsgf(IsInGameThread(),
        TEXT("UComputeFileHashAsyncAction::ComputeFileHashAsync must create its UObject on the game thread")))
    {
        return nullptr;
    }

    UComputeFileHashAsyncAction *Action = NewObject<UComputeFileHashAsyncAction>();
    Action->WorldContextObject = WorldContextObject;
    Action->TargetFilePath = FilePath;
    return Action;
}

void UComputeFileHashAsyncAction::Activate()
{
    if (!ensureMsgf(IsInGameThread(), TEXT("UComputeFileHashAsyncAction::Activate must run on the game thread")))
    {
        return;
    }

    RegisterWithGameInstance(WorldContextObject);
    const FString FilePath = TargetFilePath;
    TWeakObjectPtr<UComputeFileHashAsyncAction> WeakThis(this);

    // Only immutable path data crosses to the worker. The UObject is touched again solely from
    // the game-thread completion callback, so world teardown cannot produce a use-after-free.
    const bool bWorkerQueued = FSafeFileIO::RunTrackedWorker([WeakThis, FilePath]()
    {
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        TUniquePtr<IFileHandle> FileHandle(PlatformFile.OpenRead(*FilePath));
        if (!FileHandle.IsValid())
        {
            if (!FSafeFileIO::DispatchTrackedGameThread([WeakThis, FilePath]()
            {
                if (UComputeFileHashAsyncAction* StrongThis = WeakThis.Get())
                {
                    UE_LOG(LogTemp, Warning, TEXT("Cannot hash missing file: %s"), *FilePath);
                    StrongThis->OnCompleted.Broadcast(TEXT("File Not Found"));
                    StrongThis->SetReadyToDestroy();
                }
            }))
            {
                // Shutdown suppresses late delegate broadcasts and owns UObject teardown.
                return;
            }
            return;
        }

        // A reusable 256 KiB buffer keeps memory bounded even for large external assets.
        constexpr int32 BufferSize = 256 * 1024;
        TArray<uint8> Buffer;
        Buffer.SetNumUninitialized(BufferSize);

        FSHA1 Sha;
        const int64 TotalSize = FileHandle->Size();
        if (TotalSize < 0)
        {
            if (!FSafeFileIO::DispatchTrackedGameThread([WeakThis, FilePath]()
            {
                if (UComputeFileHashAsyncAction* StrongThis = WeakThis.Get())
                {
                    UE_LOG(LogTemp, Warning, TEXT("Cannot determine file size for hashing: %s"), *FilePath);
                    StrongThis->OnCompleted.Broadcast(TEXT("Read Error"));
                    StrongThis->SetReadyToDestroy();
                }
            }))
            {
                return;
            }
            return;
        }

        int64 ReadBytes = 0;

        while (ReadBytes < TotalSize)
        {
            const int32 ToRead = static_cast<int32>(
                FMath::Min<int64>(BufferSize, TotalSize - ReadBytes));
            if (!FileHandle->Read(Buffer.GetData(), ToRead))
            {
                if (!FSafeFileIO::DispatchTrackedGameThread([WeakThis, FilePath]()
                {
                    if (UComputeFileHashAsyncAction* StrongThis = WeakThis.Get())
                    {
                        UE_LOG(LogTemp, Warning, TEXT("Failed while hashing file: %s"), *FilePath);
                        StrongThis->OnCompleted.Broadcast(TEXT("Read Error"));
                        StrongThis->SetReadyToDestroy();
                    }
                }))
                {
                    return;
                }
                return;
            }
            Sha.Update(Buffer.GetData(), ToRead);
            ReadBytes += ToRead;
        }

        Sha.Final();
        uint8 Hash[20];
        Sha.GetHash(Hash);
        FString HashString = BytesToHex(Hash, UE_ARRAY_COUNT(Hash));

        if (!FSafeFileIO::DispatchTrackedGameThread(
            [WeakThis, HashString = MoveTemp(HashString)]()
        {
            if (UComputeFileHashAsyncAction* StrongThis = WeakThis.Get())
            {
                StrongThis->OnCompleted.Broadcast(HashString);
                StrongThis->SetReadyToDestroy();
            }
        }))
        {
            return;
        }
    });

    if (!bWorkerQueued)
    {
        // Activate runs on the game thread, so an unscheduled action can be released immediately.
        SetReadyToDestroy();
    }
}
