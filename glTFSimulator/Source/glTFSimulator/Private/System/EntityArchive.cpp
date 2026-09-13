// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file EntityArchive.cpp
 * 역할: 월드의 변경 가능한 객체·플레이어 상태를 저장합니다.
 * 핵심 기능: 512m 청크, append-only 커밋·복구, 범위 읽기.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "System/EntityArchive.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Crc.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace EntityArchivePrivate
{
    constexpr uint64 Magic = 0x32595449544E4547ull; // ASCII "GENTITY2", little endian.
    constexpr uint64 FooterMagic = 0x325254464D4F4347ull; // ASCII "GCOMFTR2", little endian.
    constexpr uint32 Version = 2;
    constexpr int32 HeaderBytes = 64;
    constexpr int32 CommitFooterBytes = 56;
    constexpr int32 RecoveryScanBytes = 1024 * 1024;
    constexpr uint32 ChunkPayloadMagic = 0x31484345u; // "ECH1"
    constexpr uint32 StatePayloadMagic = 0x31545345u; // "EST1"
    constexpr uint16 PayloadVersion = 1;
    constexpr int32 MaxObjectsPerChunk = 1000000;
    constexpr int32 MaxPlayers = 10000;
    constexpr int32 MaxItemsPerPlayer = 65536;
    constexpr int32 MaxNameBytes = 4096;
    constexpr int32 MaxPathBytes = 32768;
    constexpr int32 MaxJsonBytes = 4 * 1024 * 1024;
    constexpr int64 MaxArchiveBytes = 1024ll * 1024ll * 1024ll * 1024ll;

    // One process-wide store per canonical path serializes writers across rapid world restarts.
    // Without this registry, an old world's final worker and a new world's first worker could own
    // independent mutexes while appending competing commit generations.
    FCriticalSection StoreRegistryMutex;
    TMap<FString, TWeakPtr<FEntityArchiveStore, ESPMode::ThreadSafe>> StoreRegistry;

    bool IsFiniteVector(const FVector& Value)
    {
        return !Value.ContainsNaN() && FMath::IsFinite(Value.X)
            && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
    }

    bool IsFiniteQuat(const FQuat& Value)
    {
        return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y)
            && FMath::IsFinite(Value.Z) && FMath::IsFinite(Value.W);
    }

    class FWriter
    {
    public:
        explicit FWriter(TArray<uint8>& InBytes, bool bReset = true) : Bytes(InBytes)
        {
            if (bReset) Bytes.Reset();
        }
        bool IsOk() const { return bOk; }
        void U8(uint8 Value) { if (bOk) Bytes.Add(Value); }
        void U16(uint16 Value) { U8(Value & 0xffu); U8((Value >> 8u) & 0xffu); }
        void U32(uint32 Value)
        {
            U8(Value & 0xffu); U8((Value >> 8u) & 0xffu);
            U8((Value >> 16u) & 0xffu); U8((Value >> 24u) & 0xffu);
        }
        void I32(int32 Value) { U32(static_cast<uint32>(Value)); }
        void U64(uint64 Value)
        {
            for (int32 Shift = 0; Shift < 64; Shift += 8) U8((Value >> Shift) & 0xffull);
        }
        void Float(float Value)
        {
            uint32 Bits = 0; FMemory::Memcpy(&Bits, &Value, sizeof(Bits)); U32(Bits);
        }
        void Double(double Value)
        {
            uint64 Bits = 0; FMemory::Memcpy(&Bits, &Value, sizeof(Bits)); U64(Bits);
        }
        void Guid(const FGuid& Value)
        {
            U32(Value.A); U32(Value.B); U32(Value.C); U32(Value.D);
        }
        void Vector(const FVector& Value) { Double(Value.X); Double(Value.Y); Double(Value.Z); }
        void Quat(const FQuat& Value)
        {
            Double(Value.X); Double(Value.Y); Double(Value.Z); Double(Value.W);
        }
        void String(const FString& Value, int32 Limit)
        {
            FTCHARToUTF8 Utf8(*Value);
            if (Utf8.Length() < 0 || Utf8.Length() > Limit) { bOk = false; return; }
            U32(static_cast<uint32>(Utf8.Length()));
            if (bOk && Utf8.Length() > 0)
                Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
        }
    private:
        TArray<uint8>& Bytes;
        bool bOk = true;
    };

    class FReader
    {
    public:
        explicit FReader(const TArray<uint8>& InBytes) : Bytes(InBytes) {}
        bool IsOk() const { return bOk; }
        bool IsAtEnd() const { return bOk && Offset == Bytes.Num(); }
        uint8 U8() { if (!Require(1)) return 0; return Bytes[Offset++]; }
        uint16 U16() { const uint16 A = U8(); const uint16 B = U8(); return A | (B << 8u); }
        uint32 U32()
        {
            const uint32 A = U8(); const uint32 B = U8();
            const uint32 C = U8(); const uint32 D = U8();
            return A | (B << 8u) | (C << 16u) | (D << 24u);
        }
        int32 I32() { return static_cast<int32>(U32()); }
        uint64 U64()
        {
            uint64 Value = 0;
            for (int32 Shift = 0; Shift < 64; Shift += 8) Value |= uint64(U8()) << Shift;
            return Value;
        }
        float Float()
        {
            const uint32 Bits = U32(); float Value = 0.0f;
            FMemory::Memcpy(&Value, &Bits, sizeof(Value)); return Value;
        }
        double Double()
        {
            const uint64 Bits = U64(); double Value = 0.0;
            FMemory::Memcpy(&Value, &Bits, sizeof(Value)); return Value;
        }
        FGuid Guid()
        {
            const uint32 A = U32(); const uint32 B = U32();
            const uint32 C = U32(); const uint32 D = U32();
            return FGuid(A, B, C, D);
        }
        FVector Vector()
        {
            const double X = Double(); const double Y = Double(); const double Z = Double();
            return FVector(X, Y, Z);
        }
        FQuat Quat()
        {
            const double X = Double(); const double Y = Double();
            const double Z = Double(); const double W = Double();
            return FQuat(X, Y, Z, W);
        }
        FString String(int32 Limit)
        {
            const uint32 Count = U32();
            if (!bOk || Count > static_cast<uint32>(Limit) || Count > static_cast<uint32>(MAX_int32)
                || !Require(static_cast<int32>(Count)))
            {
                bOk = false;
                return FString();
            }
            if (Count == 0) return FString();
            const ANSICHAR* Source = reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset);
            FUTF8ToTCHAR Converted(Source, static_cast<int32>(Count));
            Offset += static_cast<int32>(Count);
            if (Converted.Length() < 0) { bOk = false; return FString(); }
            return FString(Converted.Length(), Converted.Get());
        }

    private:
        bool Require(int32 Count)
        {
            if (!bOk || Count < 0 || Offset < 0 || Count > Bytes.Num() - Offset)
            {
                bOk = false;
                return false;
            }
            return true;
        }
        const TArray<uint8>& Bytes;
        int32 Offset = 0;
        bool bOk = true;
    };

    struct FCommitFooter
    {
        uint64 ReadMagic = 0;
        uint64 Generation = 0;
        uint64 DirectoryOffset = 0;
        uint64 DirectorySize = 0;
        uint32 DirectoryCrc = 0;
        uint64 CommittedSize = 0;
        uint32 FooterCrc = 0;
    };

    FSafeFileWriteResult MakeWriteResult(
        const FString& Path,
        ESafeFileIOStatus Status,
        const FString& Error = FString(),
        int64 BytesWritten = 0)
    {
        FSafeFileWriteResult Result;
        Result.Path = Path;
        Result.Status = Status;
        Result.Error = Error;
        Result.BytesWritten = BytesWritten;
        return Result;
    }

    bool ReadExact(IFileHandle& Handle, uint8* Destination, int64 Count)
    {
        return Count >= 0 && (Count == 0 || (Destination && Handle.Read(Destination, Count)));
    }

    bool WriteExact(IFileHandle& Handle, const uint8* Source, int64 Count)
    {
        return Count >= 0 && (Count == 0 || (Source && Handle.Write(Source, Count)));
    }

    TArray<uint8> SerializeFooterPrefix(const FCommitFooter& Footer)
    {
        TArray<uint8> Bytes;
        FWriter Writer(Bytes);
        Writer.U64(FooterMagic);
        Writer.U64(Footer.Generation);
        Writer.U64(Footer.DirectoryOffset);
        Writer.U64(Footer.DirectorySize);
        Writer.U32(Footer.DirectoryCrc);
        Writer.U32(0);
        Writer.U64(Footer.CommittedSize);
        return Bytes;
    }

    TArray<uint8> SerializeFooter(const FCommitFooter& Footer)
    {
        TArray<uint8> Bytes = SerializeFooterPrefix(Footer);
        FWriter Writer(Bytes, false);
        Writer.U32(FCrc::MemCrc32(Bytes.GetData(), Bytes.Num()));
        Writer.U32(0);
        return Bytes;
    }

    FCommitFooter DeserializeFooter(const TArray<uint8>& Bytes)
    {
        FCommitFooter Footer;
        if (Bytes.Num() != CommitFooterBytes) return Footer;
        FReader Reader(Bytes);
        Footer.ReadMagic = Reader.U64();
        Footer.Generation = Reader.U64();
        Footer.DirectoryOffset = Reader.U64();
        Footer.DirectorySize = Reader.U64();
        Footer.DirectoryCrc = Reader.U32();
        (void)Reader.U32();
        Footer.CommittedSize = Reader.U64();
        Footer.FooterCrc = Reader.U32();
        (void)Reader.U32();
        if (!Reader.IsAtEnd()) Footer.Generation = 0;
        return Footer;
    }

    bool ValidateFooterChecksum(const FCommitFooter& Footer)
    {
        const TArray<uint8> Prefix = SerializeFooterPrefix(Footer);
        return Footer.ReadMagic == FooterMagic && Footer.Generation > 0
            && Prefix.Num() == CommitFooterBytes - 8
            && FCrc::MemCrc32(Prefix.GetData(), Prefix.Num()) == Footer.FooterCrc;
    }

    void WriteRecord(FWriter& Writer, const FEntityArchiveStore::FRecord& Record)
    {
        Writer.U64(Record.Offset);
        Writer.U64(Record.Size);
        Writer.U32(Record.Crc);
        Writer.U32(0);
    }

    FEntityArchiveStore::FRecord ReadRecord(FReader& Reader)
    {
        FEntityArchiveStore::FRecord Record;
        Record.Offset = Reader.U64();
        Record.Size = Reader.U64();
        Record.Crc = Reader.U32();
        (void)Reader.U32();
        return Record;
    }

    bool IsValidRecord(
        const FEntityArchiveStore::FRecord& Record,
        uint64 DirectoryOffset,
        uint64 CommittedSize,
        int64 MaximumPayload,
        bool bAllowEmpty)
    {
        if (bAllowEmpty && Record.Offset == 0 && Record.Size == 0 && Record.Crc == 0) return true;
        return Record.Offset >= static_cast<uint64>(HeaderBytes) && Record.Size > 0
            && Record.Size <= static_cast<uint64>(MaximumPayload)
            && Record.Offset <= CommittedSize
            && Record.Size <= CommittedSize - Record.Offset
            && Record.Offset + Record.Size <= DirectoryOffset;
    }

    bool SerializeDirectory(
        const FEntityArchiveStore::FRecord& StateRecord,
        const TMap<FWorldChunkCoordinate, FEntityArchiveStore::FRecord>& Chunks,
        TArray<uint8>& OutBytes,
        FString& OutError)
    {
        if (Chunks.Num() > FEntityArchiveStore::MaxChunks)
        {
            OutError = TEXT("The .dat directory exceeds the chunk-count safety limit");
            return false;
        }
        TArray<FWorldChunkCoordinate> Coordinates;
        Chunks.GetKeys(Coordinates);
        Coordinates.Sort([](const FWorldChunkCoordinate& A, const FWorldChunkCoordinate& B)
        {
            if (A.X != B.X) return A.X < B.X;
            if (A.Y != B.Y) return A.Y < B.Y;
            return A.Z < B.Z;
        });
        FWriter Writer(OutBytes);
        WriteRecord(Writer, StateRecord);
        Writer.U32(static_cast<uint32>(Coordinates.Num()));
        for (const FWorldChunkCoordinate& Coordinate : Coordinates)
        {
            Writer.I32(Coordinate.X); Writer.I32(Coordinate.Y); Writer.I32(Coordinate.Z);
            Writer.U32(0);
            WriteRecord(Writer, Chunks.FindChecked(Coordinate));
        }
        if (!Writer.IsOk())
        {
            OutError = TEXT("The .dat directory could not be serialized");
            return false;
        }
        return true;
    }

    bool DeserializeDirectory(
        const TArray<uint8>& Bytes,
        const FCommitFooter& Footer,
        FEntityArchiveStore::FRecord& OutState,
        TMap<FWorldChunkCoordinate, FEntityArchiveStore::FRecord>& OutChunks,
        FString& OutError)
    {
        FReader Reader(Bytes);
        OutState = ReadRecord(Reader);
        const uint32 Count = Reader.U32();
        if (!Reader.IsOk() || Count > FEntityArchiveStore::MaxChunks
            || !IsValidRecord(OutState, Footer.DirectoryOffset, Footer.CommittedSize,
                FEntityArchiveStore::MaxStatePayloadBytes, true))
        {
            OutError = TEXT("The .dat directory state row or chunk count is invalid");
            return false;
        }
        for (uint32 Index = 0; Index < Count; ++Index)
        {
            FWorldChunkCoordinate Coordinate;
            Coordinate.X = Reader.I32(); Coordinate.Y = Reader.I32(); Coordinate.Z = Reader.I32();
            (void)Reader.U32();
            const FEntityArchiveStore::FRecord Record = ReadRecord(Reader);
            if (!Reader.IsOk() || OutChunks.Contains(Coordinate)
                || !IsValidRecord(Record, Footer.DirectoryOffset, Footer.CommittedSize,
                    FEntityArchiveStore::MaxChunkPayloadBytes, false))
            {
                OutError = FString::Printf(TEXT("Invalid or duplicate .dat chunk row at index %u"), Index);
                return false;
            }
            OutChunks.Add(Coordinate, Record);
        }
        if (!Reader.IsAtEnd())
        {
            OutError = TEXT("The .dat directory is truncated or contains trailing bytes");
            return false;
        }

        // A valid generation never aliases two logical records to the same byte range. Besides
        // catching corruption, this prevents a malformed directory from charging the same payload
        // to multiple chunks while it is being range-read concurrently.
        TArray<TPair<uint64, uint64>> Ranges;
        Ranges.Reserve(OutChunks.Num() + (OutState.Size > 0 ? 1 : 0));
        if (OutState.Size > 0) Ranges.Emplace(OutState.Offset, OutState.Size);
        for (const TPair<FWorldChunkCoordinate, FEntityArchiveStore::FRecord>& Pair : OutChunks)
        {
            Ranges.Emplace(Pair.Value.Offset, Pair.Value.Size);
        }
        Ranges.Sort([](const TPair<uint64, uint64>& A, const TPair<uint64, uint64>& B)
        {
            return A.Key < B.Key;
        });
        for (int32 Index = 1; Index < Ranges.Num(); ++Index)
        {
            if (Ranges[Index - 1].Key + Ranges[Index - 1].Value > Ranges[Index].Key)
            {
                OutError = TEXT("The .dat live payload ranges overlap");
                return false;
            }
        }
        return true;
    }

    bool ReadDirectoryForFooter(
        IFileHandle& Handle,
        int64 PhysicalFileSize,
        const FCommitFooter& Footer,
        FEntityArchiveStore::FRecord& OutState,
        TMap<FWorldChunkCoordinate, FEntityArchiveStore::FRecord>& OutChunks,
        FString& OutError)
    {
        if (!ValidateFooterChecksum(Footer)
            || Footer.CommittedSize < static_cast<uint64>(HeaderBytes + CommitFooterBytes)
            || Footer.CommittedSize > static_cast<uint64>(PhysicalFileSize)
            || Footer.CommittedSize > static_cast<uint64>(MaxArchiveBytes)
            || Footer.DirectoryOffset < HeaderBytes || Footer.DirectorySize < 28
            || Footer.DirectorySize > static_cast<uint64>(FEntityArchiveStore::MaxChunkPayloadBytes)
            || Footer.DirectoryOffset > Footer.CommittedSize
            || Footer.DirectorySize > Footer.CommittedSize - Footer.DirectoryOffset
            || Footer.DirectoryOffset + Footer.DirectorySize
                != Footer.CommittedSize - CommitFooterBytes
            || Footer.DirectorySize > static_cast<uint64>(MAX_int32))
        {
            return false;
        }
        TArray<uint8> Directory;
        Directory.SetNumUninitialized(static_cast<int32>(Footer.DirectorySize));
        if (!Handle.Seek(static_cast<int64>(Footer.DirectoryOffset))
            || !ReadExact(Handle, Directory.GetData(), Directory.Num())
            || FCrc::MemCrc32(Directory.GetData(), Directory.Num()) != Footer.DirectoryCrc)
        {
            return false;
        }
        return DeserializeDirectory(Directory, Footer, OutState, OutChunks, OutError);
    }

    bool ReadFooterAt(
        IFileHandle& Handle,
        int64 PhysicalFileSize,
        int64 FooterOffset,
        FCommitFooter& OutFooter,
        FEntityArchiveStore::FRecord& OutState,
        TMap<FWorldChunkCoordinate, FEntityArchiveStore::FRecord>& OutChunks,
        FString& OutError)
    {
        if (FooterOffset < HeaderBytes
            || FooterOffset > PhysicalFileSize - CommitFooterBytes)
        {
            return false;
        }

        TArray<uint8> Bytes;
        Bytes.SetNumUninitialized(CommitFooterBytes);
        if (!Handle.Seek(FooterOffset)
            || !ReadExact(Handle, Bytes.GetData(), Bytes.Num()))
        {
            return false;
        }

        const FCommitFooter Footer = DeserializeFooter(Bytes);
        if (Footer.CommittedSize != static_cast<uint64>(FooterOffset + CommitFooterBytes))
        {
            return false;
        }

        FEntityArchiveStore::FRecord State;
        TMap<FWorldChunkCoordinate, FEntityArchiveStore::FRecord> Chunks;
        FString DirectoryError;
        if (!ReadDirectoryForFooter(
                Handle, PhysicalFileSize, Footer, State, Chunks, DirectoryError))
        {
            if (!DirectoryError.IsEmpty()) OutError = MoveTemp(DirectoryError);
            return false;
        }

        OutFooter = Footer;
        OutState = State;
        OutChunks = MoveTemp(Chunks);
        return true;
    }

    bool FindLatestValidFooter(
        IFileHandle& Handle,
        int64 PhysicalFileSize,
        FCommitFooter& OutFooter,
        FEntityArchiveStore::FRecord& OutState,
        TMap<FWorldChunkCoordinate, FEntityArchiveStore::FRecord>& OutChunks,
        FString& OutError)
    {
        if (PhysicalFileSize < HeaderBytes + CommitFooterBytes) return false;

        // The normal path is one fixed-size read at EOF. The reverse scan is entered only after an
        // interrupted append left bytes beyond the last durable footer.
        const int64 TailOffset = PhysicalFileSize - CommitFooterBytes;
        if (ReadFooterAt(
                Handle, PhysicalFileSize, TailOffset,
                OutFooter, OutState, OutChunks, OutError))
        {
            return true;
        }

        uint8 MagicBytes[sizeof(FooterMagic)] = {};
        for (int32 Shift = 0; Shift < 64; Shift += 8)
        {
            MagicBytes[Shift / 8] = static_cast<uint8>(FooterMagic >> Shift);
        }

        int64 SearchEnd = TailOffset;
        TArray<uint8> Window;
        while (SearchEnd > HeaderBytes)
        {
            const int64 SearchStart = FMath::Max<int64>(
                HeaderBytes, SearchEnd - RecoveryScanBytes);
            const int64 ReadEnd = FMath::Min<int64>(
                PhysicalFileSize, SearchEnd + CommitFooterBytes - 1);
            const int64 ReadSize = ReadEnd - SearchStart;
            if (ReadSize <= 0 || ReadSize > MAX_int32)
            {
                return false;
            }

            Window.SetNumUninitialized(static_cast<int32>(ReadSize));
            if (!Handle.Seek(SearchStart)
                || !ReadExact(Handle, Window.GetData(), Window.Num()))
            {
                return false;
            }

            const int32 LastCandidate = static_cast<int32>(SearchEnd - SearchStart - 1);
            for (int32 LocalOffset = LastCandidate; LocalOffset >= 0; --LocalOffset)
            {
                if (LocalOffset + CommitFooterBytes > Window.Num()
                    || FMemory::Memcmp(
                        Window.GetData() + LocalOffset,
                        MagicBytes,
                        sizeof(MagicBytes)) != 0)
                {
                    continue;
                }

                const int64 CandidateOffset = SearchStart + LocalOffset;
                if (ReadFooterAt(
                        Handle, PhysicalFileSize, CandidateOffset,
                        OutFooter, OutState, OutChunks, OutError))
                {
                    return true;
                }
            }
            SearchEnd = SearchStart;
        }
        return false;
    }

    bool JsonToString(const TSharedPtr<FJsonObject>& Json, FString& OutText)
    {
        OutText.Reset();
        const TSharedRef<FJsonObject> Object = Json.IsValid() ? Json.ToSharedRef() : MakeShared<FJsonObject>();
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutText);
        return FJsonSerializer::Serialize(Object, Writer);
    }

    bool SerializeChunk(
        const TArray<FWorldChunkObject>& Objects,
        TArray<uint8>& OutBytes,
        FString& OutError)
    {
        if (Objects.Num() > MaxObjectsPerChunk)
        {
            OutError = TEXT("An entity chunk exceeds the object-count safety limit");
            return false;
        }
        FWriter Writer(OutBytes);
        Writer.U32(ChunkPayloadMagic);
        Writer.U16(PayloadVersion);
        Writer.U16(0);
        Writer.U32(static_cast<uint32>(Objects.Num()));
        TSet<FGuid> SeenEntities;
        for (const FWorldChunkObject& Object : Objects)
        {
            if (!Object.EntityUUID.IsValid() || !Object.ModelUUID.IsValid()
                || SeenEntities.Contains(Object.EntityUUID)
                || !IsFiniteVector(Object.Location) || !IsFiniteQuat(Object.Rotation)
                || !Object.Rotation.IsNormalized() || !IsFiniteVector(Object.Scale)
                || !IsFiniteVector(Object.Velocity) || !IsFiniteVector(Object.AngularVelocity))
            {
                OutError = TEXT("An entity chunk contains an invalid model, duplicate entity UUID, or invalid dynamic transform");
                return false;
            }
            SeenEntities.Add(Object.EntityUUID);
            Writer.Guid(Object.EntityUUID);
            Writer.Guid(Object.ModelUUID);
            Writer.Vector(Object.Location);
            Writer.Quat(Object.Rotation);
            Writer.Vector(Object.Scale);
            Writer.Vector(Object.Velocity);
            Writer.Vector(Object.AngularVelocity);
        }
        if (!Writer.IsOk() || OutBytes.Num() > FEntityArchiveStore::MaxChunkPayloadBytes)
        {
            OutError = TEXT("An entity chunk could not be serialized within its byte limit");
            return false;
        }
        return true;
    }

    bool DeserializeChunk(
        const TArray<uint8>& Bytes,
        TArray<FWorldChunkObject>& OutObjects,
        FString& OutError)
    {
        FReader Reader(Bytes);
        const uint32 ReadMagic = Reader.U32();
        const uint16 ReadVersion = Reader.U16();
        (void)Reader.U16();
        const uint32 Count = Reader.U32();
        if (!Reader.IsOk() || ReadMagic != ChunkPayloadMagic || ReadVersion != PayloadVersion
            || Count > MaxObjectsPerChunk)
        {
            OutError = TEXT("The entity chunk header or object count is invalid");
            return false;
        }
        TArray<FWorldChunkObject> Objects;
        Objects.Reserve(static_cast<int32>(Count));
        TSet<FGuid> SeenEntities;
        for (uint32 Index = 0; Index < Count; ++Index)
        {
            FWorldChunkObject& Object = Objects.AddDefaulted_GetRef();
            Object.EntityUUID = Reader.Guid();
            Object.ModelUUID = Reader.Guid();
            Object.Location = Reader.Vector();
            Object.Rotation = Reader.Quat();
            Object.Scale = Reader.Vector();
            Object.Velocity = Reader.Vector();
            Object.AngularVelocity = Reader.Vector();
            if (!Reader.IsOk() || !Object.EntityUUID.IsValid() || !Object.ModelUUID.IsValid()
                || SeenEntities.Contains(Object.EntityUUID)
                || !IsFiniteVector(Object.Location) || !IsFiniteQuat(Object.Rotation)
                || !Object.Rotation.IsNormalized() || !IsFiniteVector(Object.Scale)
                || !IsFiniteVector(Object.Velocity) || !IsFiniteVector(Object.AngularVelocity))
            {
                OutError = FString::Printf(TEXT("Invalid entity object at chunk row %u"), Index);
                return false;
            }
            SeenEntities.Add(Object.EntityUUID);
        }
        if (!Reader.IsAtEnd())
        {
            OutError = TEXT("The entity chunk is truncated or contains trailing bytes");
            return false;
        }
        OutObjects = MoveTemp(Objects);
        return true;
    }

    bool SerializeState(const FWorldRuntimeState& State, TArray<uint8>& OutBytes, FString& OutError)
    {
        if (!FMath::IsFinite(State.WorldTime) || State.Players.Num() > MaxPlayers)
        {
            OutError = TEXT("The world runtime state contains invalid time or too many players");
            return false;
        }
        FWriter Writer(OutBytes);
        Writer.U32(StatePayloadMagic);
        Writer.U16(PayloadVersion);
        Writer.U16(0);
        Writer.Float(State.WorldTime);
        Writer.String(State.SelectedPlayer, MaxPathBytes);
        Writer.U32(static_cast<uint32>(State.Players.Num()));
        TSet<FString> SeenPlayers;
        for (const FWorldPlayerRecord& Player : State.Players)
        {
            const FString PlayerKey = Player.PlayerId.ToLower();
            if (Player.PlayerId.IsEmpty() || SeenPlayers.Contains(PlayerKey)
                || !IsFiniteVector(Player.Location) || !FMath::IsFinite(Player.Rotation.Pitch)
                || !FMath::IsFinite(Player.Rotation.Yaw) || !FMath::IsFinite(Player.Rotation.Roll)
                || !FMath::IsFinite(Player.Health) || Player.Level < 1
                || Player.Items.Num() > MaxItemsPerPlayer)
            {
                OutError = TEXT("The world runtime state contains an invalid player row");
                return false;
            }
            SeenPlayers.Add(PlayerKey);
            Writer.String(Player.PlayerId, MaxNameBytes);
            Writer.String(Player.DisplayName, MaxNameBytes);
            Writer.Vector(Player.Location);
            Writer.Double(Player.Rotation.Pitch);
            Writer.Double(Player.Rotation.Yaw);
            Writer.Double(Player.Rotation.Roll);
            Writer.Float(Player.Health);
            Writer.I32(Player.Level);
            Writer.String(Player.PlayerGameMode, MaxNameBytes);
            Writer.U32(static_cast<uint32>(Player.Items.Num()));
            for (const FString& Item : Player.Items) Writer.String(Item, MaxPathBytes);
            FString CustomJson;
            if (!JsonToString(Player.CustomJson, CustomJson))
            {
                OutError = TEXT("A player custom JSON object could not be serialized");
                return false;
            }
            Writer.String(CustomJson, MaxJsonBytes);
        }
        if (!Writer.IsOk() || OutBytes.Num() > FEntityArchiveStore::MaxStatePayloadBytes)
        {
            OutError = TEXT("The world runtime state exceeded a bounded field or byte limit");
            return false;
        }
        return true;
    }

    bool DeserializeState(const TArray<uint8>& Bytes, FWorldRuntimeState& OutState, FString& OutError)
    {
        FReader Reader(Bytes);
        const uint32 ReadMagic = Reader.U32();
        const uint16 ReadVersion = Reader.U16();
        (void)Reader.U16();
        FWorldRuntimeState State;
        State.WorldTime = Reader.Float();
        State.SelectedPlayer = Reader.String(MaxPathBytes);
        const uint32 Count = Reader.U32();
        if (!Reader.IsOk() || ReadMagic != StatePayloadMagic || ReadVersion != PayloadVersion
            || !FMath::IsFinite(State.WorldTime) || Count > MaxPlayers)
        {
            OutError = TEXT("The world runtime state header is invalid");
            return false;
        }
        TSet<FString> SeenPlayers;
        State.Players.Reserve(static_cast<int32>(Count));
        for (uint32 Index = 0; Index < Count; ++Index)
        {
            FWorldPlayerRecord& Player = State.Players.AddDefaulted_GetRef();
            Player.PlayerId = Reader.String(MaxNameBytes);
            Player.DisplayName = Reader.String(MaxNameBytes);
            Player.Location = Reader.Vector();
            const double Pitch = Reader.Double();
            const double Yaw = Reader.Double();
            const double Roll = Reader.Double();
            Player.Rotation = FRotator(Pitch, Yaw, Roll).GetNormalized();
            Player.Health = Reader.Float();
            Player.Level = Reader.I32();
            Player.PlayerGameMode = Reader.String(MaxNameBytes);
            const uint32 ItemCount = Reader.U32();
            const FString PlayerKey = Player.PlayerId.ToLower();
            if (!Reader.IsOk() || Player.PlayerId.IsEmpty() || SeenPlayers.Contains(PlayerKey)
                || !IsFiniteVector(Player.Location) || !FMath::IsFinite(Pitch)
                || !FMath::IsFinite(Yaw) || !FMath::IsFinite(Roll)
                || !FMath::IsFinite(Player.Health) || Player.Level < 1
                || ItemCount > MaxItemsPerPlayer)
            {
                OutError = FString::Printf(TEXT("Invalid player row %u in world runtime state"), Index);
                return false;
            }
            SeenPlayers.Add(PlayerKey);
            Player.Items.Reserve(static_cast<int32>(ItemCount));
            for (uint32 ItemIndex = 0; ItemIndex < ItemCount; ++ItemIndex)
                Player.Items.Add(Reader.String(MaxPathBytes));
            const FString CustomJson = Reader.String(MaxJsonBytes);
            FSafeJsonLimits Limits;
            Limits.MaxFileBytes = MaxJsonBytes;
            Limits.bAllowBackupRecovery = false;
            const FSafeJsonLoadResult Json = FSafeFileIO::ParseJsonText(
                CustomJson.IsEmpty() ? TEXT("{}") : CustomJson,
                FString::Printf(TEXT(".dat Player[%u]"), Index),
                Limits);
            if (!Reader.IsOk() || !Json.IsSuccess())
            {
                OutError = FString::Printf(TEXT("Invalid custom JSON for .dat player row %u"), Index);
                return false;
            }
            Player.CustomJson = Json.JsonObject;
        }
        if (!Reader.IsAtEnd())
        {
            OutError = TEXT("The world runtime state is truncated or contains trailing bytes");
            return false;
        }
        OutState = MoveTemp(State);
        return true;
    }

    bool ReadRecordPayload(
        const FString& Path,
        const FEntityArchiveStore::FRecord& Record,
        int64 MaxBytes,
        TArray<uint8>& OutBytes,
        FString& OutError)
    {
        OutBytes.Reset();
        if (Record.Size == 0 || Record.Size > static_cast<uint64>(MaxBytes)
            || Record.Size > static_cast<uint64>(MAX_int32))
        {
            OutError = TEXT("The .dat record size is invalid");
            return false;
        }
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*Path));
        if (!Handle.IsValid())
        {
            OutError = TEXT("The .dat archive could not be opened for a record read");
            return false;
        }

        // Cache and validate the signed platform size once. Repeated Size() calls can be system
        // calls, and casting a negative error result to uint64 would otherwise make it look huge.
        const int64 PhysicalSize = Handle->Size();
        if (PhysicalSize < 0
            || Record.Offset > static_cast<uint64>(PhysicalSize)
            || Record.Size > static_cast<uint64>(PhysicalSize) - Record.Offset
            || !Handle->Seek(static_cast<int64>(Record.Offset)))
        {
            OutError = TEXT("The .dat record range is outside the current file");
            return false;
        }
        TArray<uint8> Bytes;
        Bytes.SetNumUninitialized(static_cast<int32>(Record.Size));
        if (!ReadExact(*Handle, Bytes.GetData(), Bytes.Num())
            || FCrc::MemCrc32(Bytes.GetData(), Bytes.Num()) != Record.Crc)
        {
            OutError = TEXT("The .dat record read or CRC validation failed");
            return false;
        }
        OutBytes = MoveTemp(Bytes);
        return true;
    }
}

FString FEntityArchiveStore::MakeArchivePath(const FString& WorldRoot)
{
    const FString Normalized = FSafeFileIO::NormalizeFilePath(WorldRoot);
    if (Normalized.IsEmpty()) return FString();
    const FString WorldName = FPaths::GetCleanFilename(Normalized);
    const FString WorldsRoot = FPaths::GetPath(Normalized);
    return WorldName.IsEmpty() ? FString()
        : FPaths::Combine(WorldsRoot, TEXT("Data"), WorldName + TEXT(".dat"));
}

TSharedPtr<FEntityArchiveStore, ESPMode::ThreadSafe> FEntityArchiveStore::Open(
    const FString& WorldRoot,
    bool bCreateIfMissing,
    FString& OutError)
{
    using namespace EntityArchivePrivate;
    OutError.Reset();
    const FString ArchivePath = FSafeFileIO::NormalizeFilePath(MakeArchivePath(WorldRoot));
    if (ArchivePath.IsEmpty())
    {
        OutError = TEXT("The explicit world root cannot produce a .dat path");
        return nullptr;
    }

    // Windows paths are case-insensitive even though FString/TMap keys are not. Fold only the
    // process registry key so two spelling variants cannot obtain independent writer mutexes.
    FString RegistryKey = ArchivePath;
#if PLATFORM_WINDOWS
    RegistryKey.ToLowerInline();
#endif

    FScopeLock RegistryLock(&StoreRegistryMutex);
    if (const TWeakPtr<FEntityArchiveStore, ESPMode::ThreadSafe>* ExistingWeak =
            StoreRegistry.Find(RegistryKey))
    {
        if (TSharedPtr<FEntityArchiveStore, ESPMode::ThreadSafe> Existing = ExistingWeak->Pin())
        {
            return Existing;
        }
        StoreRegistry.Remove(RegistryKey);
    }

    IFileManager& FileManager = IFileManager::Get();
    if (!FileManager.FileExists(*ArchivePath))
    {
        if (!bCreateIfMissing || !FileManager.MakeDirectory(*FPaths::GetPath(ArchivePath), true))
        {
            OutError = FString::Printf(TEXT("The .dat file is missing and cannot be created: %s"), *ArchivePath);
            return nullptr;
        }

        TArray<uint8> EmptyDirectory;
        FRecord EmptyState;
        TMap<FWorldChunkCoordinate, FRecord> EmptyChunks;
        if (!SerializeDirectory(EmptyState, EmptyChunks, EmptyDirectory, OutError)) return nullptr;

        FCommitFooter FirstFooter;
        FirstFooter.ReadMagic = FooterMagic;
        FirstFooter.Generation = 1;
        FirstFooter.DirectoryOffset = HeaderBytes;
        FirstFooter.DirectorySize = EmptyDirectory.Num();
        FirstFooter.DirectoryCrc = FCrc::MemCrc32(
            EmptyDirectory.GetData(), EmptyDirectory.Num());
        FirstFooter.CommittedSize = HeaderBytes + EmptyDirectory.Num() + CommitFooterBytes;
        const TArray<uint8> FooterBytes = SerializeFooter(FirstFooter);

        TArray<uint8> Header;
        FWriter HeaderWriter(Header);
        HeaderWriter.U64(Magic);
        HeaderWriter.U32(Version);
        HeaderWriter.U32(HeaderBytes);
        Header.SetNumZeroed(HeaderBytes, EAllowShrinking::No);
        Header.Append(EmptyDirectory);
        Header.Append(FooterBytes);
        if (!HeaderWriter.IsOk() || FooterBytes.Num() != CommitFooterBytes)
        {
            OutError = TEXT("The initial .dat header or commit footer could not be serialized");
            return nullptr;
        }
        const FSafeFileWriteResult Created = FSafeFileIO::SaveBinaryBlocking(
            Header, ArchivePath, MaxChunkPayloadBytes);
        if (!Created.IsSuccess())
        {
            OutError = Created.Error;
            return nullptr;
        }
    }

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*ArchivePath));
    if (!Handle.IsValid())
    {
        OutError = TEXT("The .dat file is unreadable or has an invalid physical size");
        return nullptr;
    }

    // Size() may consult the platform filesystem and may report a negative failure sentinel.
    // Capture it once before any allocation or unsigned offset validation.
    const int64 PhysicalSize = Handle->Size();
    if (PhysicalSize < HeaderBytes || PhysicalSize > MaxArchiveBytes)
    {
        OutError = TEXT("The .dat file is unreadable or has an invalid physical size");
        return nullptr;
    }
    TArray<uint8> Header;
    Header.SetNumUninitialized(HeaderBytes);
    if (!ReadExact(*Handle, Header.GetData(), Header.Num()))
    {
        OutError = TEXT("The .dat fixed header could not be read");
        return nullptr;
    }
    FReader HeaderReader(Header);
    const uint64 ReadMagic = HeaderReader.U64();
    const uint32 ReadVersion = HeaderReader.U32();
    const uint32 ReadHeaderSize = HeaderReader.U32();
    if (!HeaderReader.IsOk() || ReadMagic != Magic || ReadVersion != Version
        || ReadHeaderSize != HeaderBytes)
    {
        OutError = TEXT("The .dat magic, version or header size is invalid");
        return nullptr;
    }

    FCommitFooter BestFooter;
    FRecord BestState;
    TMap<FWorldChunkCoordinate, FRecord> BestChunks;
    FString FooterError;
    if (!FindLatestValidFooter(
            *Handle, PhysicalSize, BestFooter, BestState, BestChunks, FooterError))
    {
        OutError = FooterError.IsEmpty()
            ? TEXT("No valid .dat commit footer could be recovered")
            : FooterError;
        return nullptr;
    }

    TSharedPtr<FEntityArchiveStore, ESPMode::ThreadSafe> Result =
        MakeShared<FEntityArchiveStore, ESPMode::ThreadSafe>();
    Result->Path = ArchivePath;
    Result->ChunkRecords = MoveTemp(BestChunks);
    Result->RuntimeStateRecord = BestState;
    Result->Generation = BestFooter.Generation;
    Result->NextWriteOrder = BestFooter.Generation;
    StoreRegistry.Add(RegistryKey, Result);
    return Result;
}

uint64 FEntityArchiveStore::GetGeneration() const
{
    FScopeLock Lock(&Mutex);
    return Generation;
}

bool FEntityArchiveStore::ContainsChunk(
    const FWorldChunkCoordinate& Coordinate) const
{
    // The directory can be replaced by a worker commit, so even this read-only lookup takes the
    // store mutex. Callers use it to avoid scheduling an OS read for known-empty spatial cells.
    FScopeLock Lock(&Mutex);
    return ChunkRecords.Contains(Coordinate);
}

bool FEntityArchiveStore::LoadChunk(
    const FWorldChunkCoordinate& Coordinate,
    TArray<FWorldChunkObject>& OutObjects,
    FString& OutError) const
{
    using namespace EntityArchivePrivate;
    OutObjects.Reset();
    FRecord Record;
    {
        FScopeLock Lock(&Mutex);
        const FRecord* Found = ChunkRecords.Find(Coordinate);
        if (!Found) return true; // A missing spatial chunk is a valid empty chunk.
        Record = *Found;
    }
    TArray<uint8> Bytes;
    return ReadRecordPayload(Path, Record, MaxChunkPayloadBytes, Bytes, OutError)
        && DeserializeChunk(Bytes, OutObjects, OutError);
}

FSafeFileWriteResult FEntityArchiveStore::SaveChunk(
    const FWorldChunkCoordinate& Coordinate,
    const TArray<FWorldChunkObject>& Objects,
    uint64 WriteOrder)
{
    if (WriteOrder == 0) WriteOrder = ReserveChunkWrite(Coordinate);
    TMap<FWorldChunkCoordinate, TArray<FWorldChunkObject>> Chunks;
    Chunks.Add(Coordinate, Objects);
    TMap<FWorldChunkCoordinate, uint64> WriteOrders;
    WriteOrders.Add(Coordinate, WriteOrder);
    return SaveChunks(Chunks, WriteOrders);
}

FSafeFileWriteResult FEntityArchiveStore::SaveChunks(
    const TMap<FWorldChunkCoordinate, TArray<FWorldChunkObject>>& Chunks,
    const TMap<FWorldChunkCoordinate, uint64>& WriteOrders)
{
    using namespace EntityArchivePrivate;
    if (Chunks.IsEmpty() || Chunks.Num() != WriteOrders.Num())
    {
        return MakeWriteResult(Path, ESafeFileIOStatus::SerializeFailed,
            TEXT("A .dat chunk batch requires matching non-empty payload/order maps"));
    }
    if (Chunks.Num() > MaxChunks)
    {
        return MakeWriteResult(Path, ESafeFileIOStatus::SerializeFailed,
            TEXT("A .dat chunk batch exceeds the configured chunk-count safety limit"));
    }

    TMap<FWorldChunkCoordinate, TArray<uint8>> Payloads;
    int64 TotalPayloadBytes = 0;
    for (const TPair<FWorldChunkCoordinate, TArray<FWorldChunkObject>>& Pair : Chunks)
    {
        if (!WriteOrders.Contains(Pair.Key))
        {
            return MakeWriteResult(Path, ESafeFileIOStatus::SerializeFailed,
                TEXT("A .dat chunk batch is missing a reserved write order"));
        }
        TArray<uint8> Bytes;
        FString Error;
        if (!SerializeChunk(Pair.Value, Bytes, Error))
        {
            return MakeWriteResult(Path, ESafeFileIOStatus::SerializeFailed, Error);
        }
        // Check before addition. Besides protecting the result byte count, this prevents a
        // maliciously large batch from wrapping and bypassing the archive cap before commit.
        if (TotalPayloadBytes > MaxArchiveBytes - static_cast<int64>(Bytes.Num()))
        {
            return MakeWriteResult(Path, ESafeFileIOStatus::SerializeFailed,
                TEXT("A .dat chunk batch exceeds the archive-size safety limit"));
        }
        TotalPayloadBytes += static_cast<int64>(Bytes.Num());
        Payloads.Add(Pair.Key, MoveTemp(Bytes));
    }

    FString Error;
    if (!CommitChunkRecords(Payloads, WriteOrders, Error))
    {
        return MakeWriteResult(Path, ESafeFileIOStatus::CommitFailed, Error);
    }
    return MakeWriteResult(
        Path, ESafeFileIOStatus::Success, FString(), TotalPayloadBytes);
}

bool FEntityArchiveStore::LoadRuntimeState(
    FWorldRuntimeState& OutState,
    bool& bOutMissing,
    FString& OutError) const
{
    using namespace EntityArchivePrivate;
    OutState = FWorldRuntimeState();
    bOutMissing = false;
    FRecord Record;
    {
        FScopeLock Lock(&Mutex);
        Record = RuntimeStateRecord;
    }
    if (Record.Offset == 0 && Record.Size == 0)
    {
        bOutMissing = true;
        return true;
    }
    TArray<uint8> Bytes;
    return ReadRecordPayload(Path, Record, MaxStatePayloadBytes, Bytes, OutError)
        && DeserializeState(Bytes, OutState, OutError);
}

uint64 FEntityArchiveStore::ReserveChunkWrite(const FWorldChunkCoordinate& Coordinate)
{
    FScopeLock Lock(&Mutex);
    const uint64 Order = ++NextWriteOrder;
    LatestChunkWriteOrders.FindOrAdd(Coordinate) = Order;
    return Order;
}

uint64 FEntityArchiveStore::ReserveRuntimeStateWrite()
{
    FScopeLock Lock(&Mutex);
    const uint64 Order = ++NextWriteOrder;
    LatestRuntimeStateWriteOrder = Order;
    return Order;
}

FSafeFileWriteResult FEntityArchiveStore::SaveRuntimeState(
    const FWorldRuntimeState& State,
    uint64 WriteOrder)
{
    using namespace EntityArchivePrivate;
    if (WriteOrder == 0) WriteOrder = ReserveRuntimeStateWrite();
    TArray<uint8> Bytes;
    FString Error;
    if (!SerializeState(State, Bytes, Error))
        return MakeWriteResult(Path, ESafeFileIOStatus::SerializeFailed, Error);
    if (!CommitRuntimeState(Bytes, WriteOrder, Error))
        return MakeWriteResult(Path, ESafeFileIOStatus::CommitFailed, Error);
    return MakeWriteResult(Path, ESafeFileIOStatus::Success, FString(), Bytes.Num());
}

bool FEntityArchiveStore::CommitRuntimeState(
    const TArray<uint8>& Payload,
    uint64 WriteOrder,
    FString& OutError)
{
    using namespace EntityArchivePrivate;
    if (Payload.IsEmpty() || Payload.Num() > MaxStatePayloadBytes || WriteOrder == 0)
    {
        OutError = TEXT("A .dat runtime-state commit has an invalid payload or write order");
        return false;
    }

    FScopeLock Lock(&Mutex);
    if (LatestRuntimeStateWriteOrder != WriteOrder)
    {
        // A newer snapshot was reserved before this worker reached the file. Treat the obsolete
        // request as successfully superseded; publishing it would regress transform or velocity.
        return true;
    }
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    TUniquePtr<IFileHandle> Handle(PlatformFile.OpenWrite(*Path, true, true));
    if (!Handle.IsValid())
    {
        OutError = FString::Printf(TEXT("The .dat file cannot be opened for append: %s"), *Path);
        return false;
    }
    // OpenWrite(..., bAppend=true) owns the EOF position. Deliberately do not seek back into the
    // file: some POSIX implementations enforce O_APPEND and ignore a caller-selected write offset.
    const int64 AppendOffset = Handle->Size();
    if (AppendOffset < HeaderBytes || AppendOffset > MaxArchiveBytes
        || Payload.Num() > MaxArchiveBytes - AppendOffset)
    {
        OutError = TEXT("The .dat append offset or resulting file size is invalid");
        return false;
    }

    FRecord NewRecord;
    NewRecord.Offset = static_cast<uint64>(AppendOffset);
    NewRecord.Size = static_cast<uint64>(Payload.Num());
    NewRecord.Crc = FCrc::MemCrc32(Payload.GetData(), Payload.Num());

    TMap<FWorldChunkCoordinate, FRecord> CandidateChunks = ChunkRecords;
    const FRecord CandidateState = NewRecord;

    TArray<uint8> Directory;
    if (!SerializeDirectory(CandidateState, CandidateChunks, Directory, OutError)) return false;
    const int64 DirectoryOffset = AppendOffset + Payload.Num();
    const int64 NewCommittedSize = DirectoryOffset + Directory.Num();
    if (NewCommittedSize > MaxArchiveBytes - CommitFooterBytes
        || !WriteExact(*Handle, Payload.GetData(), Payload.Num())
        || !WriteExact(*Handle, Directory.GetData(), Directory.Num())
        || !Handle->Flush(true))
    {
        OutError = TEXT("The .dat payload/directory append could not be flushed");
        return false;
    }

    FCommitFooter Footer;
    Footer.ReadMagic = FooterMagic;
    Footer.Generation = Generation + 1;
    Footer.DirectoryOffset = static_cast<uint64>(DirectoryOffset);
    Footer.DirectorySize = static_cast<uint64>(Directory.Num());
    Footer.DirectoryCrc = FCrc::MemCrc32(Directory.GetData(), Directory.Num());
    Footer.CommittedSize = static_cast<uint64>(NewCommittedSize + CommitFooterBytes);
    const TArray<uint8> FooterBytes = SerializeFooter(Footer);
    if (FooterBytes.Num() != CommitFooterBytes
        || !WriteExact(*Handle, FooterBytes.GetData(), FooterBytes.Num())
        || !Handle->Flush(true))
    {
        OutError = TEXT("The .dat payload was appended but its commit footer could not be published");
        return false;
    }

    // Publish in-memory state only after the footer is durable. Readers either see this complete
    // generation or recover the previous footer; they never observe the uncommitted tail.
    ChunkRecords = MoveTemp(CandidateChunks);
    RuntimeStateRecord = CandidateState;
    Generation = Footer.Generation;
    return true;
}

bool FEntityArchiveStore::CommitChunkRecords(
    const TMap<FWorldChunkCoordinate, TArray<uint8>>& Payloads,
    const TMap<FWorldChunkCoordinate, uint64>& WriteOrders,
    FString& OutError)
{
    using namespace EntityArchivePrivate;
    if (Payloads.IsEmpty() || Payloads.Num() != WriteOrders.Num())
    {
        OutError = TEXT("A .dat batch commit requires matching payload and order maps");
        return false;
    }

    FScopeLock Lock(&Mutex);
    for (const TPair<FWorldChunkCoordinate, TArray<uint8>>& Pair : Payloads)
    {
        const uint64* Order = WriteOrders.Find(Pair.Key);
        if (!Order || *Order == 0 || Pair.Value.IsEmpty()
            || Pair.Value.Num() > MaxChunkPayloadBytes)
        {
            OutError = TEXT("A .dat batch contains an invalid payload or write order");
            return false;
        }
        if (LatestChunkWriteOrders.FindRef(Pair.Key) != *Order)
        {
            // Batch atomicity is more important than partially publishing still-current rows. A
            // newer reservation will publish the complete replacement snapshot.
            return true;
        }
    }

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    TUniquePtr<IFileHandle> Handle(PlatformFile.OpenWrite(*Path, true, true));
    if (!Handle.IsValid())
    {
        OutError = FString::Printf(
            TEXT("The .dat file cannot be opened for batch append: %s"), *Path);
        return false;
    }

    const int64 AppendOffset = Handle->Size();
    if (AppendOffset < HeaderBytes || AppendOffset > MaxArchiveBytes)
    {
        OutError = TEXT("The .dat batch append offset is invalid");
        return false;
    }

    TArray<FWorldChunkCoordinate> Coordinates;
    Payloads.GetKeys(Coordinates);
    Coordinates.Sort([](const FWorldChunkCoordinate& A, const FWorldChunkCoordinate& B)
    {
        if (A.X != B.X) return A.X < B.X;
        if (A.Y != B.Y) return A.Y < B.Y;
        return A.Z < B.Z;
    });

    TMap<FWorldChunkCoordinate, FRecord> CandidateChunks = ChunkRecords;
    int64 Cursor = AppendOffset;
    for (const FWorldChunkCoordinate& Coordinate : Coordinates)
    {
        const TArray<uint8>& Payload = Payloads.FindChecked(Coordinate);
        if (Cursor > MaxArchiveBytes - Payload.Num())
        {
            OutError = TEXT("The .dat batch payloads exceed the archive safety limit");
            return false;
        }
        FRecord Record;
        Record.Offset = static_cast<uint64>(Cursor);
        Record.Size = static_cast<uint64>(Payload.Num());
        Record.Crc = FCrc::MemCrc32(Payload.GetData(), Payload.Num());
        CandidateChunks.Add(Coordinate, Record);
        Cursor += Payload.Num();
    }

    TArray<uint8> Directory;
    if (!SerializeDirectory(RuntimeStateRecord, CandidateChunks, Directory, OutError)
        || Cursor > MaxArchiveBytes - Directory.Num() - CommitFooterBytes)
    {
        if (OutError.IsEmpty())
        {
            OutError = TEXT("The .dat batch directory exceeds the archive safety limit");
        }
        return false;
    }

    for (const FWorldChunkCoordinate& Coordinate : Coordinates)
    {
        const TArray<uint8>& Payload = Payloads.FindChecked(Coordinate);
        if (!WriteExact(*Handle, Payload.GetData(), Payload.Num()))
        {
            OutError = TEXT("A .dat batch payload could not be appended");
            return false;
        }
    }
    const int64 DirectoryOffset = Cursor;
    if (!WriteExact(*Handle, Directory.GetData(), Directory.Num())
        || !Handle->Flush(true))
    {
        OutError = TEXT("The .dat batch directory could not be flushed");
        return false;
    }

    FCommitFooter Footer;
    Footer.ReadMagic = FooterMagic;
    Footer.Generation = Generation + 1;
    Footer.DirectoryOffset = static_cast<uint64>(DirectoryOffset);
    Footer.DirectorySize = static_cast<uint64>(Directory.Num());
    Footer.DirectoryCrc = FCrc::MemCrc32(Directory.GetData(), Directory.Num());
    Footer.CommittedSize = static_cast<uint64>(
        DirectoryOffset + Directory.Num() + CommitFooterBytes);
    const TArray<uint8> FooterBytes = SerializeFooter(Footer);
    if (FooterBytes.Num() != CommitFooterBytes
        || !WriteExact(*Handle, FooterBytes.GetData(), FooterBytes.Num())
        || !Handle->Flush(true))
    {
        OutError = TEXT("The .dat batch footer could not be published");
        return false;
    }

    ChunkRecords = MoveTemp(CandidateChunks);
    Generation = Footer.Generation;
    return true;
}
