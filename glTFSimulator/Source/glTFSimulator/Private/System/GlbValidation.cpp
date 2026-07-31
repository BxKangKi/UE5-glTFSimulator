// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/GlbValidation.h"
#include "System/SafeFileIO.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/Archive.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    // GLB 2.0 constants are stored little-endian in the file.
    constexpr uint32 GLB_MAGIC = 0x46546C67;
    constexpr uint32 GLB_VERSION_2 = 2;
    constexpr uint32 GLB_JSON_CHUNK = 0x4E4F534A;
    constexpr uint32 GLB_BIN_CHUNK = 0x004E4942;
    constexpr int64 GLB_HEADER_AND_CHUNK_HEADER_SIZE = 20;

    // These limits are intentionally conservative. A rejected external file is preferable to a
    // packaged build terminating inside TArray, the renderer, Chaos, or a third-party decoder.
    constexpr uint32 MAX_RUNTIME_JSON_CHUNK_BYTES = 64u * 1024u * 1024u;
    constexpr int32 MAX_RUNTIME_BUFFER_COUNT = 1;
    constexpr int32 MAX_RUNTIME_BUFFER_VIEW_COUNT = 500000;
    constexpr int32 MAX_RUNTIME_ACCESSOR_COUNT = 500000;
    constexpr int32 MAX_RUNTIME_MESH_COUNT = 65536;
    constexpr int32 MAX_RUNTIME_PRIMITIVES_PER_MESH = 4096;
    constexpr int32 MAX_RUNTIME_MORPH_TARGETS_PER_PRIMITIVE = 256;
    constexpr int32 MAX_RUNTIME_UV_CHANNELS = 8;
    constexpr int64 MAX_RUNTIME_ACCESSOR_ELEMENTS = 12000000;
    constexpr int64 MAX_RUNTIME_ACCESSOR_BYTES = 512ll * 1024ll * 1024ll;
    constexpr int64 MAX_RUNTIME_MESH_VERTICES = 4000000;
    constexpr int64 MAX_RUNTIME_MESH_INDICES = 12000000;
    constexpr int32 MAX_RUNTIME_VALIDATION_CACHE_ENTRIES = 2048;
    constexpr int32 RUNTIME_VALIDATION_SCHEMA_VERSION = 2;

    /** Cached preflight results avoid reopening and reparsing an unchanged GLB during size-scan and streaming passes. */
    struct FRuntimeMeshValidationCacheEntry
    {
        int64 FileSize = INDEX_NONE;
        FDateTime Timestamp;
        bool bValid = false;
        FString Reason;
    };

    FCriticalSection GRuntimeMeshValidationCacheMutex;
    TMap<FString, FRuntimeMeshValidationCacheEntry> GRuntimeMeshValidationCache;

    FString MakeRuntimeValidationCacheKey(const FString& NormalizedPath)
    {
        // Including a schema revision prevents stale pass/fail entries from surviving Live Coding
        // when validation rules are corrected without changing the external file.
        return FString::Printf(
            TEXT("%s|validation-v%d"),
            *NormalizedPath,
            RUNTIME_VALIDATION_SCHEMA_VERSION);
    }

    constexpr uint32 MAX_CHARACTER_JSON_CHUNK_BYTES = 64u * 1024u * 1024u;
    constexpr int64 MAX_CHARACTER_GLB_BYTES = 512ll * 1024ll * 1024ll;
    constexpr int32 MAX_CHARACTER_NODE_COUNT = 65536;
    constexpr int32 MAX_CHARACTER_MESH_COUNT = 16384;
    constexpr int32 MAX_CHARACTER_SKIN_COUNT = 128;
    constexpr int32 MAX_CHARACTER_JOINT_COUNT = 4096;

    struct FGlbHeader
    {
        uint32 Magic = 0;
        uint32 Version = 0;
        uint32 DeclaredLength = 0;
        uint32 JsonChunkLength = 0;
        uint32 JsonChunkType = 0;
    };

    struct FBufferInfo
    {
        int64 ByteLength = 0;
    };

    struct FBufferViewInfo
    {
        int32 BufferIndex = INDEX_NONE;
        int64 ByteOffset = 0;
        int64 ByteLength = 0;
        int32 ByteStride = 0;
    };

    struct FAccessorInfo
    {
        int64 Count = 0;
        int32 ComponentType = 0;
        int32 ElementByteSize = 0;
        FString Type;
    };

    struct FParsedGlbJson
    {
        TSharedPtr<FJsonObject> Root;
        int64 FileSize = 0;
        int64 BinaryChunkSize = INDEX_NONE;
        uint32 JsonChunkLength = 0;
    };

    bool TryGetCachedRuntimeValidation(
        const FString& NormalizedPath,
        const int64 FileSize,
        const FDateTime& Timestamp,
        bool& bOutValid,
        FString& OutReason)
    {
        FScopeLock Lock(&GRuntimeMeshValidationCacheMutex);
        const FRuntimeMeshValidationCacheEntry* Entry = GRuntimeMeshValidationCache.Find(NormalizedPath);
        if (!Entry || Entry->FileSize != FileSize || Entry->Timestamp != Timestamp)
        {
            return false;
        }

        bOutValid = Entry->bValid;
        OutReason = Entry->Reason;
        return true;
    }

    void CacheRuntimeValidation(
        const FString& NormalizedPath,
        const int64 FileSize,
        const FDateTime& Timestamp,
        const bool bValid,
        const FString& Reason)
    {
        FScopeLock Lock(&GRuntimeMeshValidationCacheMutex);
        if (GRuntimeMeshValidationCache.Num() >= MAX_RUNTIME_VALIDATION_CACHE_ENTRIES &&
            !GRuntimeMeshValidationCache.Contains(NormalizedPath))
        {
            // Validation is cheap compared with unbounded cache growth across user-selected worlds.
            GRuntimeMeshValidationCache.Reset();
        }

        FRuntimeMeshValidationCacheEntry& Entry = GRuntimeMeshValidationCache.FindOrAdd(NormalizedPath);
        Entry.FileSize = FileSize;
        Entry.Timestamp = Timestamp;
        Entry.bValid = bValid;
        Entry.Reason = Reason;
    }

    bool ReadHeader(FArchive& Reader, FGlbHeader& OutHeader)
    {
        Reader.Serialize(&OutHeader.Magic, sizeof(OutHeader.Magic));
        Reader.Serialize(&OutHeader.Version, sizeof(OutHeader.Version));
        Reader.Serialize(&OutHeader.DeclaredLength, sizeof(OutHeader.DeclaredLength));
        Reader.Serialize(&OutHeader.JsonChunkLength, sizeof(OutHeader.JsonChunkLength));
        Reader.Serialize(&OutHeader.JsonChunkType, sizeof(OutHeader.JsonChunkType));
        return !Reader.IsError();
    }

    bool CheckedAddNonNegative(const int64 A, const int64 B, int64& OutValue)
    {
        if (A < 0 || B < 0 || A > TNumericLimits<int64>::Max() - B)
        {
            return false;
        }
        OutValue = A + B;
        return true;
    }

    bool CheckedMultiplyNonNegative(const int64 A, const int64 B, int64& OutValue)
    {
        if (A < 0 || B < 0 || (A > 0 && B > TNumericLimits<int64>::Max() / A))
        {
            return false;
        }
        OutValue = A * B;
        return true;
    }

    bool TryGetIntegerField(
        const TSharedPtr<FJsonObject>& Object,
        const TCHAR* FieldName,
        const bool bRequired,
        const int64 DefaultValue,
        const int64 Minimum,
        const int64 Maximum,
        int64& OutValue)
    {
        OutValue = DefaultValue;
        if (!Object.IsValid())
        {
            return false;
        }

        const TSharedPtr<FJsonValue>* ValuePtr = Object->Values.Find(FieldName);
        if (!ValuePtr)
        {
            return !bRequired;
        }

        double NumberValue = 0.0;
        if (!ValuePtr->IsValid() || !(*ValuePtr)->TryGetNumber(NumberValue) || !FMath::IsFinite(NumberValue) ||
            NumberValue < static_cast<double>(Minimum) || NumberValue > static_cast<double>(Maximum))
        {
            return false;
        }

        const int64 IntegerValue = static_cast<int64>(NumberValue);
        if (static_cast<double>(IntegerValue) != NumberValue)
        {
            return false;
        }

        OutValue = IntegerValue;
        return true;
    }

    bool TryGetOptionalIndex(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, bool& bOutHasValue, int32& OutIndex)
    {
        bOutHasValue = false;
        OutIndex = INDEX_NONE;
        if (!Object.IsValid() || !Object->HasField(FieldName))
        {
            return true;
        }

        int64 IntegerValue = 0;
        if (!TryGetIntegerField(Object, FieldName, true, 0, 0, MAX_int32, IntegerValue))
        {
            return false;
        }

        bOutHasValue = true;
        OutIndex = static_cast<int32>(IntegerValue);
        return true;
    }

    bool TryGetObjectFieldSafe(
        const TSharedPtr<FJsonObject>& Object,
        const TCHAR* FieldName,
        TSharedPtr<FJsonObject>& OutObject,
        const bool bRequired)
    {
        OutObject.Reset();
        if (!Object.IsValid())
        {
            return false;
        }

        const TSharedPtr<FJsonValue>* ValuePtr = Object->Values.Find(FieldName);
        if (!ValuePtr)
        {
            return !bRequired;
        }
        if (!ValuePtr->IsValid() || (*ValuePtr)->Type != EJson::Object)
        {
            return false;
        }

        OutObject = (*ValuePtr)->AsObject();
        return OutObject.IsValid();
    }

    bool TryGetArrayFieldSafe(
        const TSharedPtr<FJsonObject>& Object,
        const TCHAR* FieldName,
        const TArray<TSharedPtr<FJsonValue>>*& OutArray,
        const bool bRequired,
        const bool bRequireNonEmpty)
    {
        OutArray = nullptr;
        if (!Object.IsValid())
        {
            return false;
        }

        const TSharedPtr<FJsonValue>* ValuePtr = Object->Values.Find(FieldName);
        if (!ValuePtr)
        {
            return !bRequired;
        }
        if (!ValuePtr->IsValid() || (*ValuePtr)->Type != EJson::Array)
        {
            return false;
        }

        OutArray = &(*ValuePtr)->AsArray();
        return OutArray && (!bRequireNonEmpty || OutArray->Num() > 0);
    }

    bool TryGetRequiredArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, const TArray<TSharedPtr<FJsonValue>>*& OutArray)
    {
        return TryGetArrayFieldSafe(Object, FieldName, OutArray, true, true);
    }

    bool TryGetJsonObjectValue(const TSharedPtr<FJsonValue>& Value, TSharedPtr<FJsonObject>& OutObject)
    {
        OutObject.Reset();
        if (!Value.IsValid() || Value->Type != EJson::Object)
        {
            return false;
        }
        OutObject = Value->AsObject();
        return OutObject.IsValid();
    }

    bool TryGetArrayIndex(const TSharedPtr<FJsonValue>& Value, int32& OutIndex)
    {
        OutIndex = INDEX_NONE;
        double NumberValue = 0.0;
        if (!Value.IsValid() || !Value->TryGetNumber(NumberValue) || !FMath::IsFinite(NumberValue) ||
            NumberValue < 0.0 || NumberValue > static_cast<double>(MAX_int32))
        {
            return false;
        }

        const int64 IntegerValue = static_cast<int64>(NumberValue);
        if (static_cast<double>(IntegerValue) != NumberValue)
        {
            return false;
        }

        OutIndex = static_cast<int32>(IntegerValue);
        return true;
    }

    bool ValidateOptionalFiniteArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, const int32 ExpectedCount)
    {
        if (!Object.IsValid() || !Object->HasField(FieldName))
        {
            return true;
        }

        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!TryGetArrayFieldSafe(Object, FieldName, Values, true, false) || !Values || Values->Num() != ExpectedCount)
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            double NumberValue = 0.0;
            if (!Value.IsValid() || !Value->TryGetNumber(NumberValue) || !FMath::IsFinite(NumberValue))
            {
                return false;
            }
        }
        return true;
    }

    int32 GetComponentByteSize(const int32 ComponentType)
    {
        switch (ComponentType)
        {
            case 5120: // BYTE
            case 5121: // UNSIGNED_BYTE
                return 1;
            case 5122: // SHORT
            case 5123: // UNSIGNED_SHORT
                return 2;
            case 5125: // UNSIGNED_INT
            case 5126: // FLOAT
                return 4;
            default:
                return 0;
        }
    }

    bool GetAccessorShape(const FString& Type, int32& OutRows, int32& OutColumns)
    {
        OutRows = 0;
        OutColumns = 0;
        if (Type == TEXT("SCALAR"))
        {
            OutRows = 1;
            OutColumns = 1;
        }
        else if (Type == TEXT("VEC2"))
        {
            OutRows = 2;
            OutColumns = 1;
        }
        else if (Type == TEXT("VEC3"))
        {
            OutRows = 3;
            OutColumns = 1;
        }
        else if (Type == TEXT("VEC4"))
        {
            OutRows = 4;
            OutColumns = 1;
        }
        else if (Type == TEXT("MAT2"))
        {
            OutRows = 2;
            OutColumns = 2;
        }
        else if (Type == TEXT("MAT3"))
        {
            OutRows = 3;
            OutColumns = 3;
        }
        else if (Type == TEXT("MAT4"))
        {
            OutRows = 4;
            OutColumns = 4;
        }
        return OutRows > 0 && OutColumns > 0;
    }

    bool CalculateAccessorElementByteSize(const int32 ComponentType, const FString& Type, int32& OutElementByteSize)
    {
        OutElementByteSize = 0;
        const int32 ComponentSize = GetComponentByteSize(ComponentType);
        int32 Rows = 0;
        int32 Columns = 0;
        if (ComponentSize <= 0 || !GetAccessorShape(Type, Rows, Columns))
        {
            return false;
        }

        const int32 ColumnByteSize = Rows * ComponentSize;
        const int32 AlignedColumnByteSize = Columns > 1 ? Align(ColumnByteSize, 4) : ColumnByteSize;
        const int64 ElementByteSize64 = static_cast<int64>(AlignedColumnByteSize) * Columns;
        if (ElementByteSize64 <= 0 || ElementByteSize64 > MAX_int32)
        {
            return false;
        }

        OutElementByteSize = static_cast<int32>(ElementByteSize64);
        return true;
    }

    bool ValidateViewRange(
        const FBufferViewInfo& View,
        const int64 LocalOffset,
        const int64 Count,
        const int64 ElementByteSize,
        const int64 ExplicitStride)
    {
        if (LocalOffset < 0 || Count < 0 || ElementByteSize <= 0 || LocalOffset > View.ByteLength)
        {
            return false;
        }
        if (Count == 0)
        {
            return true;
        }

        const int64 Stride = ExplicitStride > 0 ? ExplicitStride : ElementByteSize;
        if (Stride < ElementByteSize)
        {
            return false;
        }

        int64 LastElementOffset = 0;
        int64 RequiredBytes = 0;
        int64 EndOffset = 0;
        if (!CheckedMultiplyNonNegative(Count - 1, Stride, LastElementOffset) ||
            !CheckedAddNonNegative(LastElementOffset, ElementByteSize, RequiredBytes) ||
            !CheckedAddNonNegative(LocalOffset, RequiredBytes, EndOffset))
        {
            return false;
        }
        return EndOffset <= View.ByteLength;
    }

    bool LoadRootJsonForValidation(const FString& NormalizedPath, FParsedGlbJson& OutParsed, FString& OutReason)
    {
        OutParsed = FParsedGlbJson();
        OutParsed.FileSize = IFileManager::Get().FileSize(*NormalizedPath);
        if (OutParsed.FileSize < GLB_HEADER_AND_CHUNK_HEADER_SIZE)
        {
            OutReason = TEXT("GLB is too small to contain a JSON chunk");
            return false;
        }

        TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*NormalizedPath));
        if (!Reader.IsValid())
        {
            OutReason = TEXT("failed to open GLB for JSON validation");
            return false;
        }

        FGlbHeader Header;
        if (!ReadHeader(*Reader, Header) || Header.JsonChunkLength == 0 ||
            Header.JsonChunkLength > MAX_RUNTIME_JSON_CHUNK_BYTES)
        {
            OutReason = FString::Printf(TEXT("runtime JSON chunk is missing or exceeds %u bytes"), MAX_RUNTIME_JSON_CHUNK_BYTES);
            return false;
        }

        OutParsed.JsonChunkLength = Header.JsonChunkLength;
        TArray<uint8> JsonBytes;
        JsonBytes.SetNumUninitialized(static_cast<int32>(Header.JsonChunkLength));
        Reader->Serialize(JsonBytes.GetData(), JsonBytes.Num());
        if (Reader->IsError())
        {
            OutReason = TEXT("failed while reading the GLB JSON chunk");
            return false;
        }

        // Record the embedded BIN chunk length so buffer and buffer-view ranges can be checked
        // before glTFRuntime allocates arrays from untrusted accessor metadata.
        int64 NextChunkOffset = GLB_HEADER_AND_CHUNK_HEADER_SIZE + static_cast<int64>(Header.JsonChunkLength);
        while (NextChunkOffset < OutParsed.FileSize)
        {
            Reader->Seek(NextChunkOffset);
            uint32 ChunkLength = 0;
            uint32 ChunkType = 0;
            Reader->Serialize(&ChunkLength, sizeof(ChunkLength));
            Reader->Serialize(&ChunkType, sizeof(ChunkType));
            if (Reader->IsError())
            {
                OutReason = TEXT("failed while scanning GLB chunk headers");
                return false;
            }
            if (ChunkType == GLB_BIN_CHUNK && OutParsed.BinaryChunkSize == INDEX_NONE)
            {
                OutParsed.BinaryChunkSize = static_cast<int64>(ChunkLength);
            }
            NextChunkOffset += 8 + static_cast<int64>(ChunkLength);
        }

        // GLB JSON is UTF-8. Use the shared bounded parser so malformed encoding, excessive
        // nesting, oversized strings, NaN/Infinity, and trailing garbage are rejected consistently.
        FSafeJsonLimits JsonLimits;
        JsonLimits.MaxFileBytes = MAX_RUNTIME_JSON_CHUNK_BYTES;
        JsonLimits.MaxDepth = 96;
        JsonLimits.MaxValues = 4000000;
        JsonLimits.MaxContainerEntries = 1000000;
        JsonLimits.MaxStringCharacters = 8 * 1024 * 1024;
        JsonLimits.bAllowBackupRecovery = false;

        FSafeJsonLoadResult JsonResult = FSafeFileIO::ParseJsonUtf8Bytes(
            JsonBytes,
            NormalizedPath + TEXT("#GLB_JSON"),
            JsonLimits);
        if (!JsonResult.IsSuccess() || !JsonResult.JsonObject.IsValid())
        {
            OutReason = JsonResult.Error.IsEmpty()
                ? TEXT("GLB JSON chunk could not be parsed safely")
                : FString::Printf(TEXT("GLB JSON validation failed: %s"), *JsonResult.Error);
            return false;
        }

        OutParsed.Root = MoveTemp(JsonResult.JsonObject);
        return true;
    }

    bool ParseAndValidateBuffers(
        const TSharedPtr<FJsonObject>& Root,
        const int64 BinaryChunkSize,
        TArray<FBufferInfo>& OutBuffers,
        FString& OutReason)
    {
        const TArray<TSharedPtr<FJsonValue>>* Buffers = nullptr;
        if (!TryGetRequiredArray(Root, TEXT("buffers"), Buffers) || !Buffers ||
            Buffers->Num() > MAX_RUNTIME_BUFFER_COUNT)
        {
            OutReason = TEXT("runtime GLB must contain exactly one embedded buffer");
            return false;
        }
        if (BinaryChunkSize < 0)
        {
            OutReason = TEXT("runtime GLB has no embedded BIN chunk");
            return false;
        }

        OutBuffers.Reset(Buffers->Num());
        for (int32 BufferIndex = 0; BufferIndex < Buffers->Num(); ++BufferIndex)
        {
            TSharedPtr<FJsonObject> BufferObject;
            if (!TryGetJsonObjectValue((*Buffers)[BufferIndex], BufferObject))
            {
                OutReason = FString::Printf(TEXT("buffer %d is not a JSON object"), BufferIndex);
                return false;
            }
            if (BufferObject->HasField(TEXT("uri")))
            {
                OutReason = FString::Printf(TEXT("buffer %d uses an external URI; runtime mesh files must be self-contained GLB files"), BufferIndex);
                return false;
            }

            int64 ByteLength = 0;
            if (!TryGetIntegerField(BufferObject, TEXT("byteLength"), true, 0, 1, MAX_int32, ByteLength))
            {
                OutReason = FString::Printf(TEXT("buffer %d has an invalid byteLength"), BufferIndex);
                return false;
            }
            if (ByteLength > BinaryChunkSize || BinaryChunkSize - ByteLength > 3)
            {
                OutReason = FString::Printf(TEXT("buffer %d length %lld does not match BIN chunk length %lld"),
                    BufferIndex, ByteLength, BinaryChunkSize);
                return false;
            }

            FBufferInfo Info;
            Info.ByteLength = ByteLength;
            OutBuffers.Add(Info);
        }
        return true;
    }

    bool ParseAndValidateBufferViews(
        const TSharedPtr<FJsonObject>& Root,
        const TArray<FBufferInfo>& Buffers,
        TArray<FBufferViewInfo>& OutViews,
        FString& OutReason)
    {
        const TArray<TSharedPtr<FJsonValue>>* BufferViews = nullptr;
        if (!TryGetArrayFieldSafe(Root, TEXT("bufferViews"), BufferViews, true, false) || !BufferViews ||
            BufferViews->Num() > MAX_RUNTIME_BUFFER_VIEW_COUNT)
        {
            OutReason = TEXT("bufferViews is missing, malformed, or exceeds the safety limit");
            return false;
        }

        OutViews.Reset(BufferViews->Num());
        for (int32 ViewIndex = 0; ViewIndex < BufferViews->Num(); ++ViewIndex)
        {
            TSharedPtr<FJsonObject> ViewObject;
            if (!TryGetJsonObjectValue((*BufferViews)[ViewIndex], ViewObject))
            {
                OutReason = FString::Printf(TEXT("bufferView %d is not a JSON object"), ViewIndex);
                return false;
            }

            // EXT_meshopt_compression replaces the allocation-driving buffer, offset, length,
            // stride, and count fields. Validate the extension object itself instead of comparing
            // accessor decoded sizes against the much smaller compressed byteLength.
            TSharedPtr<FJsonObject> ExtensionsObject;
            TSharedPtr<FJsonObject> MeshOptObject;
            if (!TryGetObjectFieldSafe(ViewObject, TEXT("extensions"), ExtensionsObject, false))
            {
                OutReason = FString::Printf(TEXT("bufferView %d has a malformed extensions object"), ViewIndex);
                return false;
            }
            if (ExtensionsObject.IsValid() &&
                !TryGetObjectFieldSafe(ExtensionsObject, TEXT("EXT_meshopt_compression"), MeshOptObject, false))
            {
                OutReason = FString::Printf(TEXT("bufferView %d has a malformed EXT_meshopt_compression object"), ViewIndex);
                return false;
            }

            const bool bMeshOptCompressed = MeshOptObject.IsValid();
            const TSharedPtr<FJsonObject>& SourceObject = bMeshOptCompressed ? MeshOptObject : ViewObject;

            int64 BufferIndex64 = 0;
            int64 SourceByteOffset = 0;
            int64 SourceByteLength = 0;
            int64 ByteStride = 0;
            if (!TryGetIntegerField(SourceObject, TEXT("buffer"), true, 0, 0, MAX_int32, BufferIndex64) ||
                !Buffers.IsValidIndex(static_cast<int32>(BufferIndex64)) ||
                !TryGetIntegerField(SourceObject, TEXT("byteOffset"), false, 0, 0, MAX_int32, SourceByteOffset) ||
                !TryGetIntegerField(SourceObject, TEXT("byteLength"), true, 0, 1, MAX_int32, SourceByteLength) ||
                !TryGetIntegerField(
                    SourceObject,
                    TEXT("byteStride"),
                    bMeshOptCompressed,
                    0,
                    bMeshOptCompressed ? 1 : 0,
                    bMeshOptCompressed ? 256 : 252,
                    ByteStride))
            {
                OutReason = FString::Printf(TEXT("bufferView %d contains invalid allocation or range fields"), ViewIndex);
                return false;
            }

            if (!bMeshOptCompressed && ByteStride != 0 && (ByteStride < 4 || (ByteStride % 4) != 0))
            {
                OutReason = FString::Printf(TEXT("bufferView %d has invalid byteStride %lld"), ViewIndex, ByteStride);
                return false;
            }

            int64 SourceEndOffset = 0;
            if (!CheckedAddNonNegative(SourceByteOffset, SourceByteLength, SourceEndOffset) ||
                SourceEndOffset > Buffers[static_cast<int32>(BufferIndex64)].ByteLength)
            {
                OutReason = FString::Printf(TEXT("bufferView %d exceeds its source buffer bounds"), ViewIndex);
                return false;
            }

            int64 EffectiveByteLength = SourceByteLength;
            if (bMeshOptCompressed)
            {
                int64 ElementCount = 0;
                if (!TryGetIntegerField(
                        MeshOptObject,
                        TEXT("count"),
                        true,
                        0,
                        1,
                        MAX_RUNTIME_ACCESSOR_ELEMENTS,
                        ElementCount) ||
                    !CheckedMultiplyNonNegative(ElementCount, ByteStride, EffectiveByteLength) ||
                    EffectiveByteLength <= 0 || EffectiveByteLength > MAX_RUNTIME_ACCESSOR_BYTES)
                {
                    OutReason = FString::Printf(
                        TEXT("bufferView %d requests an unsafe EXT_meshopt_compression decoded size"),
                        ViewIndex);
                    return false;
                }

                FString Mode;
                FString Filter = TEXT("NONE");
                if (!MeshOptObject->TryGetStringField(TEXT("mode"), Mode) ||
                    (MeshOptObject->HasField(TEXT("filter")) &&
                     !MeshOptObject->TryGetStringField(TEXT("filter"), Filter)))
                {
                    OutReason = FString::Printf(TEXT("bufferView %d has invalid meshopt mode/filter fields"), ViewIndex);
                    return false;
                }

                const bool bAttributesMode = Mode == TEXT("ATTRIBUTES");
                const bool bTrianglesMode = Mode == TEXT("TRIANGLES");
                const bool bIndicesMode = Mode == TEXT("INDICES");
                if (!bAttributesMode && !bTrianglesMode && !bIndicesMode)
                {
                    OutReason = FString::Printf(TEXT("bufferView %d uses unsupported meshopt mode '%s'"), ViewIndex, *Mode);
                    return false;
                }
                if ((bTrianglesMode || bIndicesMode) && ByteStride != 2 && ByteStride != 4)
                {
                    OutReason = FString::Printf(
                        TEXT("bufferView %d uses meshopt index mode with unsupported stride %lld"),
                        ViewIndex,
                        ByteStride);
                    return false;
                }
                if (bTrianglesMode && (ElementCount % 3) != 0)
                {
                    OutReason = FString::Printf(TEXT("bufferView %d has a non-triangular meshopt element count"), ViewIndex);
                    return false;
                }

                if (Filter != TEXT("NONE") && Filter != TEXT("OCTAHEDRAL") &&
                    Filter != TEXT("QUATERNION") && Filter != TEXT("EXPONENTIAL"))
                {
                    OutReason = FString::Printf(TEXT("bufferView %d uses unsupported meshopt filter '%s'"), ViewIndex, *Filter);
                    return false;
                }
            }

            FBufferViewInfo Info;
            Info.BufferIndex = static_cast<int32>(BufferIndex64);
            Info.ByteOffset = 0; // Accessor byteOffset is relative to the decoded view.
            Info.ByteLength = EffectiveByteLength;
            Info.ByteStride = static_cast<int32>(ByteStride);
            OutViews.Add(Info);
        }
        return true;
    }

    bool ValidateSparseAccessor(
        const TSharedPtr<FJsonObject>& AccessorObject,
        const int64 AccessorCount,
        const int32 ElementByteSize,
        const TArray<FBufferViewInfo>& Views,
        FString& OutReason,
        const int32 AccessorIndex)
    {
        TSharedPtr<FJsonObject> SparseObject;
        if (!TryGetObjectFieldSafe(AccessorObject, TEXT("sparse"), SparseObject, false))
        {
            OutReason = FString::Printf(TEXT("accessor %d has an invalid sparse object"), AccessorIndex);
            return false;
        }
        if (!SparseObject.IsValid())
        {
            return true;
        }

        int64 SparseCount = 0;
        if (!TryGetIntegerField(SparseObject, TEXT("count"), true, 0, 1, AccessorCount, SparseCount))
        {
            OutReason = FString::Printf(TEXT("accessor %d has an invalid sparse count"), AccessorIndex);
            return false;
        }

        TSharedPtr<FJsonObject> IndicesObject;
        TSharedPtr<FJsonObject> ValuesObject;
        if (!TryGetObjectFieldSafe(SparseObject, TEXT("indices"), IndicesObject, true) ||
            !TryGetObjectFieldSafe(SparseObject, TEXT("values"), ValuesObject, true))
        {
            OutReason = FString::Printf(TEXT("accessor %d has malformed sparse indices or values"), AccessorIndex);
            return false;
        }

        int64 IndicesView = 0;
        int64 IndicesOffset = 0;
        int64 IndicesComponentType = 0;
        if (!TryGetIntegerField(IndicesObject, TEXT("bufferView"), true, 0, 0, MAX_int32, IndicesView) ||
            !Views.IsValidIndex(static_cast<int32>(IndicesView)) ||
            !TryGetIntegerField(IndicesObject, TEXT("byteOffset"), false, 0, 0, MAX_int32, IndicesOffset) ||
            !TryGetIntegerField(IndicesObject, TEXT("componentType"), true, 0, 0, MAX_int32, IndicesComponentType) ||
            (IndicesComponentType != 5121 && IndicesComponentType != 5123 && IndicesComponentType != 5125))
        {
            OutReason = FString::Printf(TEXT("accessor %d has invalid sparse index metadata"), AccessorIndex);
            return false;
        }
        const int32 SparseIndexByteSize = GetComponentByteSize(static_cast<int32>(IndicesComponentType));
        if (!ValidateViewRange(Views[static_cast<int32>(IndicesView)], IndicesOffset, SparseCount, SparseIndexByteSize, 0))
        {
            OutReason = FString::Printf(TEXT("accessor %d sparse indices exceed their bufferView"), AccessorIndex);
            return false;
        }

        int64 ValuesView = 0;
        int64 ValuesOffset = 0;
        if (!TryGetIntegerField(ValuesObject, TEXT("bufferView"), true, 0, 0, MAX_int32, ValuesView) ||
            !Views.IsValidIndex(static_cast<int32>(ValuesView)) ||
            !TryGetIntegerField(ValuesObject, TEXT("byteOffset"), false, 0, 0, MAX_int32, ValuesOffset) ||
            !ValidateViewRange(Views[static_cast<int32>(ValuesView)], ValuesOffset, SparseCount, ElementByteSize, 0))
        {
            OutReason = FString::Printf(TEXT("accessor %d sparse values exceed their bufferView"), AccessorIndex);
            return false;
        }
        return true;
    }

    bool ParseAndValidateAccessors(
        const TSharedPtr<FJsonObject>& Root,
        const TArray<FBufferViewInfo>& Views,
        TArray<FAccessorInfo>& OutAccessors,
        FString& OutReason)
    {
        const TArray<TSharedPtr<FJsonValue>>* Accessors = nullptr;
        if (!TryGetArrayFieldSafe(Root, TEXT("accessors"), Accessors, true, false) || !Accessors ||
            Accessors->Num() > MAX_RUNTIME_ACCESSOR_COUNT)
        {
            OutReason = TEXT("accessors is missing, malformed, or exceeds the safety limit");
            return false;
        }

        OutAccessors.Reset(Accessors->Num());
        for (int32 AccessorIndex = 0; AccessorIndex < Accessors->Num(); ++AccessorIndex)
        {
            TSharedPtr<FJsonObject> AccessorObject;
            if (!TryGetJsonObjectValue((*Accessors)[AccessorIndex], AccessorObject))
            {
                OutReason = FString::Printf(TEXT("accessor %d is not a JSON object"), AccessorIndex);
                return false;
            }

            int64 Count = 0;
            int64 ComponentType64 = 0;
            int64 ByteOffset = 0;
            if (!TryGetIntegerField(AccessorObject, TEXT("count"), true, 0, 0, MAX_RUNTIME_ACCESSOR_ELEMENTS, Count) ||
                !TryGetIntegerField(AccessorObject, TEXT("componentType"), true, 0, 0, MAX_int32, ComponentType64) ||
                !TryGetIntegerField(AccessorObject, TEXT("byteOffset"), false, 0, 0, MAX_int32, ByteOffset))
            {
                OutReason = FString::Printf(TEXT("accessor %d contains invalid integer metadata"), AccessorIndex);
                return false;
            }

            FString Type;
            if (!AccessorObject->TryGetStringField(TEXT("type"), Type))
            {
                OutReason = FString::Printf(TEXT("accessor %d has no valid type"), AccessorIndex);
                return false;
            }

            int32 ElementByteSize = 0;
            if (!CalculateAccessorElementByteSize(static_cast<int32>(ComponentType64), Type, ElementByteSize))
            {
                OutReason = FString::Printf(TEXT("accessor %d uses an unsupported componentType/type combination"), AccessorIndex);
                return false;
            }

            int64 DenseBytes = 0;
            if (!CheckedMultiplyNonNegative(Count, ElementByteSize, DenseBytes) || DenseBytes > MAX_RUNTIME_ACCESSOR_BYTES)
            {
                OutReason = FString::Printf(TEXT("accessor %d requests an unsafe decoded size of %lld bytes"), AccessorIndex, DenseBytes);
                return false;
            }

            bool bHasBufferView = false;
            int32 BufferViewIndex = INDEX_NONE;
            if (!TryGetOptionalIndex(AccessorObject, TEXT("bufferView"), bHasBufferView, BufferViewIndex))
            {
                OutReason = FString::Printf(TEXT("accessor %d has an invalid bufferView index"), AccessorIndex);
                return false;
            }
            if (bHasBufferView)
            {
                if (!Views.IsValidIndex(BufferViewIndex) ||
                    !ValidateViewRange(Views[BufferViewIndex], ByteOffset, Count, ElementByteSize, Views[BufferViewIndex].ByteStride))
                {
                    OutReason = FString::Printf(TEXT("accessor %d exceeds its bufferView bounds"), AccessorIndex);
                    return false;
                }
            }
            else if (ByteOffset != 0)
            {
                // byteOffset is relative to bufferView, so it has no valid meaning when the
                // bufferView is omitted.
                OutReason = FString::Printf(TEXT("accessor %d has a byteOffset without a bufferView"), AccessorIndex);
                return false;
            }

            // glTF 2.0 explicitly permits an accessor without a bufferView. Its dense storage is
            // initialized to zero, with an optional sparse object replacing selected elements.
            // Zero-only morph targets commonly use this compact representation.

            if (!ValidateSparseAccessor(AccessorObject, Count, ElementByteSize, Views, OutReason, AccessorIndex))
            {
                return false;
            }

            FAccessorInfo Info;
            Info.Count = Count;
            Info.ComponentType = static_cast<int32>(ComponentType64);
            Info.ElementByteSize = ElementByteSize;
            Info.Type = MoveTemp(Type);
            OutAccessors.Add(MoveTemp(Info));
        }
        return true;
    }

    bool TryGetAccessorIndexFromValue(const TSharedPtr<FJsonValue>& Value, const TArray<FAccessorInfo>& Accessors, int32& OutAccessorIndex)
    {
        if (!TryGetArrayIndex(Value, OutAccessorIndex))
        {
            return false;
        }
        return Accessors.IsValidIndex(OutAccessorIndex);
    }

    bool CalculateExpandedPrimitiveIndexCount(const int64 SourceElementCount, const int64 Mode, int64& OutExpandedCount)
    {
        OutExpandedCount = 0;
        if (SourceElementCount <= 0)
        {
            return false;
        }

        if (Mode == 4) // TRIANGLES
        {
            OutExpandedCount = SourceElementCount;
            return (OutExpandedCount % 3) == 0;
        }
        if (Mode == 5 || Mode == 6) // TRIANGLE_STRIP / TRIANGLE_FAN
        {
            if (SourceElementCount < 3)
            {
                return false;
            }
            return CheckedMultiplyNonNegative(SourceElementCount - 2, 3, OutExpandedCount);
        }
        return false;
    }

    bool ValidateMeshPrimitives(
        const TSharedPtr<FJsonObject>& Root,
        const TArray<FAccessorInfo>& Accessors,
        FString& OutReason)
    {
        const TArray<TSharedPtr<FJsonValue>>* Meshes = nullptr;
        if (!TryGetRequiredArray(Root, TEXT("meshes"), Meshes) || !Meshes || Meshes->Num() > MAX_RUNTIME_MESH_COUNT)
        {
            OutReason = TEXT("meshes is missing, empty, malformed, or exceeds the safety limit");
            return false;
        }

        for (int32 MeshIndex = 0; MeshIndex < Meshes->Num(); ++MeshIndex)
        {
            TSharedPtr<FJsonObject> MeshObject;
            if (!TryGetJsonObjectValue((*Meshes)[MeshIndex], MeshObject))
            {
                OutReason = FString::Printf(TEXT("mesh %d is not a JSON object"), MeshIndex);
                return false;
            }

            const TArray<TSharedPtr<FJsonValue>>* Primitives = nullptr;
            if (!TryGetRequiredArray(MeshObject, TEXT("primitives"), Primitives) || !Primitives ||
                Primitives->Num() > MAX_RUNTIME_PRIMITIVES_PER_MESH)
            {
                OutReason = FString::Printf(TEXT("mesh %d has no valid primitives or exceeds %d primitives"),
                    MeshIndex, MAX_RUNTIME_PRIMITIVES_PER_MESH);
                return false;
            }

            int64 TotalBuildVertices = 0;
            int64 TotalBuildIndices = 0;
            for (int32 PrimitiveIndex = 0; PrimitiveIndex < Primitives->Num(); ++PrimitiveIndex)
            {
                TSharedPtr<FJsonObject> PrimitiveObject;
                if (!TryGetJsonObjectValue((*Primitives)[PrimitiveIndex], PrimitiveObject))
                {
                    OutReason = FString::Printf(TEXT("mesh %d primitive %d is not a JSON object"), MeshIndex, PrimitiveIndex);
                    return false;
                }

                TSharedPtr<FJsonObject> AttributesObject;
                if (!TryGetObjectFieldSafe(PrimitiveObject, TEXT("attributes"), AttributesObject, true))
                {
                    OutReason = FString::Printf(TEXT("mesh %d primitive %d has no valid attributes object"), MeshIndex, PrimitiveIndex);
                    return false;
                }

                const TSharedPtr<FJsonValue>* PositionValue = AttributesObject->Values.Find(TEXT("POSITION"));
                int32 PositionAccessorIndex = INDEX_NONE;
                if (!PositionValue || !TryGetAccessorIndexFromValue(*PositionValue, Accessors, PositionAccessorIndex))
                {
                    OutReason = FString::Printf(TEXT("mesh %d primitive %d has no valid POSITION accessor"), MeshIndex, PrimitiveIndex);
                    return false;
                }

                const FAccessorInfo& PositionAccessor = Accessors[PositionAccessorIndex];
                if (PositionAccessor.Type != TEXT("VEC3") || PositionAccessor.Count <= 0 ||
                    PositionAccessor.Count > MAX_RUNTIME_MESH_VERTICES)
                {
                    OutReason = FString::Printf(TEXT("mesh %d primitive %d has an unsafe POSITION count of %lld"),
                        MeshIndex, PrimitiveIndex, PositionAccessor.Count);
                    return false;
                }

                int32 UVChannelCount = 0;
                for (const TPair<FString, TSharedPtr<FJsonValue>>& AttributePair : AttributesObject->Values)
                {
                    int32 AttributeAccessorIndex = INDEX_NONE;
                    if (!TryGetAccessorIndexFromValue(AttributePair.Value, Accessors, AttributeAccessorIndex) ||
                        Accessors[AttributeAccessorIndex].Count != PositionAccessor.Count)
                    {
                        OutReason = FString::Printf(TEXT("mesh %d primitive %d attribute '%s' is invalid or has a mismatched count"),
                            MeshIndex, PrimitiveIndex, *AttributePair.Key);
                        return false;
                    }

                    if (AttributePair.Key.StartsWith(TEXT("TEXCOORD_"), ESearchCase::CaseSensitive))
                    {
                        ++UVChannelCount;
                        const FString ChannelText = AttributePair.Key.RightChop(9);
                        int32 ChannelIndex = INDEX_NONE;
                        if (!LexTryParseString(ChannelIndex, *ChannelText) || ChannelIndex < 0 || ChannelIndex >= MAX_RUNTIME_UV_CHANNELS)
                        {
                            OutReason = FString::Printf(TEXT("mesh %d primitive %d uses unsupported UV channel '%s'"),
                                MeshIndex, PrimitiveIndex, *AttributePair.Key);
                            return false;
                        }
                    }
                }
                if (UVChannelCount > MAX_RUNTIME_UV_CHANNELS)
                {
                    OutReason = FString::Printf(TEXT("mesh %d primitive %d exceeds %d UV channels"),
                        MeshIndex, PrimitiveIndex, MAX_RUNTIME_UV_CHANNELS);
                    return false;
                }

                int64 Mode = 4;
                if (!TryGetIntegerField(PrimitiveObject, TEXT("mode"), false, 4, 0, 6, Mode))
                {
                    OutReason = FString::Printf(TEXT("mesh %d primitive %d has an invalid mode"), MeshIndex, PrimitiveIndex);
                    return false;
                }

                bool bHasIndices = false;
                int32 IndexAccessorIndex = INDEX_NONE;
                if (!TryGetOptionalIndex(PrimitiveObject, TEXT("indices"), bHasIndices, IndexAccessorIndex) ||
                    (bHasIndices && !Accessors.IsValidIndex(IndexAccessorIndex)))
                {
                    OutReason = FString::Printf(TEXT("mesh %d primitive %d has an invalid indices accessor"), MeshIndex, PrimitiveIndex);
                    return false;
                }

                int64 SourceIndexCount = PositionAccessor.Count;
                if (bHasIndices)
                {
                    const FAccessorInfo& IndexAccessor = Accessors[IndexAccessorIndex];
                    if (IndexAccessor.Type != TEXT("SCALAR") ||
                        (IndexAccessor.ComponentType != 5121 && IndexAccessor.ComponentType != 5123 && IndexAccessor.ComponentType != 5125))
                    {
                        OutReason = FString::Printf(TEXT("mesh %d primitive %d uses an invalid index accessor format"), MeshIndex, PrimitiveIndex);
                        return false;
                    }
                    SourceIndexCount = IndexAccessor.Count;
                }

                int64 ExpandedIndexCount = 0;
                if (!CalculateExpandedPrimitiveIndexCount(SourceIndexCount, Mode, ExpandedIndexCount) ||
                    ExpandedIndexCount > MAX_RUNTIME_MESH_INDICES)
                {
                    OutReason = FString::Printf(TEXT("mesh %d primitive %d expands to an unsafe index count of %lld"),
                        MeshIndex, PrimitiveIndex, ExpandedIndexCount);
                    return false;
                }

                // glTFRuntime builds one vertex per POSITION for indexed primitives, and one build
                // vertex per expanded index for non-indexed primitives.
                const int64 BuildVertexCount = bHasIndices ? PositionAccessor.Count : ExpandedIndexCount;
                int64 NewTotalVertices = 0;
                int64 NewTotalIndices = 0;
                if (!CheckedAddNonNegative(TotalBuildVertices, BuildVertexCount, NewTotalVertices) ||
                    !CheckedAddNonNegative(TotalBuildIndices, ExpandedIndexCount, NewTotalIndices) ||
                    NewTotalVertices > MAX_RUNTIME_MESH_VERTICES || NewTotalIndices > MAX_RUNTIME_MESH_INDICES)
                {
                    OutReason = FString::Printf(TEXT("mesh %d exceeds safe LOD totals (vertices=%lld indices=%lld)"),
                        MeshIndex, NewTotalVertices, NewTotalIndices);
                    return false;
                }
                TotalBuildVertices = NewTotalVertices;
                TotalBuildIndices = NewTotalIndices;

                const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
                if (!TryGetArrayFieldSafe(PrimitiveObject, TEXT("targets"), Targets, false, false))
                {
                    OutReason = FString::Printf(TEXT("mesh %d primitive %d has malformed morph targets"), MeshIndex, PrimitiveIndex);
                    return false;
                }
                if (Targets && Targets->Num() > MAX_RUNTIME_MORPH_TARGETS_PER_PRIMITIVE)
                {
                    OutReason = FString::Printf(TEXT("mesh %d primitive %d exceeds the morph-target limit"), MeshIndex, PrimitiveIndex);
                    return false;
                }
                if (Targets)
                {
                    for (const TSharedPtr<FJsonValue>& TargetValue : *Targets)
                    {
                        TSharedPtr<FJsonObject> TargetObject;
                        if (!TryGetJsonObjectValue(TargetValue, TargetObject))
                        {
                            OutReason = FString::Printf(TEXT("mesh %d primitive %d has a non-object morph target"), MeshIndex, PrimitiveIndex);
                            return false;
                        }
                        for (const TPair<FString, TSharedPtr<FJsonValue>>& TargetPair : TargetObject->Values)
                        {
                            int32 TargetAccessorIndex = INDEX_NONE;
                            if (!TryGetAccessorIndexFromValue(TargetPair.Value, Accessors, TargetAccessorIndex) ||
                                Accessors[TargetAccessorIndex].Count != PositionAccessor.Count)
                            {
                                OutReason = FString::Printf(TEXT("mesh %d primitive %d morph target '%s' has a mismatched count"),
                                    MeshIndex, PrimitiveIndex, *TargetPair.Key);
                                return false;
                            }
                        }
                    }
                }
            }
        }
        return true;
    }
}

FString GlbValidation::NormalizePath(const FString& FilePath)
{
    FString Normalized = FPaths::ConvertRelativePathToFull(FilePath);
    FPaths::NormalizeFilename(Normalized);
    FPaths::CollapseRelativeDirectories(Normalized);
    return Normalized;
}

bool GlbValidation::ValidateFile(const FString& FilePath, FString& OutReason)
{
    OutReason.Reset();
    const FString NormalizedPath = NormalizePath(FilePath);

    if (NormalizedPath.IsEmpty())
    {
        OutReason = TEXT("empty path");
        return false;
    }
    if (!FPaths::GetExtension(NormalizedPath).Equals(TEXT("glb"), ESearchCase::IgnoreCase))
    {
        OutReason = TEXT("file extension is not .glb");
        return false;
    }

    const int64 FileSize = IFileManager::Get().FileSize(*NormalizedPath);
    if (FileSize == INDEX_NONE)
    {
        OutReason = TEXT("file does not exist or cannot be opened");
        return false;
    }
    if (FileSize < GLB_HEADER_AND_CHUNK_HEADER_SIZE)
    {
        OutReason = FString::Printf(TEXT("file is truncated (%lld bytes)"), FileSize);
        return false;
    }
    if (FileSize > static_cast<int64>(MAX_int32))
    {
        OutReason = FString::Printf(TEXT("file is too large for safe runtime loading (%lld bytes)"), FileSize);
        return false;
    }

    TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*NormalizedPath));
    if (!Reader.IsValid())
    {
        OutReason = TEXT("failed to create a file reader");
        return false;
    }

    FGlbHeader Header;
    if (!ReadHeader(*Reader, Header))
    {
        OutReason = TEXT("failed while reading the GLB header");
        return false;
    }
    if (Header.Magic != GLB_MAGIC)
    {
        OutReason = TEXT("invalid GLB magic");
        return false;
    }
    if (Header.Version != GLB_VERSION_2)
    {
        OutReason = FString::Printf(TEXT("unsupported GLB version %u (expected 2)"), Header.Version);
        return false;
    }
    if (static_cast<int64>(Header.DeclaredLength) != FileSize)
    {
        OutReason = FString::Printf(TEXT("declared length %u does not match file size %lld"), Header.DeclaredLength, FileSize);
        return false;
    }
    if (Header.JsonChunkType != GLB_JSON_CHUNK)
    {
        OutReason = TEXT("the first GLB chunk is not JSON");
        return false;
    }
    if (Header.JsonChunkLength == 0 || (Header.JsonChunkLength % 4u) != 0u ||
        static_cast<int64>(Header.JsonChunkLength) > FileSize - GLB_HEADER_AND_CHUNK_HEADER_SIZE)
    {
        OutReason = TEXT("invalid, unaligned, or truncated JSON chunk");
        return false;
    }

    int64 NextChunkOffset = GLB_HEADER_AND_CHUNK_HEADER_SIZE + static_cast<int64>(Header.JsonChunkLength);
    while (NextChunkOffset < FileSize)
    {
        if (FileSize - NextChunkOffset < 8)
        {
            OutReason = TEXT("truncated GLB chunk header");
            return false;
        }

        Reader->Seek(NextChunkOffset);
        uint32 ChunkLength = 0;
        uint32 ChunkType = 0;
        Reader->Serialize(&ChunkLength, sizeof(ChunkLength));
        Reader->Serialize(&ChunkType, sizeof(ChunkType));
        if (Reader->IsError() || ChunkLength == 0 || (ChunkLength % 4u) != 0u ||
            static_cast<int64>(ChunkLength) > FileSize - NextChunkOffset - 8)
        {
            OutReason = TEXT("invalid, unaligned, or truncated GLB data chunk");
            return false;
        }
        NextChunkOffset += 8 + static_cast<int64>(ChunkLength);
    }

    if (NextChunkOffset != FileSize)
    {
        OutReason = TEXT("GLB chunks do not end at the declared file length");
        return false;
    }
    return true;
}

bool GlbValidation::ValidateRuntimeMeshFile(const FString& FilePath, FString& OutReason)
{
    const FString NormalizedPath = NormalizePath(FilePath);
    const FString ValidationCacheKey = MakeRuntimeValidationCacheKey(NormalizedPath);
    const int64 FileSize = IFileManager::Get().FileSize(*NormalizedPath);
    const FDateTime Timestamp = IFileManager::Get().GetTimeStamp(*NormalizedPath);

    bool bCachedValid = false;
    if (FileSize != INDEX_NONE &&
        TryGetCachedRuntimeValidation(ValidationCacheKey, FileSize, Timestamp, bCachedValid, OutReason))
    {
        return bCachedValid;
    }

    if (!ValidateFile(NormalizedPath, OutReason))
    {
        if (FileSize != INDEX_NONE)
        {
            CacheRuntimeValidation(ValidationCacheKey, FileSize, Timestamp, false, OutReason);
        }
        return false;
    }

    FParsedGlbJson Parsed;
    if (!LoadRootJsonForValidation(NormalizedPath, Parsed, OutReason))
    {
        CacheRuntimeValidation(ValidationCacheKey, FileSize, Timestamp, false, OutReason);
        return false;
    }

    TArray<FBufferInfo> Buffers;
    TArray<FBufferViewInfo> BufferViews;
    TArray<FAccessorInfo> Accessors;
    if (!ParseAndValidateBuffers(Parsed.Root, Parsed.BinaryChunkSize, Buffers, OutReason) ||
        !ParseAndValidateBufferViews(Parsed.Root, Buffers, BufferViews, OutReason) ||
        !ParseAndValidateAccessors(Parsed.Root, BufferViews, Accessors, OutReason) ||
        !ValidateMeshPrimitives(Parsed.Root, Accessors, OutReason))
    {
        CacheRuntimeValidation(ValidationCacheKey, FileSize, Timestamp, false, OutReason);
        return false;
    }

    OutReason.Reset();
    CacheRuntimeValidation(ValidationCacheKey, FileSize, Timestamp, true, OutReason);
    return true;
}

bool GlbValidation::ValidateCharacterFile(const FString& FilePath, FString& OutReason)
{
    if (!ValidateRuntimeMeshFile(FilePath, OutReason))
    {
        return false;
    }

    const FString NormalizedPath = NormalizePath(FilePath);
    const int64 CharacterFileSize = IFileManager::Get().FileSize(*NormalizedPath);
    if (CharacterFileSize < 0 || CharacterFileSize > MAX_CHARACTER_GLB_BYTES)
    {
        OutReason = FString::Printf(TEXT("character GLB exceeds the safe runtime size limit (%lld bytes)"), CharacterFileSize);
        return false;
    }

    FParsedGlbJson Parsed;
    if (!LoadRootJsonForValidation(NormalizedPath, Parsed, OutReason))
    {
        return false;
    }
    if (Parsed.JsonChunkLength > MAX_CHARACTER_JSON_CHUNK_BYTES)
    {
        OutReason = FString::Printf(TEXT("character JSON chunk is too large (%u bytes)"), Parsed.JsonChunkLength);
        return false;
    }

    const TSharedPtr<FJsonObject>& RootObject = Parsed.Root;
    const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Meshes = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Skins = nullptr;
    if (!TryGetRequiredArray(RootObject, TEXT("nodes"), Nodes) ||
        !TryGetRequiredArray(RootObject, TEXT("meshes"), Meshes) ||
        !TryGetRequiredArray(RootObject, TEXT("skins"), Skins))
    {
        OutReason = TEXT("character GLB requires non-empty nodes, meshes, and skins arrays");
        return false;
    }
    if (Nodes->Num() > MAX_CHARACTER_NODE_COUNT || Meshes->Num() > MAX_CHARACTER_MESH_COUNT ||
        Skins->Num() > MAX_CHARACTER_SKIN_COUNT)
    {
        OutReason = FString::Printf(TEXT("character GLB exceeds safe structure limits (nodes=%d meshes=%d skins=%d)"),
            Nodes->Num(), Meshes->Num(), Skins->Num());
        return false;
    }

    for (int32 SkinIndex = 0; SkinIndex < Skins->Num(); ++SkinIndex)
    {
        TSharedPtr<FJsonObject> SkinObject;
        if (!TryGetJsonObjectValue((*Skins)[SkinIndex], SkinObject))
        {
            OutReason = FString::Printf(TEXT("skin %d is not a JSON object"), SkinIndex);
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Joints = nullptr;
        if (!TryGetRequiredArray(SkinObject, TEXT("joints"), Joints) || Joints->Num() > MAX_CHARACTER_JOINT_COUNT)
        {
            OutReason = FString::Printf(TEXT("skin %d has no valid joints array or exceeds the joint limit"), SkinIndex);
            return false;
        }
        for (const TSharedPtr<FJsonValue>& JointValue : *Joints)
        {
            int32 JointIndex = INDEX_NONE;
            if (!TryGetArrayIndex(JointValue, JointIndex) || !Nodes->IsValidIndex(JointIndex))
            {
                OutReason = FString::Printf(TEXT("skin %d references an invalid joint node"), SkinIndex);
                return false;
            }
        }

        bool bHasSkeleton = false;
        int32 SkeletonIndex = INDEX_NONE;
        if (!TryGetOptionalIndex(SkinObject, TEXT("skeleton"), bHasSkeleton, SkeletonIndex) ||
            (bHasSkeleton && !Nodes->IsValidIndex(SkeletonIndex)))
        {
            OutReason = FString::Printf(TEXT("skin %d references an invalid skeleton node"), SkinIndex);
            return false;
        }
    }

    bool bFoundSkinnedMeshNode = false;
    TArray<TArray<int32>> NodeChildren;
    NodeChildren.SetNum(Nodes->Num());
    TArray<int32> NodeParentCounts;
    NodeParentCounts.Init(0, Nodes->Num());
    for (int32 NodeIndex = 0; NodeIndex < Nodes->Num(); ++NodeIndex)
    {
        TSharedPtr<FJsonObject> NodeObject;
        if (!TryGetJsonObjectValue((*Nodes)[NodeIndex], NodeObject))
        {
            OutReason = FString::Printf(TEXT("node %d is not a JSON object"), NodeIndex);
            return false;
        }

        if (!ValidateOptionalFiniteArray(NodeObject, TEXT("matrix"), 16) ||
            !ValidateOptionalFiniteArray(NodeObject, TEXT("translation"), 3) ||
            !ValidateOptionalFiniteArray(NodeObject, TEXT("rotation"), 4) ||
            !ValidateOptionalFiniteArray(NodeObject, TEXT("scale"), 3))
        {
            OutReason = FString::Printf(TEXT("node %d contains an invalid transform array"), NodeIndex);
            return false;
        }

        bool bHasMesh = false;
        bool bHasSkin = false;
        int32 MeshIndex = INDEX_NONE;
        int32 SkinIndex = INDEX_NONE;
        if (!TryGetOptionalIndex(NodeObject, TEXT("mesh"), bHasMesh, MeshIndex) ||
            !TryGetOptionalIndex(NodeObject, TEXT("skin"), bHasSkin, SkinIndex))
        {
            OutReason = FString::Printf(TEXT("node %d contains a non-integer mesh or skin index"), NodeIndex);
            return false;
        }
        if ((bHasMesh && !Meshes->IsValidIndex(MeshIndex)) || (bHasSkin && !Skins->IsValidIndex(SkinIndex)))
        {
            OutReason = FString::Printf(TEXT("node %d references an out-of-range mesh or skin"), NodeIndex);
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
        if (!TryGetArrayFieldSafe(NodeObject, TEXT("children"), Children, false, false))
        {
            OutReason = FString::Printf(TEXT("node %d has a malformed children array"), NodeIndex);
            return false;
        }
        if (Children)
        {
            for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
            {
                int32 ChildIndex = INDEX_NONE;
                if (!TryGetArrayIndex(ChildValue, ChildIndex) || !Nodes->IsValidIndex(ChildIndex) || ChildIndex == NodeIndex)
                {
                    OutReason = FString::Printf(TEXT("node %d references an invalid child node"), NodeIndex);
                    return false;
                }
                if (++NodeParentCounts[ChildIndex] > 1)
                {
                    OutReason = FString::Printf(TEXT("node %d is referenced by multiple parents"), ChildIndex);
                    return false;
                }
                NodeChildren[NodeIndex].Add(ChildIndex);
            }
        }

        bFoundSkinnedMeshNode |= bHasMesh && bHasSkin;
    }

    if (!bFoundSkinnedMeshNode)
    {
        OutReason = TEXT("no node references both a valid mesh and a valid skin");
        return false;
    }

    // Reject cycles before glTFRuntime recursively walks the hierarchy.
    TArray<uint8> VisitState;
    VisitState.Init(0, Nodes->Num());
    for (int32 RootIndex = 0; RootIndex < Nodes->Num(); ++RootIndex)
    {
        if (VisitState[RootIndex] != 0)
        {
            continue;
        }

        TArray<TPair<int32, int32>> Stack;
        Stack.Emplace(RootIndex, 0);
        VisitState[RootIndex] = 1;
        while (Stack.Num() > 0)
        {
            TPair<int32, int32>& Frame = Stack.Last();
            const int32 NodeIndex = Frame.Key;
            if (Frame.Value >= NodeChildren[NodeIndex].Num())
            {
                VisitState[NodeIndex] = 2;
                Stack.Pop(EAllowShrinking::No);
                continue;
            }

            const int32 ChildIndex = NodeChildren[NodeIndex][Frame.Value++];
            if (VisitState[ChildIndex] == 1)
            {
                OutReason = FString::Printf(TEXT("character GLB contains a cyclic node hierarchy near node %d"), ChildIndex);
                return false;
            }
            if (VisitState[ChildIndex] == 0)
            {
                VisitState[ChildIndex] = 1;
                Stack.Emplace(ChildIndex, 0);
            }
        }
    }

    OutReason.Reset();
    return true;
}
