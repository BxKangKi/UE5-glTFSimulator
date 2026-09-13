// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file EntityArchive.h
 * 역할: 월드의 변경 가능한 객체·플레이어 상태를 저장합니다.
 * 핵심 기능: 512m 청크, append-only 커밋·복구, 범위 읽기.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "System/SafeFileIO.h"
#include "World/PlayerData.h"

/** 512 m spatial key stored in WorldName.dat. Values are chunk indices, not centimetres. */
struct GLTFSIMULATOR_API FWorldChunkCoordinate
{
    int32 X = 0;
    int32 Y = 0;
    int32 Z = 0;

    bool operator==(const FWorldChunkCoordinate& Other) const
    {
        return X == Other.X && Y == Other.Y && Z == Other.Z;
    }
};

FORCEINLINE uint32 GetTypeHash(const FWorldChunkCoordinate& Value)
{
    return HashCombine(HashCombine(::GetTypeHash(Value.X), ::GetTypeHash(Value.Y)), ::GetTypeHash(Value.Z));
}

/**
 * Dynamic state for one runtime entity; render assets remain exclusively in .gwd.
 * EntityUUID identifies this placement, while ModelUUID selects its immutable archive model. The
 * split permits any number of placements of one model without aliasing their persistent state.
 */
struct GLTFSIMULATOR_API FWorldChunkObject
{
    FGuid EntityUUID;
    FGuid ModelUUID;
    FVector Location = FVector::ZeroVector;
    FQuat Rotation = FQuat::Identity;
    FVector Scale = FVector::OneVector;
    FVector Velocity = FVector::ZeroVector;
    FVector AngularVelocity = FVector::ZeroVector;
};

/** Non-spatial dynamic state shares the same transactional .dat commit log. */
struct GLTFSIMULATOR_API FWorldRuntimeState
{
    float WorldTime = 0.0f;
    FString SelectedPlayer;
    TArray<FWorldPlayerRecord> Players;
};

/**
 * Thread-safe append-only store for Worlds/Data/WorldName.dat.
 *
 * Only the compact directory is read during Open(). Each entity chunk is range-read on demand.
 * Writes append an immutable payload and a replacement directory, flush both, then append a
 * checksummed commit footer. No existing byte is rewritten. A crash can therefore leave an
 * unreferenced tail, but Open() can scan backward to the preceding durable footer without relying
 * on platform-specific seek behaviour of append handles.
 */
class GLTFSIMULATOR_API FEntityArchiveStore final
    : public TSharedFromThis<FEntityArchiveStore, ESPMode::ThreadSafe>
{
public:
    static constexpr int64 MaxChunkPayloadBytes = 256ll * 1024ll * 1024ll;
    static constexpr int64 MaxStatePayloadBytes = 64ll * 1024ll * 1024ll;
    static constexpr int32 MaxChunks = 2000000;

    static FString MakeArchivePath(const FString& WorldRoot);
    static TSharedPtr<FEntityArchiveStore, ESPMode::ThreadSafe> Open(
        const FString& WorldRoot,
        bool bCreateIfMissing,
        FString& OutError);

    const FString& GetPath() const { return Path; }
    uint64 GetGeneration() const;
    /**
     * Returns whether the current immutable directory contains a payload for this coordinate.
     * This is a metadata-only query and never opens or reads the archive file.
     */
    bool ContainsChunk(const FWorldChunkCoordinate& Coordinate) const;

    bool LoadChunk(
        const FWorldChunkCoordinate& Coordinate,
        TArray<FWorldChunkObject>& OutObjects,
        FString& OutError) const;
    /** Reserves request order before worker dispatch so late workers cannot overwrite newer state. */
    uint64 ReserveChunkWrite(const FWorldChunkCoordinate& Coordinate);
    FSafeFileWriteResult SaveChunk(
        const FWorldChunkCoordinate& Coordinate,
        const TArray<FWorldChunkObject>& Objects,
        uint64 WriteOrder = 0);

    /**
     * Commits several spatial chunks under one footer. This is used for boundary crossings so a
     * crash can expose neither a missing entity nor two committed copies of the same placement.
     */
    FSafeFileWriteResult SaveChunks(
        const TMap<FWorldChunkCoordinate, TArray<FWorldChunkObject>>& Chunks,
        const TMap<FWorldChunkCoordinate, uint64>& WriteOrders);

    bool LoadRuntimeState(FWorldRuntimeState& OutState, bool& bOutMissing, FString& OutError) const;
    uint64 ReserveRuntimeStateWrite();
    FSafeFileWriteResult SaveRuntimeState(
        const FWorldRuntimeState& State,
        uint64 WriteOrder = 0);

    /** Internal immutable range descriptor; public only so file-local serializers can stay POD-only. */
    struct FRecord
    {
        uint64 Offset = 0;
        uint64 Size = 0;
        uint32 Crc = 0;
    };

private:
    FString Path;
    mutable FCriticalSection Mutex;
    TMap<FWorldChunkCoordinate, FRecord> ChunkRecords;
    FRecord RuntimeStateRecord;
    uint64 Generation = 0;
    uint64 NextWriteOrder = 0;
    TMap<FWorldChunkCoordinate, uint64> LatestChunkWriteOrders;
    uint64 LatestRuntimeStateWriteOrder = 0;
    bool CommitRuntimeState(
        const TArray<uint8>& Payload,
        uint64 WriteOrder,
        FString& OutError);
    bool CommitChunkRecords(
        const TMap<FWorldChunkCoordinate, TArray<uint8>>& Payloads,
        const TMap<FWorldChunkCoordinate, uint64>& WriteOrders,
        FString& OutError);
};
