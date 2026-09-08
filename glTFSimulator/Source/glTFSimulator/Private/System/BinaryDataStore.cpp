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
    constexpr uint16 SchemaVersion = 2;
    constexpr int32 HeaderBytes = 16;
    constexpr int32 MaxStringBytes = 4 * 1024 * 1024;
    constexpr int32 MaxPathBytes = 32768;
    constexpr int32 MaxNameBytes = 4096;
    constexpr int32 MaxModelMeshes = 500000;
    constexpr int32 MaxPlayers = 10000;
    constexpr int32 MaxItemsPerPlayer = 65536;
    constexpr int32 MaxChunkObjects = 1000000;
    constexpr int32 MaxDatabaseEntries = 1000000;
    constexpr uint32 ModelCacheMagic = 0x3148434Du; // "MCH1" in little-endian byte order.
    constexpr uint16 ModelCacheVersion = 3;
    constexpr uint16 ModelCacheCodecZlib = 1;

    enum class EDatKind : uint16
    {
        Model = 1,
        WorldChunk = 2,
        ModelDatabase = 3,
        Level = 4,
        SceneDatabase = 5
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

        void WriteGuid(const FGuid& Value)
        {
            WriteU32(Value.A); WriteU32(Value.B); WriteU32(Value.C); WriteU32(Value.D);
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

        FGuid ReadGuid()
        {
            return FGuid(ReadU32(), ReadU32(), ReadU32(), ReadU32());
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

    /** Reads exactly one primary DAT and publishes data only after schema/length/CRC validation. */
    template <typename ParseFunction>
    static bool LoadValidatedDat(
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
        return Parse(Primary.Data, OutError);
    }
}

bool FModelCacheData::IsSane() const
{
    using namespace BinaryDataStorePrivate;
    if (ModelHash.IsEmpty() || ModelHash.Len() > 128 || DefinitionJson.IsEmpty()
        || DefinitionJson.Len() > MaxStringBytes ||
        !IsFiniteVector(Center) || !IsFiniteVector(Extent) ||
        Extent.X < 0.0 || Extent.Y < 0.0 || Extent.Z < 0.0 ||
        MeshSizes.Num() < 0 || MeshSizes.Num() > MaxModelMeshes)
    {
        return false;
    }

    for (const TPair<FName, FVector>& Pair : MeshSizes)
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
    RawWriter.WriteString(Cache.DefinitionJson, MaxStringBytes);
    RawWriter.WriteVector(Cache.Center);
    RawWriter.WriteVector(Cache.Extent);

    TArray<FName> Keys;
    Cache.MeshSizes.GetKeys(Keys);
    Keys.Sort([](const FName A, const FName B)
    {
        return A.LexicalLess(B);
    });
    RawWriter.WriteU32(static_cast<uint32>(Keys.Num()));
    for (const FName Key : Keys)
    {
        RawWriter.WriteString(Key.ToString(), MaxNameBytes);
        RawWriter.WriteVector(Cache.MeshSizes.FindChecked(Key));
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
        CompressionBound <= 0 || CompressionBound > MaxModelCacheBytes || CompressionBound > MAX_int32)
    {
        OutError = TEXT("Could not determine a safe zlib buffer size for the model cache");
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
        OutError = TEXT("Zlib compression failed while building the model cache");
        return false;
    }
    Compressed.SetNum(static_cast<int32>(CompressedBytes), EAllowShrinking::No);

    TArray<uint8> CachePayload;
    FWriter CacheWriter(CachePayload);
    CacheWriter.WriteU32(ModelCacheMagic);
    CacheWriter.WriteU16(ModelCacheVersion);
    CacheWriter.WriteU16(ModelCacheCodecZlib);
    CacheWriter.WriteU64(static_cast<uint64>(RawPayload.Num()));
    CacheWriter.WriteU64(static_cast<uint64>(Compressed.Num()));
    CacheWriter.WriteU32(FCrc::MemCrc32(RawPayload.GetData(), RawPayload.Num()));
    CacheWriter.Append(Compressed);
    if (!CacheWriter.IsOk() || static_cast<int64>(CachePayload.Num()) > MaxModelCacheBytes)
    {
        OutError = TEXT("Compressed model-cache payload exceeded its bounded file-size limit");
        return false;
    }

    return BuildEnvelope(EDatKind::Model, CachePayload, OutBytes, OutError);
}

bool FBinaryDataStore::DeserializeModelCache(const TArray<uint8>& Bytes, FModelCacheData& OutCache, FString& OutError)
{
    using namespace BinaryDataStorePrivate;
    TArray<uint8> CachePayload;
    if (!ExtractPayload(Bytes, EDatKind::Model, CachePayload, OutError))
    {
        return false;
    }

    FReader CacheReader(CachePayload);
    const uint32 CacheMagic = CacheReader.ReadU32();
    const uint16 CacheVersion = CacheReader.ReadU16();
    const uint16 CacheCodec = CacheReader.ReadU16();
    const uint64 RawBytes64 = CacheReader.ReadU64();
    const uint64 CompressedBytes64 = CacheReader.ReadU64();
    const uint32 ExpectedRawCrc = CacheReader.ReadU32();
    if (!CacheReader.IsOk() || CacheMagic != ModelCacheMagic || CacheVersion != ModelCacheVersion ||
        CacheCodec != ModelCacheCodecZlib || RawBytes64 == 0 ||
        RawBytes64 > static_cast<uint64>(MaxModelCacheRawBytes) ||
        CompressedBytes64 == 0 || CompressedBytes64 > static_cast<uint64>(MaxModelCacheBytes) ||
        CompressedBytes64 != static_cast<uint64>(CacheReader.Remaining()) ||
        RawBytes64 > static_cast<uint64>(MAX_int32) ||
        CompressedBytes64 > static_cast<uint64>(MAX_int32))
    {
        OutError = TEXT("Model-cache header, codec, or bounded size fields are invalid");
        return false;
    }

    TArray<uint8> Compressed = CacheReader.ReadBytes(static_cast<int32>(CompressedBytes64));
    if (!CacheReader.IsAtEnd())
    {
        OutError = TEXT("Model cache contains trailing or truncated compressed bytes");
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
        OutError = TEXT("Zlib decompression failed for the model cache");
        return false;
    }
    const uint32 ActualRawCrc = FCrc::MemCrc32(RawPayload.GetData(), RawPayload.Num());
    if (ActualRawCrc != ExpectedRawCrc)
    {
        OutError = TEXT("Model-cache raw-payload CRC validation failed");
        return false;
    }

    FReader Reader(RawPayload);
    FModelCacheData Parsed;
    Parsed.ModelHash = Reader.ReadString(128).ToUpper();
    Parsed.DefinitionJson = Reader.ReadString(MaxStringBytes);
    Parsed.Center = Reader.ReadVector();
    Parsed.Extent = Reader.ReadVector();
    const uint32 Count = Reader.ReadU32();
    if (!Reader.IsOk() || Count > static_cast<uint32>(MaxModelMeshes))
    {
        OutError = TEXT("Model cache mesh count exceeds the safety limit");
        return false;
    }

    Parsed.MeshSizes.Reserve(static_cast<int32>(Count));
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        const FString Name = Reader.ReadString(MaxNameBytes).TrimStartAndEnd();
        const FVector Size = Reader.ReadVector();
        const FName Key(*Name);
        if (!Reader.IsOk() || Key.IsNone() || Parsed.MeshSizes.Contains(Key) ||
            !IsFiniteVector(Size) || Size.X < 0.0 || Size.Y < 0.0 || Size.Z < 0.0)
        {
            OutError = TEXT("Model cache contains an invalid or duplicate full mesh-size entry");
            return false;
        }
        Parsed.MeshSizes.Add(Key, Size);
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
    if (!BinaryDataStorePrivate::LoadValidatedDat(
            CachePath,
            MaxModelCacheBytes,
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
        OutError = TEXT("Model hash differs from the cached model hash");
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
    return FSafeFileIO::SaveBinaryBlocking(Bytes, CachePath, MaxModelCacheBytes);
}

bool FBinaryDataStore::InvalidateCacheFile(const FString& CachePath, FString& OutError)
{
    OutError.Reset();
    const FString Normalized = FSafeFileIO::NormalizeFilePath(CachePath);
    if (Normalized.IsEmpty())
    {
        OutError = TEXT("Model-cache path is empty");
        return false;
    }

    IFileManager& FileManager = IFileManager::Get();
    bool bOk = true;
    const TArray<FString> DirectPaths = { Normalized };
    for (const FString& Path : DirectPaths)
    {
        if (FileManager.FileExists(*Path) && !FileManager.Delete(*Path, false, true, true))
        {
            bOk = false;
            OutError += FString::Printf(TEXT("Failed to delete stale cache generation: %s\n"), *Path);
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
            OutError += FString::Printf(TEXT("Failed to delete stale cache transaction: %s\n"), *FullPath);
        }
    }
    OutError.TrimEndInline();
    return bOk;
}

bool FBinaryDataStore::SerializeWorldChunk(
    const TArray<FWorldChunkObject>& Objects,
    TArray<uint8>& OutBytes,
    FString& OutError)
{
    using namespace BinaryDataStorePrivate;
    if (Objects.Num() > MaxChunkObjects)
    {
        OutError = TEXT("World chunk exceeds the object-count safety limit");
        return false;
    }

    TArray<uint8> Payload;
    FWriter Writer(Payload);
    Writer.WriteU32(static_cast<uint32>(Objects.Num()));
    for (const FWorldChunkObject& Object : Objects)
    {
        const bool bValid = Object.UUID.IsValid() && IsFiniteVector(Object.Location)
            && IsFiniteQuat(Object.Rotation) && Object.Rotation.IsNormalized()
            && IsFiniteVector(Object.Scale) && IsFiniteVector(Object.Velocity)
            && IsFiniteVector(Object.AngularVelocity);
        if (!bValid)
        {
            OutError = TEXT("World chunk contains an invalid UUID or transform/velocity");
            return false;
        }
        Writer.WriteGuid(Object.UUID);
        Writer.WriteVector(Object.Location);
        Writer.WriteQuat(Object.Rotation);
        Writer.WriteVector(Object.Scale);
        Writer.WriteVector(Object.Velocity);
        Writer.WriteVector(Object.AngularVelocity);
    }
    if (!Writer.IsOk())
    {
        OutError = TEXT("World chunk serialization failed");
        return false;
    }
    return BuildEnvelope(EDatKind::WorldChunk, Payload, OutBytes, OutError);
}

bool FBinaryDataStore::DeserializeWorldChunk(
    const TArray<uint8>& Bytes,
    TArray<FWorldChunkObject>& OutObjects,
    FString& OutError)
{
    using namespace BinaryDataStorePrivate;
    TArray<uint8> Payload;
    if (!ExtractPayload(Bytes, EDatKind::WorldChunk, Payload, OutError))
    {
        return false;
    }
    FReader Reader(Payload);
    const uint32 Count = Reader.ReadU32();
    if (!Reader.IsOk() || Count > static_cast<uint32>(MaxChunkObjects))
    {
        OutError = TEXT("World chunk object count is invalid");
        return false;
    }

    TArray<FWorldChunkObject> Parsed;
    Parsed.Reserve(static_cast<int32>(Count));
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        FWorldChunkObject& Object = Parsed.AddDefaulted_GetRef();
        Object.UUID = Reader.ReadGuid();
        Object.Location = Reader.ReadVector();
        Object.Rotation = Reader.ReadQuat();
        Object.Scale = Reader.ReadVector();
        Object.Velocity = Reader.ReadVector();
        Object.AngularVelocity = Reader.ReadVector();
        if (!Reader.IsOk() || !Object.UUID.IsValid() || !IsFiniteVector(Object.Location)
            || !IsFiniteQuat(Object.Rotation) || !Object.Rotation.IsNormalized()
            || !IsFiniteVector(Object.Scale) || !IsFiniteVector(Object.Velocity)
            || !IsFiniteVector(Object.AngularVelocity))
        {
            OutError = FString::Printf(TEXT("Invalid world chunk object at index %u"), Index);
            return false;
        }
    }
    if (!Reader.IsAtEnd())
    {
        OutError = TEXT("World chunk is truncated or has trailing bytes");
        return false;
    }
    OutObjects = MoveTemp(Parsed);
    return true;
}

bool FBinaryDataStore::LoadWorldChunk(const FString& DatPath, TArray<FWorldChunkObject>& OutObjects, FString& OutError)
{
    return BinaryDataStorePrivate::LoadValidatedDat(
        DatPath, MaxWorldChunkDatBytes,
        [&OutObjects](const TArray<uint8>& Bytes, FString& Error)
        {
            return DeserializeWorldChunk(Bytes, OutObjects, Error);
        }, OutError);
}

void FBinaryDataStore::LoadWorldChunkAsync(
    const FString& DatPath,
    TFunction<void(bool, TArray<FWorldChunkObject>, FString)> Callback)
{
    const FString SafePath = FSafeFileIO::NormalizeFilePath(DatPath);
    const bool bQueued = FSafeFileIO::RunTrackedWorker([SafePath, Callback]() mutable
    {
        TArray<FWorldChunkObject> Objects;
        FString Error;
        const bool bMissing = !IFileManager::Get().FileExists(*SafePath);
        const bool bLoaded = bMissing || LoadWorldChunk(SafePath, Objects, Error);
        FSafeFileIO::DispatchTrackedGameThread(
            [Callback = MoveTemp(Callback), bLoaded, Objects = MoveTemp(Objects), Error = MoveTemp(Error)]() mutable
            {
                if (Callback) Callback(bLoaded, MoveTemp(Objects), MoveTemp(Error));
            });
    });
    if (!bQueued && Callback)
    {
        Callback(false, TArray<FWorldChunkObject>(), TEXT("async I/O queue is shutting down"));
    }
}

FSafeFileWriteResult FBinaryDataStore::SaveWorldChunkBlocking(const FString& DatPath, const TArray<FWorldChunkObject>& Objects)
{
    TArray<uint8> Bytes;
    FString Error;
    if (!SerializeWorldChunk(Objects, Bytes, Error))
    {
        return BinaryDataStorePrivate::MakeSerializationFailure(DatPath, Error);
    }
    return FSafeFileIO::SaveBinaryBlocking(Bytes, DatPath, MaxWorldChunkDatBytes);
}

void FBinaryDataStore::SaveWorldChunkAsync(
    const FString& DatPath,
    const TArray<FWorldChunkObject>& Objects,
    FSafeFileIO::FWriteCallback Callback)
{
    TArray<uint8> Bytes;
    FString Error;
    if (!SerializeWorldChunk(Objects, Bytes, Error))
    {
        if (Callback) Callback(BinaryDataStorePrivate::MakeSerializationFailure(DatPath, Error));
        return;
    }
    FSafeFileIO::SaveBinaryAsync(Bytes, DatPath, MaxWorldChunkDatBytes, MoveTemp(Callback));
}

bool FBinaryDataStore::SerializeModelDatabase(
    const TArray<FModelDatabaseEntry>& Entries,
    TArray<uint8>& OutBytes,
    FString& OutError)
{
    using namespace BinaryDataStorePrivate;
    if (Entries.Num() > MaxDatabaseEntries)
    {
        OutError = TEXT("Model database exceeds the row-count safety limit");
        return false;
    }
    TArray<uint8> Payload;
    FWriter Writer(Payload);
    Writer.WriteU32(static_cast<uint32>(Entries.Num()));
    TSet<FGuid> Seen;
    for (const FModelDatabaseEntry& Entry : Entries)
    {
        if (!Entry.UUID.IsValid() || Seen.Contains(Entry.UUID) || Entry.Json.IsEmpty())
        {
            OutError = TEXT("Model database contains a duplicate/invalid UUID or empty JSON path");
            return false;
        }
        Seen.Add(Entry.UUID);
        Writer.WriteGuid(Entry.UUID);
        Writer.WriteString(Entry.Cache, MaxPathBytes);
        Writer.WriteString(Entry.Json, MaxPathBytes);
    }
    return Writer.IsOk() && BuildEnvelope(EDatKind::ModelDatabase, Payload, OutBytes, OutError);
}

bool FBinaryDataStore::DeserializeModelDatabase(
    const TArray<uint8>& Bytes,
    TArray<FModelDatabaseEntry>& OutEntries,
    FString& OutError)
{
    using namespace BinaryDataStorePrivate;
    TArray<uint8> Payload;
    if (!ExtractPayload(Bytes, EDatKind::ModelDatabase, Payload, OutError)) return false;
    FReader Reader(Payload);
    const uint32 Count = Reader.ReadU32();
    if (!Reader.IsOk() || Count > static_cast<uint32>(MaxDatabaseEntries))
    {
        OutError = TEXT("Model database row count is invalid");
        return false;
    }
    TArray<FModelDatabaseEntry> Parsed;
    TSet<FGuid> Seen;
    Parsed.Reserve(static_cast<int32>(Count));
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        FModelDatabaseEntry& Entry = Parsed.AddDefaulted_GetRef();
        Entry.UUID = Reader.ReadGuid();
        Entry.Cache = Reader.ReadString(MaxPathBytes);
        Entry.Json = Reader.ReadString(MaxPathBytes);
        if (!Reader.IsOk() || !Entry.UUID.IsValid() || Seen.Contains(Entry.UUID) || Entry.Json.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Invalid model database row at index %u"), Index);
            return false;
        }
        Seen.Add(Entry.UUID);
    }
    if (!Reader.IsAtEnd())
    {
        OutError = TEXT("Model database is truncated or has trailing bytes");
        return false;
    }
    OutEntries = MoveTemp(Parsed);
    return true;
}

bool FBinaryDataStore::LoadModelDatabase(const FString& DatPath, TArray<FModelDatabaseEntry>& OutEntries, FString& OutError)
{
    return BinaryDataStorePrivate::LoadValidatedDat(
        DatPath, MaxModelDatabaseDatBytes,
        [&OutEntries](const TArray<uint8>& Bytes, FString& Error)
        {
            return DeserializeModelDatabase(Bytes, OutEntries, Error);
        }, OutError);
}

FSafeFileWriteResult FBinaryDataStore::SaveModelDatabaseBlocking(const FString& DatPath, const TArray<FModelDatabaseEntry>& Entries)
{
    TArray<uint8> Bytes;
    FString Error;
    if (!SerializeModelDatabase(Entries, Bytes, Error))
        return BinaryDataStorePrivate::MakeSerializationFailure(DatPath, Error);
    return FSafeFileIO::SaveBinaryBlocking(Bytes, DatPath, MaxModelDatabaseDatBytes);
}

void FBinaryDataStore::SaveModelDatabaseAsync(const FString& DatPath, const TArray<FModelDatabaseEntry>& Entries, FSafeFileIO::FWriteCallback Callback)
{
    TArray<uint8> Bytes;
    FString Error;
    if (!SerializeModelDatabase(Entries, Bytes, Error))
    {
        if (Callback) Callback(BinaryDataStorePrivate::MakeSerializationFailure(DatPath, Error));
        return;
    }
    FSafeFileIO::SaveBinaryAsync(Bytes, DatPath, MaxModelDatabaseDatBytes, MoveTemp(Callback));
}

bool FBinaryDataStore::SerializeSceneDatabase(
    const TArray<FSceneDatabaseEntry>& Entries,
    TArray<uint8>& OutBytes,
    FString& OutError)
{
    using namespace BinaryDataStorePrivate;
    if (Entries.Num() > MaxDatabaseEntries)
    {
        OutError = TEXT("Scene database exceeds the row-count safety limit");
        return false;
    }

    TArray<FSceneDatabaseEntry> Sorted = Entries;
    Sorted.Sort([](const FSceneDatabaseEntry& A, const FSceneDatabaseEntry& B)
    {
        return A.UUID.ToString(EGuidFormats::Digits).Compare(
            B.UUID.ToString(EGuidFormats::Digits), ESearchCase::CaseSensitive) < 0;
    });

    TArray<uint8> Payload;
    FWriter Writer(Payload);
    Writer.WriteU32(static_cast<uint32>(Sorted.Num()));
    TSet<FGuid> Seen;
    for (const FSceneDatabaseEntry& Entry : Sorted)
    {
        if (!Entry.UUID.IsValid() || Seen.Contains(Entry.UUID)
            || !IsFiniteVector(Entry.Location) || !IsFiniteVector(Entry.Size)
            || Entry.Size.X < 0.0 || Entry.Size.Y < 0.0 || Entry.Size.Z < 0.0
            || Entry.Size.IsNearlyZero(0.001))
        {
            OutError = TEXT("Scene database contains a duplicate UUID or invalid bounds");
            return false;
        }
        Seen.Add(Entry.UUID);
        Writer.WriteGuid(Entry.UUID);
        Writer.WriteVector(Entry.Location);
        Writer.WriteVector(Entry.Size);
    }
    return Writer.IsOk() && BuildEnvelope(EDatKind::SceneDatabase, Payload, OutBytes, OutError);
}

bool FBinaryDataStore::DeserializeSceneDatabase(
    const TArray<uint8>& Bytes,
    TArray<FSceneDatabaseEntry>& OutEntries,
    FString& OutError)
{
    using namespace BinaryDataStorePrivate;
    TArray<uint8> Payload;
    if (!ExtractPayload(Bytes, EDatKind::SceneDatabase, Payload, OutError)) return false;

    FReader Reader(Payload);
    const uint32 Count = Reader.ReadU32();
    if (!Reader.IsOk() || Count > static_cast<uint32>(MaxDatabaseEntries))
    {
        OutError = TEXT("Scene database row count is invalid");
        return false;
    }

    TArray<FSceneDatabaseEntry> Parsed;
    TSet<FGuid> Seen;
    Parsed.Reserve(static_cast<int32>(Count));
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        FSceneDatabaseEntry& Entry = Parsed.AddDefaulted_GetRef();
        Entry.UUID = Reader.ReadGuid();
        Entry.Location = Reader.ReadVector();
        Entry.Size = Reader.ReadVector();
        if (!Reader.IsOk() || !Entry.UUID.IsValid() || Seen.Contains(Entry.UUID)
            || !IsFiniteVector(Entry.Location) || !IsFiniteVector(Entry.Size)
            || Entry.Size.X < 0.0 || Entry.Size.Y < 0.0 || Entry.Size.Z < 0.0
            || Entry.Size.IsNearlyZero(0.001))
        {
            OutError = FString::Printf(TEXT("Invalid scene database row at index %u"), Index);
            return false;
        }
        Seen.Add(Entry.UUID);
    }
    if (!Reader.IsAtEnd())
    {
        OutError = TEXT("Scene database is truncated or has trailing bytes");
        return false;
    }
    OutEntries = MoveTemp(Parsed);
    return true;
}

bool FBinaryDataStore::LoadSceneDatabase(
    const FString& DatPath,
    TArray<FSceneDatabaseEntry>& OutEntries,
    FString& OutError)
{
    return BinaryDataStorePrivate::LoadValidatedDat(
        DatPath, MaxSceneDatabaseDatBytes,
        [&OutEntries](const TArray<uint8>& Bytes, FString& Error)
        {
            return DeserializeSceneDatabase(Bytes, OutEntries, Error);
        }, OutError);
}

void FBinaryDataStore::LoadSceneDatabaseAsync(
    const FString& DatPath,
    TFunction<void(bool, bool, TArray<FSceneDatabaseEntry>, FString)> Callback)
{
    const FString SafePath = FSafeFileIO::NormalizeFilePath(DatPath);
    const bool bQueued = FSafeFileIO::RunTrackedWorker([SafePath, Callback]() mutable
    {
        TArray<FSceneDatabaseEntry> Entries;
        FString Error;
        const bool bMissing = !IFileManager::Get().FileExists(*SafePath);
        const bool bLoaded = bMissing || LoadSceneDatabase(SafePath, Entries, Error);
        FSafeFileIO::DispatchTrackedGameThread(
            [Callback = MoveTemp(Callback), bLoaded, bMissing, Entries = MoveTemp(Entries), Error = MoveTemp(Error)]() mutable
            {
                if (Callback) Callback(bLoaded, bMissing, MoveTemp(Entries), MoveTemp(Error));
            });
    });
    if (!bQueued && Callback)
    {
        Callback(false, false, TArray<FSceneDatabaseEntry>(), TEXT("async I/O queue is shutting down"));
    }
}

FSafeFileWriteResult FBinaryDataStore::SaveSceneDatabaseBlocking(
    const FString& DatPath,
    const TArray<FSceneDatabaseEntry>& Entries)
{
    TArray<uint8> Bytes;
    FString Error;
    if (!SerializeSceneDatabase(Entries, Bytes, Error))
        return BinaryDataStorePrivate::MakeSerializationFailure(DatPath, Error);
    return FSafeFileIO::SaveBinaryBlocking(Bytes, DatPath, MaxSceneDatabaseDatBytes);
}

void FBinaryDataStore::SaveSceneDatabaseAsync(
    const FString& DatPath,
    const TArray<FSceneDatabaseEntry>& Entries,
    FSafeFileIO::FWriteCallback Callback)
{
    TArray<uint8> Bytes;
    FString Error;
    if (!SerializeSceneDatabase(Entries, Bytes, Error))
    {
        if (Callback) Callback(BinaryDataStorePrivate::MakeSerializationFailure(DatPath, Error));
        return;
    }
    FSafeFileIO::SaveBinaryAsync(Bytes, DatPath, MaxSceneDatabaseDatBytes, MoveTemp(Callback));
}

bool FBinaryDataStore::SerializeLevel(const FLevelRuntimeData& Data, TArray<uint8>& OutBytes, FString& OutError)
{
    using namespace BinaryDataStorePrivate;
    if (!FMath::IsFinite(Data.WorldTime) || Data.Players.Num() > MaxPlayers)
    {
        OutError = TEXT("Level state contains invalid time or too many players");
        return false;
    }
    TArray<uint8> Payload;
    FWriter Writer(Payload);
    Writer.WriteFloat(Data.WorldTime);
    Writer.WriteString(Data.SelectedPlayer, MaxPathBytes);
    Writer.WriteU32(static_cast<uint32>(Data.Players.Num()));
    TSet<FString> Seen;
    for (const FWorldPlayerRecord& Player : Data.Players)
    {
        const FString Key = Player.PlayerId.ToLower();
        if (Player.PlayerId.IsEmpty() || Seen.Contains(Key) || !IsFiniteVector(Player.Location)
            || !FMath::IsFinite(Player.Rotation.Pitch) || !FMath::IsFinite(Player.Rotation.Yaw)
            || !FMath::IsFinite(Player.Rotation.Roll) || !FMath::IsFinite(Player.Health)
            || Player.Items.Num() > MaxItemsPerPlayer)
        {
            OutError = TEXT("Level state contains an invalid player record");
            return false;
        }
        Seen.Add(Key);
        Writer.WriteString(Player.PlayerId, MaxNameBytes);
        Writer.WriteString(Player.DisplayName, MaxNameBytes);
        Writer.WriteVector(Player.Location);
        Writer.WriteDouble(Player.Rotation.Pitch);
        Writer.WriteDouble(Player.Rotation.Yaw);
        Writer.WriteDouble(Player.Rotation.Roll);
        Writer.WriteFloat(Player.Health);
        Writer.WriteI32(FMath::Max(1, Player.Level));
        Writer.WriteString(Player.PlayerGameMode, MaxNameBytes);
        Writer.WriteU32(static_cast<uint32>(Player.Items.Num()));
        for (const FString& Item : Player.Items) Writer.WriteString(Item, MaxPathBytes);
        FString Custom;
        if (!JsonObjectToCompactString(Player.CustomJson, Custom))
        {
            OutError = TEXT("Player custom JSON cannot be serialized");
            return false;
        }
        Writer.WriteString(Custom, MaxStringBytes);
    }
    return Writer.IsOk() && BuildEnvelope(EDatKind::Level, Payload, OutBytes, OutError);
}

bool FBinaryDataStore::DeserializeLevel(const TArray<uint8>& Bytes, FLevelRuntimeData& OutData, FString& OutError)
{
    using namespace BinaryDataStorePrivate;
    TArray<uint8> Payload;
    if (!ExtractPayload(Bytes, EDatKind::Level, Payload, OutError)) return false;
    FReader Reader(Payload);
    FLevelRuntimeData Parsed;
    Parsed.WorldTime = Reader.ReadFloat();
    Parsed.SelectedPlayer = Reader.ReadString(MaxPathBytes);
    const uint32 Count = Reader.ReadU32();
    if (!Reader.IsOk() || !FMath::IsFinite(Parsed.WorldTime) || Count > static_cast<uint32>(MaxPlayers))
    {
        OutError = TEXT("Level header is invalid");
        return false;
    }
    TSet<FString> Seen;
    Parsed.Players.Reserve(static_cast<int32>(Count));
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        FWorldPlayerRecord& Player = Parsed.Players.AddDefaulted_GetRef();
        Player.PlayerId = Reader.ReadString(MaxNameBytes);
        Player.DisplayName = Reader.ReadString(MaxNameBytes);
        Player.Location = Reader.ReadVector();
        Player.Rotation = FRotator(Reader.ReadDouble(), Reader.ReadDouble(), Reader.ReadDouble()).GetNormalized();
        Player.Health = Reader.ReadFloat();
        Player.Level = Reader.ReadI32();
        Player.PlayerGameMode = Reader.ReadString(MaxNameBytes);
        const uint32 ItemCount = Reader.ReadU32();
        if (!Reader.IsOk() || Player.PlayerId.IsEmpty() || Seen.Contains(Player.PlayerId.ToLower())
            || ItemCount > static_cast<uint32>(MaxItemsPerPlayer) || !IsFiniteVector(Player.Location)
            || !FMath::IsFinite(Player.Health) || Player.Level < 1)
        {
            OutError = FString::Printf(TEXT("Invalid level player at index %u"), Index);
            return false;
        }
        Seen.Add(Player.PlayerId.ToLower());
        for (uint32 ItemIndex = 0; ItemIndex < ItemCount; ++ItemIndex)
            Player.Items.Add(Reader.ReadString(MaxPathBytes));
        const FString Custom = Reader.ReadString(MaxStringBytes);
        FSafeJsonLimits Limits;
        Limits.MaxFileBytes = MaxStringBytes;
        const FSafeJsonLoadResult Json = FSafeFileIO::ParseJsonText(Custom.IsEmpty() ? TEXT("{}") : Custom,
            FString::Printf(TEXT("level.dat Player[%u]"), Index), Limits);
        if (!Reader.IsOk() || !Json.IsSuccess())
        {
            OutError = FString::Printf(TEXT("Invalid custom data for player %u"), Index);
            return false;
        }
        Player.CustomJson = Json.JsonObject;
    }
    if (!Reader.IsAtEnd())
    {
        OutError = TEXT("Level state is truncated or has trailing bytes");
        return false;
    }
    OutData = MoveTemp(Parsed);
    return true;
}

bool FBinaryDataStore::LoadLevel(const FString& DatPath, FLevelRuntimeData& OutData, FString& OutError)
{
    return BinaryDataStorePrivate::LoadValidatedDat(
        DatPath, MaxLevelDatBytes,
        [&OutData](const TArray<uint8>& Bytes, FString& Error)
        {
            return DeserializeLevel(Bytes, OutData, Error);
        }, OutError);
}

FSafeFileWriteResult FBinaryDataStore::SaveLevelBlocking(const FString& DatPath, const FLevelRuntimeData& Data)
{
    TArray<uint8> Bytes;
    FString Error;
    if (!SerializeLevel(Data, Bytes, Error)) return BinaryDataStorePrivate::MakeSerializationFailure(DatPath, Error);
    return FSafeFileIO::SaveBinaryBlocking(Bytes, DatPath, MaxLevelDatBytes);
}

void FBinaryDataStore::SaveLevelAsync(const FString& DatPath, const FLevelRuntimeData& Data, FSafeFileIO::FWriteCallback Callback)
{
    TArray<uint8> Bytes;
    FString Error;
    if (!SerializeLevel(Data, Bytes, Error))
    {
        if (Callback) Callback(BinaryDataStorePrivate::MakeSerializationFailure(DatPath, Error));
        return;
    }
    FSafeFileIO::SaveBinaryAsync(Bytes, DatPath, MaxLevelDatBytes, MoveTemp(Callback));
}
