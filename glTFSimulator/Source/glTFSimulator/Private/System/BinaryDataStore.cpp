// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/BinaryDataStore.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Compression.h"
#include "Misc/Crc.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace BinaryDataStorePrivate
{
    constexpr uint32 EnvelopeMagic = 0x44534647u; // "GFSD" in little-endian byte order.
    constexpr uint16 SchemaVersion = 1;
    constexpr int32 HeaderBytes = 16;
    constexpr int32 MaxStringBytes = 4 * 1024 * 1024;
    constexpr int32 MaxPathBytes = 32768;
    constexpr int32 MaxNameBytes = 4096;
    constexpr int32 MaxModelMeshes = 500000;
    constexpr int32 MaxEntities = 1000000;
    constexpr int32 MaxPlayers = 10000;
    constexpr int32 MaxItemsPerPlayer = 65536;
    constexpr uint32 ModelSczMagic = 0x315A4353u; // "SCZ1" in little-endian byte order.
    constexpr uint16 ModelSczVersion = 1;
    constexpr uint16 ModelSczCodecZlib = 1;

    enum class EDatKind : uint16
    {
        Model = 1,
        Entities = 2,
        Players = 3,
        World = 4
    };

    static bool IsFiniteVector(const FVector& Value)
    {
        return !Value.ContainsNaN() &&
            FMath::IsFinite(Value.X) &&
            FMath::IsFinite(Value.Y) &&
            FMath::IsFinite(Value.Z);
    }

    static bool IsFiniteQuat(const FQuat& Value)
    {
        return FMath::IsFinite(Value.X) &&
            FMath::IsFinite(Value.Y) &&
            FMath::IsFinite(Value.Z) &&
            FMath::IsFinite(Value.W);
    }

    static bool IsFiniteTransform(const FTransform& Value)
    {
        const FQuat Rotation = Value.GetRotation();
        const FVector Scale = Value.GetScale3D();
        return !Value.ContainsNaN() &&
            IsFiniteVector(Value.GetLocation()) &&
            IsFiniteVector(Scale) &&
            IsFiniteQuat(Rotation) &&
            Rotation.IsNormalized() &&
            FMath::Abs(Scale.X) <= 1000000.0 &&
            FMath::Abs(Scale.Y) <= 1000000.0 &&
            FMath::Abs(Scale.Z) <= 1000000.0;
    }

    class FWriter
    {
    public:
        explicit FWriter(TArray<uint8>& InBytes) : Bytes(InBytes)
        {
            Bytes.Reset();
        }

        bool IsOk() const { return bOk; }

        void WriteU8(const uint8 Value)
        {
            if (!bOk)
            {
                return;
            }
            Bytes.Add(Value);
        }

        void WriteU16(const uint16 Value)
        {
            WriteU8(static_cast<uint8>(Value & 0xffu));
            WriteU8(static_cast<uint8>((Value >> 8u) & 0xffu));
        }

        void WriteU32(const uint32 Value)
        {
            WriteU8(static_cast<uint8>(Value & 0xffu));
            WriteU8(static_cast<uint8>((Value >> 8u) & 0xffu));
            WriteU8(static_cast<uint8>((Value >> 16u) & 0xffu));
            WriteU8(static_cast<uint8>((Value >> 24u) & 0xffu));
        }

        void WriteI32(const int32 Value)
        {
            WriteU32(static_cast<uint32>(Value));
        }

        void WriteU64(const uint64 Value)
        {
            for (int32 Shift = 0; Shift < 64; Shift += 8)
            {
                WriteU8(static_cast<uint8>((Value >> Shift) & 0xffull));
            }
        }

        void WriteFloat(const float Value)
        {
            uint32 Bits = 0;
            static_assert(sizeof(Bits) == sizeof(Value), "float must be 32-bit");
            FMemory::Memcpy(&Bits, &Value, sizeof(Value));
            WriteU32(Bits);
        }

        void WriteDouble(const double Value)
        {
            uint64 Bits = 0;
            static_assert(sizeof(Bits) == sizeof(Value), "double must be 64-bit");
            FMemory::Memcpy(&Bits, &Value, sizeof(Value));
            WriteU64(Bits);
        }

        void WriteString(const FString& Value, const int32 MaxBytes)
        {
            FTCHARToUTF8 Utf8(*Value);
            if (Utf8.Length() < 0 || Utf8.Length() > MaxBytes)
            {
                bOk = false;
                return;
            }
            WriteU32(static_cast<uint32>(Utf8.Length()));
            if (Utf8.Length() > 0 && bOk)
            {
                Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
            }
        }

        void WriteVector(const FVector& Value)
        {
            WriteDouble(Value.X);
            WriteDouble(Value.Y);
            WriteDouble(Value.Z);
        }

        void WriteQuat(const FQuat& Value)
        {
            WriteDouble(Value.X);
            WriteDouble(Value.Y);
            WriteDouble(Value.Z);
            WriteDouble(Value.W);
        }

        void WriteTransform(const FTransform& Value)
        {
            WriteVector(Value.GetLocation());
            WriteQuat(Value.GetRotation());
            WriteVector(Value.GetScale3D());
        }

        void Append(const TArray<uint8>& Value)
        {
            if (bOk)
            {
                Bytes.Append(Value);
            }
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
        int32 Remaining() const { return bOk ? Bytes.Num() - Offset : 0; }
        bool IsAtEnd() const { return bOk && Offset == Bytes.Num(); }

        uint8 ReadU8()
        {
            if (!Require(1))
            {
                return 0;
            }
            return Bytes[Offset++];
        }

        uint16 ReadU16()
        {
            const uint16 A = ReadU8();
            const uint16 B = ReadU8();
            return static_cast<uint16>(A | (B << 8u));
        }

        uint32 ReadU32()
        {
            const uint32 A = ReadU8();
            const uint32 B = ReadU8();
            const uint32 C = ReadU8();
            const uint32 D = ReadU8();
            return A | (B << 8u) | (C << 16u) | (D << 24u);
        }

        int32 ReadI32()
        {
            return static_cast<int32>(ReadU32());
        }

        uint64 ReadU64()
        {
            uint64 Value = 0;
            for (int32 Shift = 0; Shift < 64; Shift += 8)
            {
                Value |= static_cast<uint64>(ReadU8()) << Shift;
            }
            return Value;
        }

        float ReadFloat()
        {
            const uint32 Bits = ReadU32();
            float Value = 0.0f;
            FMemory::Memcpy(&Value, &Bits, sizeof(Value));
            return Value;
        }

        double ReadDouble()
        {
            const uint64 Bits = ReadU64();
            double Value = 0.0;
            FMemory::Memcpy(&Value, &Bits, sizeof(Value));
            return Value;
        }

        FString ReadString(const int32 MaxBytes)
        {
            const uint32 ByteCount = ReadU32();
            if (!bOk || ByteCount > static_cast<uint32>(MaxBytes) || !Require(static_cast<int32>(ByteCount)))
            {
                bOk = false;
                return FString();
            }
            if (ByteCount == 0)
            {
                return FString();
            }

            const ANSICHAR* Source = reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset);
            FUTF8ToTCHAR Converted(Source, static_cast<int32>(ByteCount));
            Offset += static_cast<int32>(ByteCount);
            if (Converted.Length() < 0)
            {
                bOk = false;
                return FString();
            }
            return FString(Converted.Length(), Converted.Get());
        }

        FVector ReadVector()
        {
            const double X = ReadDouble();
            const double Y = ReadDouble();
            const double Z = ReadDouble();
            return FVector(X, Y, Z);
        }

        FQuat ReadQuat()
        {
            const double X = ReadDouble();
            const double Y = ReadDouble();
            const double Z = ReadDouble();
            const double W = ReadDouble();
            return FQuat(X, Y, Z, W);
        }

        FTransform ReadTransform()
        {
            const FVector Location = ReadVector();
            FQuat Rotation = ReadQuat();
            const FVector Scale = ReadVector();
            if (bOk)
            {
                if (!IsFiniteQuat(Rotation) || Rotation.SizeSquared() <= SMALL_NUMBER)
                {
                    bOk = false;
                    Rotation = FQuat::Identity;
                }
                else if (!Rotation.IsNormalized())
                {
                    Rotation.Normalize();
                }
            }
            return FTransform(Rotation, Location, Scale);
        }

        TArray<uint8> ReadBytes(const int32 Count)
        {
            TArray<uint8> Result;
            if (!Require(Count))
            {
                return Result;
            }
            Result.Append(Bytes.GetData() + Offset, Count);
            Offset += Count;
            return Result;
        }

    private:
        bool Require(const int32 Count)
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

    static bool BuildEnvelope(const EDatKind Kind, const TArray<uint8>& Payload, TArray<uint8>& OutBytes, FString& OutError)
    {
        if (Payload.Num() < 0 || static_cast<uint64>(Payload.Num()) > MAX_uint32)
        {
            OutError = TEXT("Binary payload is too large for the DAT envelope");
            return false;
        }

        TArray<uint8> Header;
        FWriter Writer(Header);
        Writer.WriteU32(EnvelopeMagic);
        Writer.WriteU16(SchemaVersion);
        Writer.WriteU16(static_cast<uint16>(Kind));
        Writer.WriteU32(static_cast<uint32>(Payload.Num()));
        const uint32 Crc = Payload.IsEmpty() ? 0u : FCrc::MemCrc32(Payload.GetData(), Payload.Num());
        Writer.WriteU32(Crc);
        if (!Writer.IsOk() || Header.Num() != HeaderBytes)
        {
            OutError = TEXT("Failed to construct the DAT envelope header");
            return false;
        }

        OutBytes = MoveTemp(Header);
        OutBytes.Append(Payload);
        return true;
    }

    static bool ExtractPayload(const TArray<uint8>& Bytes, const EDatKind ExpectedKind, TArray<uint8>& OutPayload, FString& OutError)
    {
        if (Bytes.Num() < HeaderBytes)
        {
            OutError = TEXT("DAT file is shorter than its fixed header");
            return false;
        }

        FReader Reader(Bytes);
        const uint32 Magic = Reader.ReadU32();
        const uint16 Version = Reader.ReadU16();
        const uint16 Kind = Reader.ReadU16();
        const uint32 PayloadBytes = Reader.ReadU32();
        const uint32 ExpectedCrc = Reader.ReadU32();
        if (!Reader.IsOk() || Magic != EnvelopeMagic)
        {
            OutError = TEXT("DAT magic is invalid");
            return false;
        }
        if (Version != SchemaVersion)
        {
            OutError = FString::Printf(TEXT("Unsupported DAT schema version: %u"), Version);
            return false;
        }
        if (Kind != static_cast<uint16>(ExpectedKind))
        {
            OutError = TEXT("DAT file kind does not match the requested data type");
            return false;
        }
        if (PayloadBytes != static_cast<uint32>(Reader.Remaining()))
        {
            OutError = TEXT("DAT payload length does not match the file length");
            return false;
        }

        OutPayload = Reader.ReadBytes(static_cast<int32>(PayloadBytes));
        if (!Reader.IsAtEnd())
        {
            OutError = TEXT("DAT envelope contains trailing or truncated bytes");
            return false;
        }
        const uint32 ActualCrc = OutPayload.IsEmpty() ? 0u : FCrc::MemCrc32(OutPayload.GetData(), OutPayload.Num());
        if (ActualCrc != ExpectedCrc)
        {
            OutPayload.Reset();
            OutError = TEXT("DAT payload CRC validation failed");
            return false;
        }
        return true;
    }

    static bool JsonObjectToCompactString(const TSharedPtr<FJsonObject>& Json, FString& OutText)
    {
        OutText.Reset();
        const TSharedRef<FJsonObject> SafeJson = Json.IsValid() ? Json.ToSharedRef() : MakeShared<FJsonObject>();
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutText);
        return FJsonSerializer::Serialize(SafeJson, Writer);
    }

    static FSafeFileWriteResult MakeSerializationFailure(const FString& Path, const FString& Error)
    {
        FSafeFileWriteResult Result;
        Result.Status = ESafeFileIOStatus::SerializeFailed;
        Result.Path = FSafeFileIO::NormalizeFilePath(Path);
        Result.Error = Error;
        return Result;
    }

    static void RestoreValidatedDatPrimaryBestEffort(
        const FString& NormalizedPath,
        const TArray<uint8>& ValidatedBytes,
        const int64 MaxBytes)
    {
        IFileManager& FileManager = IFileManager::Get();
        if (FileManager.FileExists(*NormalizedPath) &&
            !FileManager.Delete(*NormalizedPath, false, true, true))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("Validated DAT backup was loaded but the corrupt primary could not be removed. Path=%s"),
                *NormalizedPath);
            return;
        }

        const FSafeFileWriteResult RestoreResult =
            FSafeFileIO::SaveBinaryBlocking(ValidatedBytes, NormalizedPath, MaxBytes);
        if (!RestoreResult.IsSuccess())
        {
            UE_LOG(LogTemp, Warning,
                TEXT("Validated DAT backup was loaded but could not be restored as the primary. Path=%s Reason=%s"),
                *NormalizedPath,
                *RestoreResult.Error);
        }
    }

    /**
     * FSafeFileIO can recover a binary backup when the primary cannot be read. DAT files also have
     * a schema and CRC, so a readable-but-corrupt primary needs one additional semantic fallback.
     * Every deserializer used here publishes its destination only after complete validation, making
     * it safe to call the parser again with the backup bytes.
     */
    template <typename ParseFunction>
    static bool LoadValidatedDatWithBackup(
        const FString& DatPath,
        const int64 MaxBytes,
        ParseFunction&& Parse,
        FString& OutError)
    {
        const FString NormalizedPath = FSafeFileIO::NormalizeFilePath(DatPath);
        const FSafeBinaryLoadResult Primary = FSafeFileIO::LoadBinaryBlocking(NormalizedPath, MaxBytes);
        if (!Primary.IsSuccess())
        {
            OutError = Primary.Error;
            return false;
        }

        FString PrimaryValidationError;
        if (Parse(Primary.Data, PrimaryValidationError))
        {
            if (Primary.Status == ESafeFileIOStatus::RecoveredFromBackup)
            {
                RestoreValidatedDatPrimaryBestEffort(NormalizedPath, Primary.Data, MaxBytes);
            }
            OutError.Reset();
            return true;
        }

        // LoadBinaryBlocking already returned the backup when the primary read itself failed.
        // In that case there is no other committed generation to try.
        if (Primary.Status == ESafeFileIOStatus::RecoveredFromBackup)
        {
            OutError = FString::Printf(
                TEXT("Recovered DAT backup also failed semantic validation: %s"),
                *PrimaryValidationError);
            return false;
        }

        const FString BackupPath = NormalizedPath + TEXT(".bak");
        const FSafeBinaryLoadResult Backup = FSafeFileIO::LoadBinaryBlocking(BackupPath, MaxBytes);
        FString BackupValidationError;
        if (Backup.IsSuccess() && Parse(Backup.Data, BackupValidationError))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("Primary DAT failed validation and the last committed backup was used. Path=%s Reason=%s"),
                *NormalizedPath,
                *PrimaryValidationError);
            RestoreValidatedDatPrimaryBestEffort(NormalizedPath, Backup.Data, MaxBytes);
            OutError.Reset();
            return true;
        }

        OutError = FString::Printf(
            TEXT("Primary DAT validation failed (%s); backup validation failed (%s)"),
            *PrimaryValidationError,
            Backup.IsSuccess() ? *BackupValidationError : *Backup.Error);
        return false;
    }
}

bool FModelCacheData::IsSane() const
{
    using namespace BinaryDataStorePrivate;
    if (ModelHash.IsEmpty() || ModelHash.Len() > 128 ||
        !IsFiniteVector(Center) || !IsFiniteVector(Extent) ||
        Extent.X < 0.0 || Extent.Y < 0.0 || Extent.Z < 0.0 ||
        MeshExtents.Num() < 0 || MeshExtents.Num() > MaxModelMeshes)
    {
        return false;
    }

    for (const TPair<FName, FVector>& Pair : MeshExtents)
    {
        if (Pair.Key.IsNone() || !IsFiniteVector(Pair.Value) ||
            Pair.Value.X < 0.0 || Pair.Value.Y < 0.0 || Pair.Value.Z < 0.0)
        {
            return false;
        }
    }
    return true;
}

bool FBinaryDataStore::ComputeFileSha1(const FString& FilePath, FString& OutHash, FString& OutError)
{
    OutHash.Reset();
    OutError.Reset();

    const FString NormalizedPath = FSafeFileIO::NormalizeFilePath(FilePath);
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*NormalizedPath));
    if (!Handle.IsValid())
    {
        OutError = FString::Printf(TEXT("Model file is missing or unreadable: %s"), *NormalizedPath);
        return false;
    }

    const int64 TotalSize = Handle->Size();
    if (TotalSize < 0)
    {
        OutError = TEXT("Could not determine model file size while hashing");
        return false;
    }

    constexpr int32 BufferSize = 256 * 1024;
    TArray<uint8> Buffer;
    Buffer.SetNumUninitialized(BufferSize);
    FSHA1 Sha;
    int64 ReadOffset = 0;
    while (ReadOffset < TotalSize)
    {
        const int32 ToRead = static_cast<int32>(FMath::Min<int64>(BufferSize, TotalSize - ReadOffset));
        if (ToRead <= 0 || !Handle->Read(Buffer.GetData(), ToRead))
        {
            OutError = TEXT("Model file read failed while hashing");
            return false;
        }
        Sha.Update(Buffer.GetData(), ToRead);
        ReadOffset += ToRead;
    }

    Sha.Final();
    uint8 Hash[20];
    Sha.GetHash(Hash);
    OutHash = BytesToHex(Hash, UE_ARRAY_COUNT(Hash)).ToUpper();
    return !OutHash.IsEmpty();
}

bool FBinaryDataStore::SerializeModelCache(const FModelCacheData& Cache, TArray<uint8>& OutBytes, FString& OutError)
{
    using namespace BinaryDataStorePrivate;
    if (!Cache.IsSane())
    {
        OutError = TEXT("Model cache contains invalid bounds, mesh entries, or hash data");
        return false;
    }

    TArray<uint8> RawPayload;
    FWriter RawWriter(RawPayload);
    RawWriter.WriteString(Cache.ModelHash, 128);
    RawWriter.WriteVector(Cache.Center);
    RawWriter.WriteVector(Cache.Extent);

    TArray<FName> Keys;
    Cache.MeshExtents.GetKeys(Keys);
    Keys.Sort([](const FName A, const FName B)
    {
        return A.LexicalLess(B);
    });
    RawWriter.WriteU32(static_cast<uint32>(Keys.Num()));
    for (const FName Key : Keys)
    {
        RawWriter.WriteString(Key.ToString(), MaxNameBytes);
        RawWriter.WriteVector(Cache.MeshExtents.FindChecked(Key));
    }
    if (!RawWriter.IsOk() || RawPayload.IsEmpty() ||
        static_cast<int64>(RawPayload.Num()) > MaxModelCacheRawBytes)
    {
        OutError = TEXT("Model cache serialization exceeded a bounded field or raw-size limit");
        return false;
    }

    // Use the 64-bit, failure-reporting overload. The legacy int32 overload can fatal when a
    // compression bound cannot be produced, which is unsuitable for an externally supplied model.
    int64 CompressionBound = 0;
    if (!FCompression::CompressMemoryBound(
            NAME_Zlib,
            CompressionBound,
            static_cast<int64>(RawPayload.Num()),
            0) ||
        CompressionBound <= 0 || CompressionBound > MaxModelSczBytes || CompressionBound > MAX_int32)
    {
        OutError = TEXT("Could not determine a safe zlib buffer size for the model SCZ cache");
        return false;
    }

    TArray<uint8> Compressed;
    Compressed.SetNumUninitialized(static_cast<int32>(CompressionBound));
    int64 CompressedBytes = CompressionBound;
    if (!FCompression::CompressMemory(
            NAME_Zlib,
            Compressed.GetData(),
            CompressedBytes,
            RawPayload.GetData(),
            static_cast<int64>(RawPayload.Num()),
            COMPRESS_NoFlags,
            0) ||
        CompressedBytes <= 0 || CompressedBytes > CompressionBound || CompressedBytes > MAX_int32)
    {
        OutError = TEXT("Zlib compression failed while building the model SCZ cache");
        return false;
    }
    Compressed.SetNum(static_cast<int32>(CompressedBytes), EAllowShrinking::No);

    TArray<uint8> SczPayload;
    FWriter SczWriter(SczPayload);
    SczWriter.WriteU32(ModelSczMagic);
    SczWriter.WriteU16(ModelSczVersion);
    SczWriter.WriteU16(ModelSczCodecZlib);
    SczWriter.WriteU64(static_cast<uint64>(RawPayload.Num()));
    SczWriter.WriteU64(static_cast<uint64>(Compressed.Num()));
    SczWriter.WriteU32(FCrc::MemCrc32(RawPayload.GetData(), RawPayload.Num()));
    SczWriter.Append(Compressed);
    if (!SczWriter.IsOk() || static_cast<int64>(SczPayload.Num()) > MaxModelSczBytes)
    {
        OutError = TEXT("Compressed model SCZ payload exceeded its bounded file-size limit");
        return false;
    }

    return BuildEnvelope(EDatKind::Model, SczPayload, OutBytes, OutError);
}

bool FBinaryDataStore::DeserializeModelCache(const TArray<uint8>& Bytes, FModelCacheData& OutCache, FString& OutError)
{
    using namespace BinaryDataStorePrivate;
    TArray<uint8> SczPayload;
    if (!ExtractPayload(Bytes, EDatKind::Model, SczPayload, OutError))
    {
        return false;
    }

    FReader SczReader(SczPayload);
    const uint32 SczMagic = SczReader.ReadU32();
    const uint16 SczVersion = SczReader.ReadU16();
    const uint16 SczCodec = SczReader.ReadU16();
    const uint64 RawBytes64 = SczReader.ReadU64();
    const uint64 CompressedBytes64 = SczReader.ReadU64();
    const uint32 ExpectedRawCrc = SczReader.ReadU32();
    if (!SczReader.IsOk() || SczMagic != ModelSczMagic || SczVersion != ModelSczVersion ||
        SczCodec != ModelSczCodecZlib || RawBytes64 == 0 ||
        RawBytes64 > static_cast<uint64>(MaxModelCacheRawBytes) ||
        CompressedBytes64 == 0 || CompressedBytes64 > static_cast<uint64>(MaxModelSczBytes) ||
        CompressedBytes64 != static_cast<uint64>(SczReader.Remaining()) ||
        RawBytes64 > static_cast<uint64>(MAX_int32) ||
        CompressedBytes64 > static_cast<uint64>(MAX_int32))
    {
        OutError = TEXT("Model SCZ header, codec, or bounded size fields are invalid");
        return false;
    }

    TArray<uint8> Compressed = SczReader.ReadBytes(static_cast<int32>(CompressedBytes64));
    if (!SczReader.IsAtEnd())
    {
        OutError = TEXT("Model SCZ cache contains trailing or truncated compressed bytes");
        return false;
    }

    TArray<uint8> RawPayload;
    RawPayload.SetNumUninitialized(static_cast<int32>(RawBytes64));
    if (!FCompression::UncompressMemory(
            NAME_Zlib,
            RawPayload.GetData(),
            static_cast<int64>(RawPayload.Num()),
            Compressed.GetData(),
            static_cast<int64>(Compressed.Num()),
            COMPRESS_NoFlags,
            0))
    {
        OutError = TEXT("Zlib decompression failed for the model SCZ cache");
        return false;
    }
    const uint32 ActualRawCrc = FCrc::MemCrc32(RawPayload.GetData(), RawPayload.Num());
    if (ActualRawCrc != ExpectedRawCrc)
    {
        OutError = TEXT("Model SCZ raw-payload CRC validation failed");
        return false;
    }

    FReader Reader(RawPayload);
    FModelCacheData Parsed;
    Parsed.ModelHash = Reader.ReadString(128).ToUpper();
    Parsed.Center = Reader.ReadVector();
    Parsed.Extent = Reader.ReadVector();
    const uint32 Count = Reader.ReadU32();
    if (!Reader.IsOk() || Count > static_cast<uint32>(MaxModelMeshes))
    {
        OutError = TEXT("Model cache mesh count exceeds the safety limit");
        return false;
    }

    Parsed.MeshExtents.Reserve(static_cast<int32>(Count));
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        const FString Name = Reader.ReadString(MaxNameBytes).TrimStartAndEnd();
        const FVector Extent = Reader.ReadVector();
        const FName Key(*Name);
        if (!Reader.IsOk() || Key.IsNone() || Parsed.MeshExtents.Contains(Key) ||
            !IsFiniteVector(Extent) || Extent.X < 0.0 || Extent.Y < 0.0 || Extent.Z < 0.0)
        {
            OutError = TEXT("Model cache contains an invalid or duplicate mesh extent entry");
            return false;
        }
        Parsed.MeshExtents.Add(Key, Extent);
    }

    if (!Reader.IsAtEnd() || !Parsed.IsSane())
    {
        OutError = TEXT("Model cache payload is truncated, has trailing data, or failed validation");
        return false;
    }
    OutCache = MoveTemp(Parsed);
    return true;
}

bool FBinaryDataStore::LoadModelCache(
    const FString& CachePath,
    const FString& ExpectedHash,
    FModelCacheData& OutCache,
    FString& OutError,
    bool& bOutHashMismatch)
{
    bOutHashMismatch = false;
    OutCache = FModelCacheData();
    if (!BinaryDataStorePrivate::LoadValidatedDatWithBackup(
            CachePath,
            MaxModelSczBytes,
            [&OutCache](const TArray<uint8>& Bytes, FString& ValidationError)
            {
                return FBinaryDataStore::DeserializeModelCache(Bytes, OutCache, ValidationError);
            },
            OutError))
    {
        return false;
    }
    if (!ExpectedHash.IsEmpty() && !OutCache.ModelHash.Equals(ExpectedHash, ESearchCase::IgnoreCase))
    {
        bOutHashMismatch = true;
        OutError = TEXT("Model hash differs from the cached SCZ hash");
        OutCache = FModelCacheData();
        return false;
    }
    return true;
}

FSafeFileWriteResult FBinaryDataStore::SaveModelCacheBlocking(const FString& CachePath, const FModelCacheData& Cache)
{
    TArray<uint8> Bytes;
    FString Error;
    if (!SerializeModelCache(Cache, Bytes, Error))
    {
        return BinaryDataStorePrivate::MakeSerializationFailure(CachePath, Error);
    }
    return FSafeFileIO::SaveBinaryBlocking(Bytes, CachePath, MaxModelSczBytes);
}

bool FBinaryDataStore::InvalidateCacheFile(const FString& CachePath, FString& OutError)
{
    OutError.Reset();
    const FString Normalized = FSafeFileIO::NormalizeFilePath(CachePath);
    if (Normalized.IsEmpty())
    {
        OutError = TEXT("SCZ cache path is empty");
        return false;
    }

    IFileManager& FileManager = IFileManager::Get();
    bool bOk = true;
    const TArray<FString> DirectPaths = { Normalized, Normalized + TEXT(".bak") };
    for (const FString& Path : DirectPaths)
    {
        if (FileManager.FileExists(*Path) && !FileManager.Delete(*Path, false, true, true))
        {
            bOk = false;
            OutError += FString::Printf(TEXT("Failed to delete stale SCZ generation: %s\n"), *Path);
        }
    }

    TArray<FString> TemporaryNames;
    const FString Directory = FPaths::GetPath(Normalized);
    const FString Pattern = FPaths::GetCleanFilename(Normalized) + TEXT(".tmp.*");
    FileManager.FindFiles(TemporaryNames, *FPaths::Combine(Directory, Pattern), true, false);
    for (const FString& FileName : TemporaryNames)
    {
        const FString FullPath = FPaths::Combine(Directory, FileName);
        if (!FileManager.Delete(*FullPath, false, true, true))
        {
            bOk = false;
            OutError += FString::Printf(TEXT("Failed to delete stale SCZ transaction: %s\n"), *FullPath);
        }
    }
    OutError.TrimEndInline();
    return bOk;
}

bool FBinaryDataStore::SerializeEntities(const TArray<FPlacedObjectRecord>& Records, TArray<uint8>& OutBytes, FString& OutError)
{
    using namespace BinaryDataStorePrivate;
    if (Records.Num() < 0 || Records.Num() > MaxEntities)
    {
        OutError = TEXT("Entity count exceeds the safety limit");
        return false;
    }

    TArray<uint8> Payload;
    FWriter Writer(Payload);
    Writer.WriteU32(static_cast<uint32>(Records.Num()));
    for (const FPlacedObjectRecord& Record : Records)
    {
        if (Record.ObjectName.IsEmpty() || !IsFiniteTransform(Record.Transform) ||
            (Record.Kind != EPlacedObjectKind::Prefab && Record.Kind != EPlacedObjectKind::Vehicle))
        {
            OutError = TEXT("Entity record contains an unsupported kind, invalid transform, or empty name");
            return false;
        }
        Writer.WriteString(Record.ObjectName, MaxNameBytes);
        Writer.WriteString(Record.BaseName, MaxNameBytes);
        Writer.WriteString(Record.SourceFile, MaxPathBytes);
        Writer.WriteU8(static_cast<uint8>(Record.Kind));
        Writer.WriteTransform(Record.Transform);
    }
    if (!Writer.IsOk())
    {
        OutError = TEXT("Entity serialization exceeded a bounded field limit");
        return false;
    }
    return BuildEnvelope(EDatKind::Entities, Payload, OutBytes, OutError);
}

bool FBinaryDataStore::DeserializeEntities(const TArray<uint8>& Bytes, TArray<FPlacedObjectRecord>& OutRecords, FString& OutError)
{
    using namespace BinaryDataStorePrivate;
    TArray<uint8> Payload;
    if (!ExtractPayload(Bytes, EDatKind::Entities, Payload, OutError))
    {
        return false;
    }

    FReader Reader(Payload);
    const uint32 Count = Reader.ReadU32();
    if (!Reader.IsOk() || Count > static_cast<uint32>(MaxEntities))
    {
        OutError = TEXT("Entity count exceeds the safety limit");
        return false;
    }

    TArray<FPlacedObjectRecord> Parsed;
    Parsed.Reserve(static_cast<int32>(Count));
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        FPlacedObjectRecord Record;
        Record.ObjectName = Reader.ReadString(MaxNameBytes).TrimStartAndEnd();
        Record.BaseName = Reader.ReadString(MaxNameBytes).TrimStartAndEnd();
        Record.SourceFile = Reader.ReadString(MaxPathBytes).TrimStartAndEnd();
        Record.Kind = static_cast<EPlacedObjectKind>(Reader.ReadU8());
        Record.Transform = Reader.ReadTransform();
        if (!Reader.IsOk() || Record.ObjectName.IsEmpty() || !IsFiniteTransform(Record.Transform) ||
            (Record.Kind != EPlacedObjectKind::Prefab && Record.Kind != EPlacedObjectKind::Vehicle))
        {
            OutError = FString::Printf(TEXT("Invalid entity record at index %u"), Index);
            return false;
        }
        Parsed.Add(MoveTemp(Record));
    }
    if (!Reader.IsAtEnd())
    {
        OutError = TEXT("Entity DAT is truncated or contains trailing data");
        return false;
    }
    OutRecords = MoveTemp(Parsed);
    return true;
}

bool FBinaryDataStore::LoadEntities(const FString& DatPath, TArray<FPlacedObjectRecord>& OutRecords, FString& OutError)
{
    OutRecords.Reset();
    return BinaryDataStorePrivate::LoadValidatedDatWithBackup(
        DatPath,
        MaxEntitiesDatBytes,
        [&OutRecords](const TArray<uint8>& Bytes, FString& ValidationError)
        {
            return FBinaryDataStore::DeserializeEntities(Bytes, OutRecords, ValidationError);
        },
        OutError);
}

FSafeFileWriteResult FBinaryDataStore::SaveEntitiesBlocking(const FString& DatPath, const TArray<FPlacedObjectRecord>& Records)
{
    TArray<uint8> Bytes;
    FString Error;
    if (!SerializeEntities(Records, Bytes, Error))
    {
        return BinaryDataStorePrivate::MakeSerializationFailure(DatPath, Error);
    }
    return FSafeFileIO::SaveBinaryBlocking(Bytes, DatPath, MaxEntitiesDatBytes);
}

void FBinaryDataStore::SaveEntitiesAsync(
    const FString& DatPath,
    const TArray<FPlacedObjectRecord>& Records,
    FSafeFileIO::FWriteCallback Callback)
{
    TArray<uint8> Bytes;
    FString Error;
    if (!SerializeEntities(Records, Bytes, Error))
    {
        FSafeFileWriteResult Result = BinaryDataStorePrivate::MakeSerializationFailure(DatPath, Error);
        if (Callback)
        {
            Callback(MoveTemp(Result));
        }
        return;
    }
    FSafeFileIO::SaveBinaryAsync(Bytes, DatPath, MaxEntitiesDatBytes, MoveTemp(Callback));
}

bool FBinaryDataStore::SerializePlayers(const UPlayerData* Data, TArray<uint8>& OutBytes, FString& OutError)
{
    using namespace BinaryDataStorePrivate;
    if (!IsValid(Data) || Data->Players.Num() < 0 || Data->Players.Num() > MaxPlayers)
    {
        OutError = TEXT("Player data is invalid or exceeds the player-count limit");
        return false;
    }

    TArray<uint8> Payload;
    FWriter Writer(Payload);
    Writer.WriteString(Data->Version, MaxNameBytes);
    Writer.WriteU32(static_cast<uint32>(Data->Players.Num()));
    for (const FWorldPlayerRecord& Record : Data->Players)
    {
        if (Record.PlayerId.IsEmpty() || !IsFiniteVector(Record.Location) ||
            !FMath::IsFinite(Record.Rotation.Pitch) || !FMath::IsFinite(Record.Rotation.Yaw) ||
            !FMath::IsFinite(Record.Rotation.Roll) || !FMath::IsFinite(Record.Health) ||
            Record.Items.Num() < 0 || Record.Items.Num() > MaxItemsPerPlayer)
        {
            OutError = TEXT("Player record contains invalid numeric data or too many items");
            return false;
        }

        Writer.WriteString(Record.PlayerId, MaxNameBytes);
        Writer.WriteString(Record.DisplayName, MaxNameBytes);
        Writer.WriteVector(Record.Location);
        Writer.WriteDouble(Record.Rotation.Pitch);
        Writer.WriteDouble(Record.Rotation.Yaw);
        Writer.WriteDouble(Record.Rotation.Roll);
        Writer.WriteFloat(Record.Health);
        Writer.WriteI32(FMath::Max(1, Record.Level));
        Writer.WriteString(Record.PlayerGameMode, MaxNameBytes);
        Writer.WriteU32(static_cast<uint32>(Record.Items.Num()));
        for (const FString& Item : Record.Items)
        {
            Writer.WriteString(Item, MaxPathBytes);
        }

        FString CustomText;
        if (!JsonObjectToCompactString(Record.CustomJson, CustomText))
        {
            OutError = TEXT("Player Custom JSON could not be serialized into the binary record");
            return false;
        }
        Writer.WriteString(CustomText, MaxStringBytes);
    }
    if (!Writer.IsOk())
    {
        OutError = TEXT("Player serialization exceeded a bounded field limit");
        return false;
    }
    return BuildEnvelope(EDatKind::Players, Payload, OutBytes, OutError);
}

bool FBinaryDataStore::DeserializePlayers(const TArray<uint8>& Bytes, UPlayerData* OutData, FString& OutError)
{
    using namespace BinaryDataStorePrivate;
    if (!IsValid(OutData))
    {
        OutError = TEXT("Destination UPlayerData is invalid");
        return false;
    }

    TArray<uint8> Payload;
    if (!ExtractPayload(Bytes, EDatKind::Players, Payload, OutError))
    {
        return false;
    }

    FReader Reader(Payload);
    FString ParsedVersion = Reader.ReadString(MaxNameBytes);
    const uint32 Count = Reader.ReadU32();
    if (!Reader.IsOk() || Count > static_cast<uint32>(MaxPlayers))
    {
        OutError = TEXT("Player count exceeds the safety limit");
        return false;
    }

    TArray<FWorldPlayerRecord> ParsedPlayers;
    ParsedPlayers.Reserve(static_cast<int32>(Count));
    TSet<FString> SeenIds;
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        FWorldPlayerRecord Record;
        Record.PlayerId = Reader.ReadString(MaxNameBytes).TrimStartAndEnd();
        Record.DisplayName = Reader.ReadString(MaxNameBytes).TrimStartAndEnd();
        Record.Location = Reader.ReadVector();
        Record.Rotation = FRotator(Reader.ReadDouble(), Reader.ReadDouble(), Reader.ReadDouble()).GetNormalized();
        Record.Health = Reader.ReadFloat();
        Record.Level = Reader.ReadI32();
        Record.PlayerGameMode = Reader.ReadString(MaxNameBytes);
        const uint32 ItemCount = Reader.ReadU32();
        if (!Reader.IsOk() || Record.PlayerId.IsEmpty() || SeenIds.Contains(Record.PlayerId.ToLower()) ||
            ItemCount > static_cast<uint32>(MaxItemsPerPlayer) || !IsFiniteVector(Record.Location) ||
            !FMath::IsFinite(Record.Rotation.Pitch) || !FMath::IsFinite(Record.Rotation.Yaw) ||
            !FMath::IsFinite(Record.Rotation.Roll) || !FMath::IsFinite(Record.Health) || Record.Level < 1)
        {
            OutError = FString::Printf(TEXT("Invalid player record at index %u"), Index);
            return false;
        }

        Record.Items.Reserve(static_cast<int32>(ItemCount));
        for (uint32 ItemIndex = 0; ItemIndex < ItemCount; ++ItemIndex)
        {
            Record.Items.Add(Reader.ReadString(MaxPathBytes));
        }
        const FString CustomText = Reader.ReadString(MaxStringBytes);
        if (!Reader.IsOk())
        {
            OutError = FString::Printf(TEXT("Truncated player record at index %u"), Index);
            return false;
        }

        FSafeJsonLimits JsonLimits;
        JsonLimits.MaxFileBytes = MaxStringBytes;
        JsonLimits.MaxDepth = 32;
        JsonLimits.MaxValues = 100000;
        JsonLimits.MaxContainerEntries = 50000;
        JsonLimits.MaxStringCharacters = MaxStringBytes;
        const FSafeJsonLoadResult CustomResult = FSafeFileIO::ParseJsonText(
            CustomText.IsEmpty() ? FString(TEXT("{}")) : CustomText,
            FString::Printf(TEXT("players.dat Custom[%u]"), Index),
            JsonLimits);
        if (!CustomResult.IsSuccess())
        {
            OutError = FString::Printf(TEXT("Invalid embedded Custom JSON in player record %u: %s"), Index, *CustomResult.Error);
            return false;
        }
        Record.CustomJson = CustomResult.JsonObject;
        SeenIds.Add(Record.PlayerId.ToLower());
        ParsedPlayers.Add(MoveTemp(Record));
    }

    if (!Reader.IsAtEnd())
    {
        OutError = TEXT("Player DAT is truncated or contains trailing data");
        return false;
    }
    OutData->Version = ParsedVersion.IsEmpty() ? FString(TEXT("1")) : MoveTemp(ParsedVersion);
    OutData->Players = MoveTemp(ParsedPlayers);
    return true;
}

bool FBinaryDataStore::LoadPlayers(const FString& DatPath, UPlayerData* OutData, FString& OutError)
{
    return BinaryDataStorePrivate::LoadValidatedDatWithBackup(
        DatPath,
        MaxPlayersDatBytes,
        [OutData](const TArray<uint8>& Bytes, FString& ValidationError)
        {
            return FBinaryDataStore::DeserializePlayers(Bytes, OutData, ValidationError);
        },
        OutError);
}

FSafeFileWriteResult FBinaryDataStore::SavePlayersBlocking(const FString& DatPath, const UPlayerData* Data)
{
    TArray<uint8> Bytes;
    FString Error;
    if (!SerializePlayers(Data, Bytes, Error))
    {
        return BinaryDataStorePrivate::MakeSerializationFailure(DatPath, Error);
    }
    return FSafeFileIO::SaveBinaryBlocking(Bytes, DatPath, MaxPlayersDatBytes);
}

void FBinaryDataStore::SavePlayersAsync(
    const FString& DatPath,
    const UPlayerData* Data,
    FSafeFileIO::FWriteCallback Callback)
{
    TArray<uint8> Bytes;
    FString Error;
    if (!SerializePlayers(Data, Bytes, Error))
    {
        FSafeFileWriteResult Result = BinaryDataStorePrivate::MakeSerializationFailure(DatPath, Error);
        if (Callback)
        {
            Callback(MoveTemp(Result));
        }
        return;
    }
    FSafeFileIO::SaveBinaryAsync(Bytes, DatPath, MaxPlayersDatBytes, MoveTemp(Callback));
}

bool FBinaryDataStore::SerializeWorldRuntime(const FWorldRuntimeData& Data, TArray<uint8>& OutBytes, FString& OutError)
{
    using namespace BinaryDataStorePrivate;
    if (!FMath::IsFinite(Data.WorldTime))
    {
        OutError = TEXT("World time is not finite");
        return false;
    }

    TArray<uint8> Payload;
    FWriter Writer(Payload);
    Writer.WriteFloat(Data.WorldTime);
    Writer.WriteString(Data.SelectedPlayer, MaxPathBytes);
    if (!Writer.IsOk())
    {
        OutError = TEXT("World runtime serialization exceeded a bounded field limit");
        return false;
    }
    return BuildEnvelope(EDatKind::World, Payload, OutBytes, OutError);
}

bool FBinaryDataStore::DeserializeWorldRuntime(const TArray<uint8>& Bytes, FWorldRuntimeData& OutData, FString& OutError)
{
    using namespace BinaryDataStorePrivate;
    TArray<uint8> Payload;
    if (!ExtractPayload(Bytes, EDatKind::World, Payload, OutError))
    {
        return false;
    }

    FReader Reader(Payload);
    FWorldRuntimeData Parsed;
    Parsed.WorldTime = Reader.ReadFloat();
    Parsed.SelectedPlayer = Reader.ReadString(MaxPathBytes).TrimStartAndEnd();
    if (!Reader.IsAtEnd() || !FMath::IsFinite(Parsed.WorldTime))
    {
        OutError = TEXT("World runtime DAT is invalid, truncated, or contains trailing data");
        return false;
    }
    OutData = MoveTemp(Parsed);
    return true;
}

bool FBinaryDataStore::LoadWorldRuntime(const FString& DatPath, FWorldRuntimeData& OutData, FString& OutError)
{
    return BinaryDataStorePrivate::LoadValidatedDatWithBackup(
        DatPath,
        MaxWorldDatBytes,
        [&OutData](const TArray<uint8>& Bytes, FString& ValidationError)
        {
            return FBinaryDataStore::DeserializeWorldRuntime(Bytes, OutData, ValidationError);
        },
        OutError);
}

FSafeFileWriteResult FBinaryDataStore::SaveWorldRuntimeBlocking(const FString& DatPath, const FWorldRuntimeData& Data)
{
    TArray<uint8> Bytes;
    FString Error;
    if (!SerializeWorldRuntime(Data, Bytes, Error))
    {
        return BinaryDataStorePrivate::MakeSerializationFailure(DatPath, Error);
    }
    return FSafeFileIO::SaveBinaryBlocking(Bytes, DatPath, MaxWorldDatBytes);
}

void FBinaryDataStore::SaveWorldRuntimeAsync(
    const FString& DatPath,
    const FWorldRuntimeData& Data,
    FSafeFileIO::FWriteCallback Callback)
{
    TArray<uint8> Bytes;
    FString Error;
    if (!SerializeWorldRuntime(Data, Bytes, Error))
    {
        FSafeFileWriteResult Result = BinaryDataStorePrivate::MakeSerializationFailure(DatPath, Error);
        if (Callback)
        {
            Callback(MoveTemp(Result));
        }
        return;
    }
    FSafeFileIO::SaveBinaryAsync(Bytes, DatPath, MaxWorldDatBytes, MoveTemp(Callback));
}
