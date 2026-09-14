// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldArchive.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Misc/ScopeLock.h"
#include "System/WorldArchive.h"

#include "Async/ParallelFor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Crc.h"
#include "Misc/Compression.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "System/SafeFileIO.h"
#include "System/WorldArchiveCodec.h"

namespace GWorldArchivePrivate
{
    // Development archive identity only. Intentionally no public/spec version is encoded yet;
    // incompatible development archives are rebuilt instead of carrying premature version logic.
    constexpr uint64 Magic = 0x00444C524F574747ull; // ASCII "GGWORLD\0", little endian.
    constexpr int32 HeaderBytes = 64;
    constexpr int64 PayloadAlignment = 4096;
    constexpr int32 MaxStringBytes = 64 * 1024 * 1024;
    constexpr int32 MaxNameBytes = 4096;
    constexpr int32 MaxNodesPerModel = 2000000;
    constexpr int32 MaxMeshesPerModel = 1000000;
    constexpr int32 MaxBonesPerModel = 65536;
    constexpr int32 MaxCollidersPerMesh = 65536;
    constexpr int32 MaxLightsPerMesh = 65536;
    constexpr int64 MaxArchiveBytes = 16ll * 1024ll * 1024ll * 1024ll * 1024ll;
    constexpr double FineChunkCm = 512.0 * 100.0;
    constexpr double CoarseChunkCm = 8192.0 * 100.0;
    constexpr int64 FinePerCoarse = 16;

    bool TryComputeSpatialChunk(const FVector& Location, FIntVector& OutCoarse, FIntVector& OutFine)
    {
        const auto Floor32 = [](const double Value, const double Cell, int64& Out)
        {
            if (!FMath::IsFinite(Value)) return false;
            const double Floored = FMath::FloorToDouble(Value / Cell);
            if (!FMath::IsFinite(Floored)
                || Floored < static_cast<double>(MIN_int32)
                || Floored > static_cast<double>(MAX_int32)) return false;
            Out = static_cast<int64>(Floored);
            return true;
        };

        int64 CoarseX = 0, CoarseY = 0, CoarseZ = 0;
        int64 FineXGlobal = 0, FineYGlobal = 0, FineZGlobal = 0;
        if (!Floor32(Location.X, CoarseChunkCm, CoarseX)
            || !Floor32(Location.Y, CoarseChunkCm, CoarseY)
            || !Floor32(Location.Z, CoarseChunkCm, CoarseZ)
            || !Floor32(Location.X, FineChunkCm, FineXGlobal)
            || !Floor32(Location.Y, FineChunkCm, FineYGlobal)
            || !Floor32(Location.Z, FineChunkCm, FineZGlobal)) return false;

        const int64 FineX = FineXGlobal - CoarseX * FinePerCoarse;
        const int64 FineY = FineYGlobal - CoarseY * FinePerCoarse;
        const int64 FineZ = FineZGlobal - CoarseZ * FinePerCoarse;
        if (FineX < 0 || FineX >= FinePerCoarse
            || FineY < 0 || FineY >= FinePerCoarse
            || FineZ < 0 || FineZ >= FinePerCoarse) return false;

        OutCoarse = FIntVector(static_cast<int32>(CoarseX), static_cast<int32>(CoarseY), static_cast<int32>(CoarseZ));
        OutFine = FIntVector(static_cast<int32>(FineX), static_cast<int32>(FineY), static_cast<int32>(FineZ));
        return true;
    }

    /** Matches fixed-width GUID digit ordering without allocation inside O(N log N) sorts. */
    bool IsGuidLess(const FGuid& Left, const FGuid& Right)
    {
        if (Left.A != Right.A) return Left.A < Right.A;
        if (Left.B != Right.B) return Left.B < Right.B;
        if (Left.C != Right.C) return Left.C < Right.C;
        return Left.D < Right.D;
    }

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

    bool IsFiniteTransform(const FTransform& Value)
    {
        return !Value.ContainsNaN() && IsFiniteVector(Value.GetLocation())
            && IsFiniteVector(Value.GetScale3D()) && IsFiniteQuat(Value.GetRotation())
            && Value.GetRotation().IsNormalized();
    }

    bool IsSaneMeshSettings(const FMeshData& Data)
    {
        if (Data.Colliders.Num() > MaxCollidersPerMesh
            || Data.Lights.Num() > MaxLightsPerMesh)
        {
            return false;
        }
        for (const FModelCollider& Collider : Data.Colliders)
        {
            if (static_cast<uint8>(Collider.Collider) > static_cast<uint8>(EColliderType::Box)
                || !IsFiniteVector(Collider.Center) || !IsFiniteVector(Collider.Size)
                || Collider.Size.GetMin() < 0.0)
            {
                return false;
            }
        }
        for (const FLightData& Light : Data.Lights)
        {
            if (!IsFiniteVector(Light.Location) || !FMath::IsFinite(Light.Intensity)
                || !FMath::IsFinite(Light.SourceRadius)
                || !FMath::IsFinite(Light.SoftSourceRadius)
                || !FMath::IsFinite(Light.AttenuationRadius)
                || !FMath::IsFinite(Light.Length)
                || static_cast<uint8>(Light.Unit) > static_cast<uint8>(ELightUnits::Nits)
                || Light.SourceRadius < 0.0f || Light.SoftSourceRadius < 0.0f
                || Light.AttenuationRadius < 0.0f || Light.Length < 0.0f)
            {
                return false;
            }
        }
        return true;
    }

    class FWriter
    {
    public:
        explicit FWriter(TArray<uint8>& InBytes, const int64 InMaxBytes = MAX_int32)
            : Bytes(InBytes)
            , MaxBytes(FMath::Clamp<int64>(InMaxBytes, 1, MAX_int32))
        {
            Bytes.Reset();
        }
        bool IsOk() const { return bOk; }

        void U8(uint8 Value)
        {
            if (!CanAppend(1)) return;
            Bytes.Add(Value);
        }
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
        void String(const FString& Value, int32 Limit = MaxStringBytes)
        {
            FTCHARToUTF8 Utf8(*Value);
            if (Utf8.Length() < 0 || Utf8.Length() > Limit)
            {
                bOk = false;
                return;
            }
            U32(static_cast<uint32>(Utf8.Length()));
            if (bOk && Utf8.Length() > 0)
            {
                Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
            }
        }
        void Name(const FName Value) { String(Value.ToString(), MaxNameBytes); }
        void Vector(const FVector& Value) { Double(Value.X); Double(Value.Y); Double(Value.Z); }
        void Quat(const FQuat& Value)
        {
            Double(Value.X); Double(Value.Y); Double(Value.Z); Double(Value.W);
        }
        void Transform(const FTransform& Value)
        {
            Quat(Value.GetRotation()); Vector(Value.GetLocation()); Vector(Value.GetScale3D());
        }
    private:
        bool CanAppend(const int64 Count)
        {
            if (!bOk || Count < 0 || Count > MaxBytes - static_cast<int64>(Bytes.Num()))
            {
                bOk = false;
                return false;
            }
            return true;
        }

        void Append(const uint8* Data, const int32 Count)
        {
            if (!Data || Count <= 0 || !CanAppend(Count))
            {
                if (Count != 0) bOk = false;
                return;
            }
            Bytes.Append(Data, Count);
        }

        TArray<uint8>& Bytes;
        int64 MaxBytes = MAX_int32;
        bool bOk = true;
    };

    class FReader
    {
    public:
        explicit FReader(const TArray<uint8>& InBytes) : Bytes(InBytes) {}
        bool IsOk() const { return bOk; }
        bool IsAtEnd() const { return bOk && Offset == Bytes.Num(); }
        int32 Remaining() const { return bOk ? Bytes.Num() - Offset : 0; }

        uint8 U8()
        {
            if (!Require(1)) return 0;
            return Bytes[Offset++];
        }
        uint16 U16()
        {
            const uint16 A = U8();
            const uint16 B = U8();
            return A | (B << 8u);
        }
        uint32 U32()
        {
            const uint32 A = U8();
            const uint32 B = U8();
            const uint32 C = U8();
            const uint32 D = U8();
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
            // Function-argument evaluation order is not a serialization contract. Read fields in
            // their exact on-disk order before constructing the GUID.
            const uint32 A = U32();
            const uint32 B = U32();
            const uint32 C = U32();
            const uint32 D = U32();
            return FGuid(A, B, C, D);
        }
        FString String(int32 Limit = MaxStringBytes)
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
        FName Name() { return FName(*String(MaxNameBytes)); }
        FVector Vector()
        {
            // Never rely on constructor-argument evaluation order for an on-disk sequence.
            const double X = Double();
            const double Y = Double();
            const double Z = Double();
            return FVector(X, Y, Z);
        }
        FQuat Quat()
        {
            const double X = Double();
            const double Y = Double();
            const double Z = Double();
            const double W = Double();
            return FQuat(X, Y, Z, W);
        }
        FTransform Transform()
        {
            const FQuat Rotation = Quat();
            const FVector Location = Vector();
            const FVector Scale = Vector();
            return FTransform(Rotation, Location, Scale);
        }
        uint32 Count(const uint32 Limit, const int32 MinimumBytes = 0)
        {
            const uint32 Value = U32();
            if (!bOk || Value > Limit || (MinimumBytes > 0
                && uint64(Value) * uint64(MinimumBytes) > uint64(Remaining())))
            {
                bOk = false;
            }
            return bOk ? Value : 0;
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

    template <typename ValueType, typename WriteValue>
    void WriteSortedStringMap(FWriter& Writer, const TMap<FString, ValueType>& Values, WriteValue&& Write)
    {
        TArray<FString> Keys;
        Values.GetKeys(Keys);
        Keys.Sort();
        Writer.U32(static_cast<uint32>(Keys.Num()));
        for (const FString& Key : Keys)
        {
            Writer.String(Key, MaxNameBytes);
            Write(Values.FindChecked(Key));
        }
    }

    void WriteMeshSettings(FWriter& Writer, const FMeshData& Data)
    {
        Writer.U8(Data.bComplexCollision ? 1 : 0);
        Writer.U8(Data.bSimpleCollision ? 1 : 0);
        Writer.U8(Data.bIsEntity ? 1 : 0);
        Writer.U8(0);
        Writer.U32(static_cast<uint32>(Data.Colliders.Num()));
        for (const FModelCollider& Collider : Data.Colliders)
        {
            Writer.U8(static_cast<uint8>(Collider.Collider));
            Writer.U8(0); Writer.U16(0);
            Writer.Vector(Collider.Center);
            Writer.Vector(Collider.Size);
        }
        Writer.U32(static_cast<uint32>(Data.Lights.Num()));
        for (const FLightData& Light : Data.Lights)
        {
            Writer.Vector(Light.Location);
            Writer.Float(Light.Intensity);
            Writer.Float(Light.SourceRadius);
            Writer.Float(Light.SoftSourceRadius);
            Writer.Float(Light.AttenuationRadius);
            Writer.Float(Light.Length);
            Writer.U8(static_cast<uint8>(Light.Unit));
            Writer.U8(0); Writer.U16(0);
        }
    }

    bool ReadMeshSettings(FReader& Reader, FMeshData& OutData)
    {
        OutData = FMeshData();
        OutData.bComplexCollision = Reader.U8() != 0;
        OutData.bSimpleCollision = Reader.U8() != 0;
        OutData.bIsEntity = Reader.U8() != 0;
        (void)Reader.U8();
        const uint32 ColliderCount = Reader.Count(MaxCollidersPerMesh, 52);
        if (!Reader.IsOk()) return false;
        OutData.Colliders.Reserve(static_cast<int32>(ColliderCount));
        for (uint32 Index = 0; Index < ColliderCount; ++Index)
        {
            FModelCollider& Collider = OutData.Colliders.AddDefaulted_GetRef();
            const uint8 ColliderType = Reader.U8();
            (void)Reader.U8(); (void)Reader.U16();
            Collider.Center = Reader.Vector();
            Collider.Size = Reader.Vector();
            if (ColliderType > static_cast<uint8>(EColliderType::Box)
                || !IsFiniteVector(Collider.Center) || !IsFiniteVector(Collider.Size)) return false;
            Collider.Collider = static_cast<EColliderType>(ColliderType);
        }
        const uint32 LightCount = Reader.Count(MaxLightsPerMesh, 48);
        if (!Reader.IsOk()) return false;
        OutData.Lights.Reserve(static_cast<int32>(LightCount));
        for (uint32 Index = 0; Index < LightCount; ++Index)
        {
            FLightData& Light = OutData.Lights.AddDefaulted_GetRef();
            Light.Location = Reader.Vector();
            Light.Intensity = Reader.Float();
            Light.SourceRadius = Reader.Float();
            Light.SoftSourceRadius = Reader.Float();
            Light.AttenuationRadius = Reader.Float();
            Light.Length = Reader.Float();
            Light.Unit = static_cast<ELightUnits>(Reader.U8());
            (void)Reader.U8(); (void)Reader.U16();
            if (!IsFiniteVector(Light.Location) || !FMath::IsFinite(Light.Intensity)
                || !FMath::IsFinite(Light.SourceRadius) || !FMath::IsFinite(Light.SoftSourceRadius)
                || !FMath::IsFinite(Light.AttenuationRadius) || !FMath::IsFinite(Light.Length)) return false;
        }
        return Reader.IsOk() && IsSaneMeshSettings(OutData);
    }

    void WriteMetadata(
        FWriter& Writer,
        const FGWorldModelMetadata& Metadata,
        const FGWorldModelSummary& Summary)
    {
        Writer.I32(Summary.SourceNodeCount);
        Writer.I32(Summary.SourceMeshCount);
        Writer.I32(Summary.SourceMaterialCount);
        Writer.I32(Summary.SourceTextureCount);
        Writer.Vector(Summary.Center);
        Writer.Vector(Summary.Size);

        // This complete index-addressed table preserves hierarchy nodes that the render-oriented
        // NodeMap intentionally omits (for example empty pivots and skeleton helpers).
        Writer.U32(static_cast<uint32>(Metadata.NodeTransforms.Num()));
        for (const FGWorldNodeTransform& Node : Metadata.NodeTransforms)
        {
            Writer.I32(Node.NodeIndex);
            Writer.I32(Node.ParentIndex);
            Writer.I32(Node.MeshIndex);
            Writer.I32(Node.SkinIndex);
            Writer.String(Node.Name, MaxNameBytes);
            Writer.Transform(Node.LocalTransform);
        }

        TArray<FName> NodeNames;
        Metadata.SceneData.NodeMap.GetKeys(NodeNames);
        NodeNames.Sort([](const FName A, const FName B) { return A.LexicalLess(B); });
        Writer.U32(static_cast<uint32>(NodeNames.Num()));
        for (const FName NodeName : NodeNames)
        {
            const FModelNodeData& Node = Metadata.SceneData.NodeMap.FindChecked(NodeName);
            Writer.Name(NodeName);
            Writer.Name(Node.MeshName);
            Writer.Transform(Node.Transform);
            Writer.I32(Node.CoarseChunk.X); Writer.I32(Node.CoarseChunk.Y); Writer.I32(Node.CoarseChunk.Z);
            Writer.I32(Node.FineChunk.X); Writer.I32(Node.FineChunk.Y); Writer.I32(Node.FineChunk.Z);
            Writer.U8(Node.bAlwaysLoaded ? 1 : 0); Writer.U8(0); Writer.U16(0);
        }

        TArray<FName> WaterNames;
        Metadata.SceneData.WaterNodeMap.GetKeys(WaterNames);
        WaterNames.Sort([](const FName A, const FName B) { return A.LexicalLess(B); });
        Writer.U32(static_cast<uint32>(WaterNames.Num()));
        for (const FName NodeName : WaterNames)
        {
            const FWaterStreamNodeData& Water = Metadata.SceneData.WaterNodeMap.FindChecked(NodeName);
            Writer.Name(NodeName);
            Writer.Transform(Water.Transform);
            Writer.Float(Water.StreamRadius);
        }

        TArray<FName> MeshNames;
        Metadata.SceneData.MeshMap.GetKeys(MeshNames);
        MeshNames.Sort([](const FName A, const FName B) { return A.LexicalLess(B); });
        Writer.U32(static_cast<uint32>(MeshNames.Num()));
        for (const FName MeshName : MeshNames)
        {
            const FModelMeshData& Mesh = Metadata.SceneData.MeshMap.FindChecked(MeshName);
            Writer.Name(MeshName);
            Writer.I32(Mesh.LOD0); Writer.I32(Mesh.LOD1);
            Writer.I32(Mesh.LOD2); Writer.I32(Mesh.LOD3);
            Writer.Vector(Mesh.Extent);
            Writer.Vector(Mesh.Size);
            WriteMeshSettings(Writer, Mesh.Data);
        }

        TArray<FName> SettingNames;
        Metadata.SceneData.ModelData.MeshData.GetKeys(SettingNames);
        SettingNames.Sort([](const FName A, const FName B) { return A.LexicalLess(B); });
        Writer.U32(static_cast<uint32>(SettingNames.Num()));
        for (const FName Name : SettingNames)
        {
            Writer.Name(Name);
            WriteMeshSettings(Writer, Metadata.SceneData.ModelData.MeshData.FindChecked(Name));
        }
    }

    bool ReadMetadata(FReader& Reader, FGWorldModelMetadata& OutMetadata)
    {
        OutMetadata = FGWorldModelMetadata();
        OutMetadata.SourceNodeCount = Reader.I32();
        OutMetadata.SourceMeshCount = Reader.I32();
        OutMetadata.SourceMaterialCount = Reader.I32();
        OutMetadata.SourceTextureCount = Reader.I32();
        OutMetadata.SceneData.ModelData.Center = Reader.Vector();
        OutMetadata.SceneData.ModelData.Size = Reader.Vector();

        const uint32 TransformCount = Reader.Count(MaxNodesPerModel, 100);
        if (!Reader.IsOk()) return false;
        OutMetadata.NodeTransforms.Reserve(static_cast<int32>(TransformCount));
        for (uint32 Index = 0; Index < TransformCount; ++Index)
        {
            FGWorldNodeTransform& Node = OutMetadata.NodeTransforms.AddDefaulted_GetRef();
            Node.NodeIndex = Reader.I32();
            Node.ParentIndex = Reader.I32();
            Node.MeshIndex = Reader.I32();
            Node.SkinIndex = Reader.I32();
            Node.Name = Reader.String(MaxNameBytes);
            Node.LocalTransform = Reader.Transform();
            if (!Reader.IsOk() || !IsFiniteTransform(Node.LocalTransform)) return false;
        }

        const uint32 NodeCount = Reader.Count(MaxNodesPerModel, 88);
        if (!Reader.IsOk()) return false;
        OutMetadata.SceneData.NodeMap.Reserve(static_cast<int32>(NodeCount));
        for (uint32 Index = 0; Index < NodeCount; ++Index)
        {
            const FName Name = Reader.Name();
            FModelNodeData Node;
            Node.MeshName = Reader.Name();
            Node.Transform = Reader.Transform();
            Node.CoarseChunk.X = Reader.I32(); Node.CoarseChunk.Y = Reader.I32(); Node.CoarseChunk.Z = Reader.I32();
            Node.FineChunk.X = Reader.I32(); Node.FineChunk.Y = Reader.I32(); Node.FineChunk.Z = Reader.I32();
            Node.bAlwaysLoaded = Reader.U8() != 0; (void)Reader.U8(); (void)Reader.U16();
            if (!Reader.IsOk() || Name.IsNone() || Node.MeshName.IsNone()
                || !IsFiniteTransform(Node.Transform)
                || Node.FineChunk.X < 0 || Node.FineChunk.X >= 16
                || Node.FineChunk.Y < 0 || Node.FineChunk.Y >= 16
                || Node.FineChunk.Z < 0 || Node.FineChunk.Z >= 16
                || OutMetadata.SceneData.NodeMap.Contains(Name)) return false;
            OutMetadata.SceneData.NodeMap.Add(Name, MoveTemp(Node));
        }

        const uint32 WaterCount = Reader.Count(MaxNodesPerModel, 88);
        if (!Reader.IsOk()) return false;
        OutMetadata.SceneData.WaterNodeMap.Reserve(static_cast<int32>(WaterCount));
        for (uint32 Index = 0; Index < WaterCount; ++Index)
        {
            const FName Name = Reader.Name();
            FWaterStreamNodeData Water;
            Water.Transform = Reader.Transform();
            Water.StreamRadius = Reader.Float();
            if (!Reader.IsOk() || Name.IsNone() || !IsFiniteTransform(Water.Transform)
                || !FMath::IsFinite(Water.StreamRadius) || Water.StreamRadius <= 0.0f
                || OutMetadata.SceneData.WaterNodeMap.Contains(Name)) return false;
            OutMetadata.SceneData.WaterNodeMap.Add(Name, MoveTemp(Water));
        }

        const uint32 MeshCount = Reader.Count(MaxMeshesPerModel, 80);
        if (!Reader.IsOk()) return false;
        OutMetadata.SceneData.MeshMap.Reserve(static_cast<int32>(MeshCount));
        for (uint32 Index = 0; Index < MeshCount; ++Index)
        {
            const FName Name = Reader.Name();
            FModelMeshData Mesh;
            Mesh.LOD0 = Reader.I32(); Mesh.LOD1 = Reader.I32();
            Mesh.LOD2 = Reader.I32(); Mesh.LOD3 = Reader.I32();
            Mesh.Extent = Reader.Vector();
            Mesh.Size = Reader.Vector();
            if (!ReadMeshSettings(Reader, Mesh.Data) || Name.IsNone()
                || !IsFiniteVector(Mesh.Extent) || !IsFiniteVector(Mesh.Size)
                || OutMetadata.SceneData.MeshMap.Contains(Name)) return false;
            OutMetadata.SceneData.MeshMap.Add(Name, MoveTemp(Mesh));
        }

        const uint32 SettingCount = Reader.Count(MaxMeshesPerModel, 16);
        if (!Reader.IsOk()) return false;
        OutMetadata.SceneData.ModelData.MeshData.Reserve(static_cast<int32>(SettingCount));
        for (uint32 Index = 0; Index < SettingCount; ++Index)
        {
            const FName Name = Reader.Name();
            FMeshData Settings;
            if (!ReadMeshSettings(Reader, Settings) || Name.IsNone()
                || OutMetadata.SceneData.ModelData.MeshData.Contains(Name)) return false;
            OutMetadata.SceneData.ModelData.MeshData.Add(Name, MoveTemp(Settings));
        }
        if (!Reader.IsOk() || !OutMetadata.IsSane())
        {
            return false;
        }
        // bSuccess is transport state, not serialized authoring data. Set it only after the full
        // bounded member has passed structural validation so downstream wrapper users see the same
        // contract as the original build-time scan callback.
        OutMetadata.SceneData.bSuccess = true;
        return true;
    }

    constexpr uint32 DefinitionMagic = 0x31464447u; // "GDF1"
    constexpr uint32 MetadataMagic = 0x31444D47u; // "GMD1"
    constexpr uint32 ConfigMagic = 0x31434647u; // "GFC1"

    void WriteRange(FWriter& Writer, const FGWorldArchiveRange& Range)
    {
        Writer.String(Range.Name, MaxNameBytes);
        Writer.U64(Range.Offset);
        Writer.U64(Range.StoredSize);
        Writer.U64(Range.UncompressedSize);
        Writer.U32(Range.Crc);
        Writer.U8(Range.Codec);
        Writer.U8(0);
        Writer.U16(0);
    }

    FGWorldArchiveRange ReadRange(FReader& Reader)
    {
        FGWorldArchiveRange Range;
        Range.Name = Reader.String(MaxNameBytes);
        Range.Offset = Reader.U64();
        Range.StoredSize = Reader.U64();
        Range.UncompressedSize = Reader.U64();
        Range.Crc = Reader.U32();
        Range.Codec = Reader.U8();
        (void)Reader.U8();
        (void)Reader.U16();
        return Range;
    }

    void WriteSummary(FWriter& Writer, const FGWorldModelSummary& Summary)
    {
        Writer.I32(Summary.SourceNodeCount);
        Writer.I32(Summary.SourceMeshCount);
        Writer.I32(Summary.SourceMaterialCount);
        Writer.I32(Summary.SourceTextureCount);
        Writer.Vector(Summary.Center);
        Writer.Vector(Summary.Size);
    }

    FGWorldModelSummary ReadSummary(FReader& Reader)
    {
        FGWorldModelSummary Summary;
        Summary.SourceNodeCount = Reader.I32();
        Summary.SourceMeshCount = Reader.I32();
        Summary.SourceMaterialCount = Reader.I32();
        Summary.SourceTextureCount = Reader.I32();
        Summary.Center = Reader.Vector();
        Summary.Size = Reader.Vector();
        return Summary;
    }

    bool SerializeDefinitionData(
        const FModelDefinition& Definition,
        const FString& DefinitionJson,
        TArray<uint8>& OutBytes,
        FString& OutError)
    {
        FWriter Writer(OutBytes, FGWorldArchive::MaxDirectoryBytes);
        Writer.U32(DefinitionMagic);
        TArray<FString> BoneNames;
        Definition.Bones.GetKeys(BoneNames);
        BoneNames.Sort();
        Writer.U32(static_cast<uint32>(BoneNames.Num()));
        for (const FString& BoneName : BoneNames)
        {
            Writer.String(BoneName, MaxNameBytes);
            Writer.String(Definition.Bones.FindChecked(BoneName), MaxNameBytes);
        }
        Writer.String(DefinitionJson);
        if (!Writer.IsOk() || OutBytes.IsEmpty()
            || static_cast<int64>(OutBytes.Num()) > FGWorldArchive::MaxDirectoryBytes)
        {
            OutError = TEXT("A model definition member exceeded a bounded field or size limit");
            return false;
        }
        return true;
    }

    bool DeserializeDefinitionData(
        const TArray<uint8>& Bytes,
        TMap<FString, FString>& OutBones,
        FString& OutDefinitionJson,
        FString& OutError)
    {
        FReader Reader(Bytes);
        if (Reader.U32() != DefinitionMagic)
        {
            OutError = TEXT("The .gwd model definition member has an invalid header");
            return false;
        }
        const uint32 BoneCount = Reader.Count(MaxBonesPerModel, 8);
        if (!Reader.IsOk())
        {
            OutError = TEXT("The .gwd model definition bone count is invalid");
            return false;
        }
        TMap<FString, FString> Bones;
        for (uint32 Index = 0; Index < BoneCount; ++Index)
        {
            FString BoneName = Reader.String(MaxNameBytes);
            FString BoneTarget = Reader.String(MaxNameBytes);
            if (!Reader.IsOk() || BoneName.IsEmpty() || BoneTarget.IsEmpty()
                || Bones.Contains(BoneName))
            {
                OutError = FString::Printf(
                    TEXT("Invalid or duplicate bone alias at .gwd definition row %u"), Index);
                return false;
            }
            Bones.Add(MoveTemp(BoneName), MoveTemp(BoneTarget));
        }
        FString Json = Reader.String();
        if (!Reader.IsAtEnd() || Json.IsEmpty())
        {
            OutError = TEXT("The .gwd model definition is truncated, empty, or has trailing bytes");
            return false;
        }
        OutBones = MoveTemp(Bones);
        OutDefinitionJson = MoveTemp(Json);
        return true;
    }

    bool SerializeMetadataData(
        const FGWorldModelMetadata& Metadata,
        const FGWorldModelSummary& Summary,
        TArray<uint8>& OutBytes,
        FString& OutError)
    {
        FWriter Writer(OutBytes, FGWorldArchive::MaxDirectoryBytes);
        Writer.U32(MetadataMagic);
        WriteMetadata(Writer, Metadata, Summary);
        if (!Writer.IsOk() || OutBytes.IsEmpty()
            || static_cast<int64>(OutBytes.Num()) > FGWorldArchive::MaxDirectoryBytes)
        {
            OutError = TEXT("A model metadata member exceeded a bounded field or size limit");
            return false;
        }
        return true;
    }

    bool DeserializeMetadataData(
        const TArray<uint8>& Bytes,
        FGWorldModelMetadata& OutMetadata,
        FString& OutError)
    {
        FReader Reader(Bytes);
        if (Reader.U32() != MetadataMagic
            || !ReadMetadata(Reader, OutMetadata) || !Reader.IsAtEnd())
        {
            OutError = TEXT("The .gwd model metadata member is invalid or truncated");
            return false;
        }
        return true;
    }

    bool SerializeWorldConfig(const FString& Json, TArray<uint8>& OutBytes, FString& OutError)
    {
        if (Json.TrimStartAndEnd().IsEmpty())
        {
            OutError = TEXT("config.json is empty");
            return false;
        }
        FWriter Writer(OutBytes, FGWorldArchive::MaxDirectoryBytes);
        Writer.U32(ConfigMagic);
        Writer.String(Json);
        if (!Writer.IsOk() || OutBytes.IsEmpty())
        {
            OutError = TEXT("config.json exceeded its bounded archive member limit");
            return false;
        }
        return true;
    }

    bool DeserializeWorldConfig(const TArray<uint8>& Bytes, FString& OutJson, FString& OutError)
    {
        FReader Reader(Bytes);
        if (Reader.U32() != ConfigMagic)
        {
            OutError = TEXT("The .gwd config.json member has an invalid header");
            return false;
        }
        OutJson = Reader.String();
        if (!Reader.IsAtEnd() || OutJson.TrimStartAndEnd().IsEmpty())
        {
            OutJson.Reset();
            OutError = TEXT("The .gwd config.json member is invalid or truncated");
            return false;
        }
        return true;
    }

    void WriteDirectoryRecord(FWriter& Writer, const FGWorldModelRecord& Record)
    {
        const FModelDefinition& Definition = Record.Definition;
        Writer.Guid(Definition.UUID);
        Writer.U8(static_cast<uint8>(Definition.ModelType));
        Writer.U8(static_cast<uint8>(Definition.EntityType));
        Writer.U8(static_cast<uint8>(Definition.ItemType));
        Writer.U8(0);
        Writer.String(Definition.Name, MaxNameBytes);
        Writer.String(Definition.DisplayName, MaxNameBytes);
        WriteSummary(Writer, Record.Summary);
        WriteRange(Writer, Record.DefinitionRange);
        WriteRange(Writer, Record.MetadataRange);
        WriteRange(Writer, Record.ManifestRange);
    }

    bool IsValidMemberRange(
        const FGWorldArchiveRange& Range,
        uint64 DirectoryOffset,
        uint64 MaximumSize)
    {
        const bool bValidName = Range.Name.EndsWith(TEXT(".dat"), ESearchCase::CaseSensitive)
            || Range.Name.Equals(TEXT("config.json"), ESearchCase::CaseSensitive);
        return !Range.Name.IsEmpty()
            && bValidName
            && Range.Offset >= static_cast<uint64>(HeaderBytes)
            && Range.Offset % static_cast<uint64>(PayloadAlignment) == 0
            && Range.StoredSize > 0 && Range.StoredSize <= MaximumSize
            && Range.UncompressedSize > 0 && Range.UncompressedSize <= MaximumSize
            && Range.Codec <= 1
            && ((Range.Codec == 0 && Range.StoredSize == Range.UncompressedSize)
                || (Range.Codec == 1 && Range.StoredSize < Range.UncompressedSize))
            && Range.Offset <= DirectoryOffset
            && Range.StoredSize <= DirectoryOffset - Range.Offset;
    }

    bool ReadDirectoryRecord(
        FReader& Reader,
        int64 FileSize,
        uint64 DirectoryOffset,
        FGWorldModelRecord& OutRecord)
    {
        OutRecord = FGWorldModelRecord();
        FModelDefinition& Definition = OutRecord.Definition;
        Definition.UUID = Reader.Guid();
        const uint8 ModelType = Reader.U8();
        const uint8 EntityType = Reader.U8();
        const uint8 ItemType = Reader.U8();
        (void)Reader.U8();
        Definition.Name = Reader.String(MaxNameBytes);
        Definition.DisplayName = Reader.String(MaxNameBytes);
        OutRecord.Summary = ReadSummary(Reader);
        OutRecord.DefinitionRange = ReadRange(Reader);
        OutRecord.MetadataRange = ReadRange(Reader);
        OutRecord.ManifestRange = ReadRange(Reader);

        // Do not trust a directory merely because each enum byte is individually in range. The
        // subtype relationship is part of the model identity: accepting (for example) an Item
        // with a Vehicle subtype would make different runtime call sites interpret the same row
        // differently. Keep this check in the root-directory reader so corrupt rows are rejected
        // before any definition, metadata, or decoded .dat member is allocated.
        const bool bHasEntitySubtype = EntityType != static_cast<uint8>(EModelEntityType::None);
        const bool bHasItemSubtype = ItemType != static_cast<uint8>(EModelItemType::None);
        const bool bSubtypeCombinationIsValid =
            (ModelType == static_cast<uint8>(EModelDefinitionType::Dynamic)
                && !(bHasEntitySubtype && bHasItemSubtype))
            || ((ModelType == static_cast<uint8>(EModelDefinitionType::Static)
                    || ModelType == static_cast<uint8>(EModelDefinitionType::Character))
                && !bHasEntitySubtype && !bHasItemSubtype);

        const FString ExpectedPrefix = FString::Printf(TEXT("models/%s/"),
            *Definition.UUID.ToString(EGuidFormats::Digits));
        const bool bRootNamesAreValid =
            OutRecord.DefinitionRange.Name.Equals(
                ExpectedPrefix + TEXT("definition.dat"), ESearchCase::CaseSensitive)
            && OutRecord.MetadataRange.Name.Equals(
                ExpectedPrefix + TEXT("metadata.dat"), ESearchCase::CaseSensitive)
            && OutRecord.ManifestRange.Name.Equals(
                ExpectedPrefix + TEXT("manifest.dat"), ESearchCase::CaseSensitive);

        if (!Definition.UUID.IsValid() || Definition.Name.IsEmpty() || Definition.DisplayName.IsEmpty()
            || ModelType > static_cast<uint8>(EModelDefinitionType::Character)
            || EntityType > static_cast<uint8>(EModelEntityType::Animal)
            || ItemType > static_cast<uint8>(EModelItemType::Misc)
            || !bSubtypeCombinationIsValid
            || !bRootNamesAreValid
            || !OutRecord.Summary.IsSane()
            || !IsValidMemberRange(OutRecord.DefinitionRange, DirectoryOffset,
                static_cast<uint64>(FGWorldArchive::MaxDirectoryBytes))
            || !IsValidMemberRange(OutRecord.MetadataRange, DirectoryOffset,
                static_cast<uint64>(FGWorldArchive::MaxDirectoryBytes))
            || !IsValidMemberRange(OutRecord.ManifestRange, DirectoryOffset,
                static_cast<uint64>(FGWorldArchive::MaxDirectoryBytes)))
        {
            return false;
        }

        Definition.ModelType = static_cast<EModelDefinitionType>(ModelType);
        Definition.EntityType = static_cast<EModelEntityType>(EntityType);
        Definition.ItemType = static_cast<EModelItemType>(ItemType);
        Definition.GlbPath = FGWorldArchive::MakeModelReference(Definition.UUID);
        Definition.JsonPath = Definition.GlbPath + TEXT("#definition");
        return Reader.IsOk();
    }

    bool SerializeDirectory(const FGWorldArchiveRange& ConfigRange, const TArray<FGWorldModelRecord>& Records, TArray<uint8>& OutBytes, FString& OutError)
    {
        FWriter Writer(OutBytes, FGWorldArchive::MaxDirectoryBytes);
        WriteRange(Writer, ConfigRange);
        Writer.U32(static_cast<uint32>(Records.Num()));
        for (const FGWorldModelRecord& Record : Records) WriteDirectoryRecord(Writer, Record);
        if (!Writer.IsOk() || OutBytes.Num() < 4
            || static_cast<int64>(OutBytes.Num()) > FGWorldArchive::MaxDirectoryBytes)
        {
            OutError = TEXT("The .gwd directory exceeded a bounded field or size limit");
            return false;
        }
        return true;
    }

    bool DeserializeDirectory(
        const TArray<uint8>& Bytes,
        int64 FileSize,
        uint64 DirectoryOffset,
        uint32 ExpectedModels,
        FGWorldArchiveRange& OutConfigRange,
        TMap<FGuid, FGWorldModelRecord>& OutRecords,
        FString& OutError)
    {
        FReader Reader(Bytes);
        OutConfigRange = ReadRange(Reader);
        if (!IsValidMemberRange(OutConfigRange, DirectoryOffset, static_cast<uint64>(FGWorldArchive::MaxDirectoryBytes))
            || !OutConfigRange.Name.Equals(TEXT("config.json"), ESearchCase::CaseSensitive))
        {
            OutError = TEXT("The .gwd config.json directory range is invalid");
            return false;
        }
        // Even an empty-string record has a substantial fixed payload. Validate physical space first.
        const uint32 Count = Reader.Count(FGWorldArchive::MaxModels, 196);
        if (!Reader.IsOk() || Count != ExpectedModels)
        {
            OutError = TEXT("The .gwd model count is invalid or disagrees with its header");
            return false;
        }

        TArray<TPair<uint64, uint64>> Ranges;
        Ranges.Reserve(static_cast<int32>(Count) * 3 + 1);
        Ranges.Emplace(OutConfigRange.Offset, OutConfigRange.StoredSize);
        for (uint32 Index = 0; Index < Count; ++Index)
        {
            FGWorldModelRecord Record;
            if (!ReadDirectoryRecord(Reader, FileSize, DirectoryOffset, Record)
                || OutRecords.Contains(Record.Definition.UUID))
            {
                OutError = FString::Printf(TEXT("Invalid or duplicate .gwd model row at index %u"), Index);
                return false;
            }
            Ranges.Emplace(Record.DefinitionRange.Offset, Record.DefinitionRange.StoredSize);
            Ranges.Emplace(Record.MetadataRange.Offset, Record.MetadataRange.StoredSize);
            Ranges.Emplace(Record.ManifestRange.Offset, Record.ManifestRange.StoredSize);
            OutRecords.Add(Record.Definition.UUID, MoveTemp(Record));
        }
        if (!Reader.IsAtEnd())
        {
            OutError = TEXT("The .gwd directory is truncated or contains trailing bytes");
            return false;
        }

        Ranges.Sort([](const TPair<uint64, uint64>& A, const TPair<uint64, uint64>& B)
        {
            return A.Key < B.Key;
        });
        for (int32 Index = 1; Index < Ranges.Num(); ++Index)
        {
            if (Ranges[Index - 1].Key + Ranges[Index - 1].Value > Ranges[Index].Key)
            {
                OutError = TEXT("The .gwd definition, metadata, or manifest ranges overlap");
                return false;
            }
        }
        return true;
    }

    bool ReadExact(IFileHandle& Handle, uint8* Destination, int64 Count)
    {
        return Count >= 0 && (Count == 0 || (Destination && Handle.Read(Destination, Count)));
    }

    bool WriteExact(IFileHandle& Handle, const uint8* Source, int64 Count)
    {
        return Count >= 0 && (Count == 0 || (Source && Handle.Write(Source, Count)));
    }

    bool ReadChecksummedRange(
        IFileHandle& Handle,
        int64 ExpectedFileSize,
        uint64 DataEndOffset,
        const FGWorldArchiveRange& Range,
        int64 MaximumBytes,
        TArray<uint8>& OutBytes,
        FString& OutError)
    {
        OutBytes.Reset();
        const bool bValidName = Range.Name.EndsWith(TEXT(".dat"), ESearchCase::CaseSensitive)
            || Range.Name.Equals(TEXT("config.json"), ESearchCase::CaseSensitive);
        if (ExpectedFileSize < HeaderBytes || Range.Name.IsEmpty()
            || !bValidName
            || Range.StoredSize == 0 || Range.UncompressedSize == 0 || Range.Codec > 1
            || Range.StoredSize > static_cast<uint64>(MaximumBytes)
            || Range.UncompressedSize > static_cast<uint64>(MaximumBytes)
            || Range.StoredSize > static_cast<uint64>(MAX_int32)
            || Range.UncompressedSize > static_cast<uint64>(MAX_int32)
            || Range.Offset > DataEndOffset
            || Range.StoredSize > DataEndOffset - Range.Offset
            || (Range.Codec == 0 && Range.StoredSize != Range.UncompressedSize)
            || (Range.Codec == 1 && Range.StoredSize >= Range.UncompressedSize))
        {
            OutError = TEXT("The requested .gwd member range is invalid or exceeds its memory limit");
            return false;
        }

        if (Handle.Size() != ExpectedFileSize
            || !Handle.Seek(static_cast<int64>(Range.Offset)))
        {
            OutError = TEXT("The .gwd file changed or could not be opened for a member range read");
            return false;
        }

        TArray<uint8> Stored;
        Stored.SetNumUninitialized(static_cast<int32>(Range.StoredSize));
        if (!ReadExact(Handle, Stored.GetData(), Stored.Num()))
        {
            OutError = TEXT("The requested .gwd member could not be read completely");
            return false;
        }

        if (Range.Codec == 0)
        {
            if (Range.StoredSize != Range.UncompressedSize)
            {
                OutError = TEXT("An uncompressed .dat member has inconsistent lengths");
                return false;
            }
            OutBytes = MoveTemp(Stored);
        }
        else
        {
            OutBytes.SetNumUninitialized(static_cast<int32>(Range.UncompressedSize));
            if (!FCompression::UncompressMemory(
                    NAME_Zlib,
                    OutBytes.GetData(), static_cast<int64>(OutBytes.Num()),
                    Stored.GetData(), static_cast<int64>(Stored.Num()),
                    COMPRESS_NoFlags,
                    0))
            {
                OutBytes.Reset();
                OutError = TEXT("The requested .dat member failed zlib decompression");
                return false;
            }
        }
        if (FCrc::MemCrc32(OutBytes.GetData(), OutBytes.Num()) != Range.Crc)
        {
            OutBytes.Reset();
            OutError = TEXT("The requested .dat member failed its uncompressed CRC check");
            return false;
        }
        return true;
    }

    bool ReadChecksummedRange(
        const FString& Path,
        const int64 ExpectedFileSize,
        const uint64 DataEndOffset,
        const FGWorldArchiveRange& Range,
        const int64 MaximumBytes,
        TArray<uint8>& OutBytes,
        FString& OutError)
    {
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*Path));
        if (!Handle.IsValid())
        {
            OutBytes.Reset();
            OutError = TEXT("The .gwd file could not be opened for a member range read");
            return false;
        }
        return ReadChecksummedRange(
            *Handle, ExpectedFileSize, DataEndOffset, Range, MaximumBytes,
            OutBytes, OutError);
    }

    bool WritePadding(IFileHandle& Handle, int64& InOutOffset, int64 Alignment)
    {
        const int64 AlignedOffset = Align(InOutOffset, Alignment);
        const int64 Padding = AlignedOffset - InOutOffset;
        if (Padding <= 0) return true;
        TArray<uint8> Zeros;
        Zeros.SetNumZeroed(static_cast<int32>(Padding));
        if (!WriteExact(Handle, Zeros.GetData(), Padding)) return false;
        InOutOffset = AlignedOffset;
        return true;
    }

    bool WriteDatMember(
        IFileHandle& Handle,
        int64& InOutOffset,
        const FString& MemberName,
        const TArray<uint8>& RawBytes,
        FGWorldArchiveRange& OutRange,
        FString& OutError,
        bool bPreferFastRead = false)
    {
        const bool bValidName = MemberName.EndsWith(TEXT(".dat"), ESearchCase::CaseSensitive)
            || MemberName.Equals(TEXT("config.json"), ESearchCase::CaseSensitive);
        if (MemberName.IsEmpty() || !bValidName
            || RawBytes.IsEmpty() || RawBytes.Num() > FGWorldArchive::MaxDatMemberBytes
            || !WritePadding(Handle, InOutOffset, PayloadAlignment))
        {
            OutError = FString::Printf(TEXT("Invalid or unalignable archive member: %s"), *MemberName);
            return false;
        }

        TArray<uint8> Compressed;
        // Use the failure-reporting 64-bit compression API. The legacy return-value overload may
        // assert when a bound cannot be represented, which is unacceptable for author-supplied
        // models even though each final TArray member is capped below MAX_int32.
        int64 Bound64 = 0;
        const bool bBoundFitsArray = FCompression::CompressMemoryBound(
                NAME_Zlib,
                Bound64,
                static_cast<int64>(RawBytes.Num()),
                0)
            && Bound64 > 0
            && Bound64 <= FGWorldArchive::MaxDatMemberBytes
            && Bound64 <= MAX_int32;
        const int32 Bound = bBoundFitsArray ? static_cast<int32>(Bound64) : 0;
        int64 CompressedSize = Bound64;
        // Mesh/texture payloads are latency-sensitive range-read members.  Compressing hundreds or
        // thousands of them creates a long zlib tail at the end of a world build and forces the
        // first stream of each member to decompress again.  Metadata remains compressed normally.
        bool bUseCompression = bBoundFitsArray && !bPreferFastRead;
        if (bUseCompression)
        {
            Compressed.SetNumUninitialized(Bound);
            bUseCompression = FCompression::CompressMemory(
                NAME_Zlib,
                Compressed.GetData(), CompressedSize,
                RawBytes.GetData(), static_cast<int64>(RawBytes.Num()),
                COMPRESS_BiasSpeed,
                0)
                && CompressedSize > 0
                && CompressedSize < static_cast<int64>(RawBytes.Num())
                && CompressedSize <= MAX_int32;


            if (bUseCompression)
            {
                Compressed.SetNum(static_cast<int32>(CompressedSize), EAllowShrinking::No);
            }
            else Compressed.Reset();
        }

        const uint8* StoredData = bUseCompression ? Compressed.GetData() : RawBytes.GetData();
        const int32 StoredSize = bUseCompression ? Compressed.Num() : RawBytes.Num();
        if (InOutOffset > MaxArchiveBytes || StoredSize > MaxArchiveBytes - InOutOffset
            || !WriteExact(Handle, StoredData, StoredSize))
        {
            OutError = FString::Printf(TEXT("Could not write archive member: %s"), *MemberName);
            return false;
        }

        OutRange.Name = MemberName;
        OutRange.Offset = static_cast<uint64>(InOutOffset);
        OutRange.StoredSize = static_cast<uint64>(StoredSize);
        OutRange.UncompressedSize = static_cast<uint64>(RawBytes.Num());
        OutRange.Crc = FCrc::MemCrc32(RawBytes.GetData(), RawBytes.Num());
        OutRange.Codec = bUseCompression ? 1 : 0;
        InOutOffset += StoredSize;
        return true;
    }

    TArray<uint8> BuildHeader(
        uint64 DirectoryOffset,
        uint64 DirectorySize,
        uint32 DirectoryCrc,
        uint32 ModelCount,
        const FGuid& BuildId)
    {
        TArray<uint8> Bytes;
        FWriter Writer(Bytes, HeaderBytes);
        Writer.U64(Magic);
        Writer.U32(HeaderBytes);
        Writer.U32(0); // Reserved for alignment/future non-versioned header flags.
        Writer.U64(DirectoryOffset);
        Writer.U64(DirectorySize);
        Writer.U32(DirectoryCrc);
        Writer.U32(ModelCount);
        Writer.Guid(BuildId);
        Writer.U64(0);
        return Bytes;
    }

    bool BuildBlocking(
        const FString& ArchivePathInput,
        const TArray<FGWorldBuildModel>& Models,
        FString& OutArchivePath,
        FString& OutError,
        const TFunction<bool()>& ShouldCancel,
        const FString& WorldConfigJson)
    {
        OutArchivePath.Reset();
        OutError.Reset();
        const FString ArchivePath = FSafeFileIO::NormalizeFilePath(ArchivePathInput);
        const auto Cancelled = [&ShouldCancel]() { return ShouldCancel && ShouldCancel(); };
        if (ArchivePath.IsEmpty() || Models.IsEmpty()
            || Models.Num() > FGWorldArchive::MaxModels || Cancelled())
        {
            OutError = TEXT("Archive path, model count, or cancellation state is invalid");
            return false;
        }

        TArray<const FGWorldBuildModel*> Sorted;
        Sorted.Reserve(Models.Num());
        for (const FGWorldBuildModel& Model : Models) Sorted.Add(&Model);
        Sorted.Sort([](const FGWorldBuildModel& A, const FGWorldBuildModel& B)
        {
            return IsGuidLess(A.Definition.UUID, B.Definition.UUID);
        });

        TSet<FGuid> UUIDs;
        for (const FGWorldBuildModel* Model : Sorted)
        {
            FString Reason;
            if (!Model || !Model->Definition.UUID.IsValid() || UUIDs.Contains(Model->Definition.UUID)
                || Model->Definition.Name.IsEmpty() || Model->Definition.DisplayName.IsEmpty()
                || Model->Definition.GlbPath.IsEmpty() || Model->DefinitionJson.IsEmpty()
                || Model->SourceFileSize <= 0 || !Model->Metadata.IsSane(&Reason)
                || !Model->BakedData.IsSane(&Reason)
                || Model->Metadata.SourceMeshCount != Model->BakedData.Meshes.Num()
                || Model->Metadata.SourceMaterialCount != Model->BakedData.Materials.Num()
                || Model->Metadata.SourceTextureCount != Model->BakedData.Textures.Num())
            {
                OutError = FString::Printf(TEXT("Invalid decoded world-build input. UUID=%s Reason=%s"),
                    Model ? *Model->Definition.UUID.ToString() : TEXT("<null>"),
                    Reason.IsEmpty() ? TEXT("identity/count validation failed") : *Reason);
                return false;
            }

            TSet<int32> CapturedSkinIds;
            for (const FGWorldBakedSkin& Skin : Model->BakedData.Skins)
            {
                CapturedSkinIds.Add(Skin.SkinIndex);
            }
            for (const FGWorldNodeTransform& Node : Model->Metadata.NodeTransforms)
            {
                if (Node.MeshIndex != INDEX_NONE && Node.SkinIndex != INDEX_NONE
                    && !CapturedSkinIds.Contains(Node.SkinIndex))
                {
                    OutError = FString::Printf(
                        TEXT("A skinned node has no decoded skin.dat. UUID=%s Node=%d Skin=%d"),
                        *Model->Definition.UUID.ToString(), Node.NodeIndex, Node.SkinIndex);
                    return false;
                }
            }
            UUIDs.Add(Model->Definition.UUID);
        }

        IFileManager& FileManager = IFileManager::Get();
        if (!FileManager.MakeDirectory(*FPaths::GetPath(ArchivePath), true))
        {
            OutError = TEXT("Could not create the world output directory");
            return false;
        }

        const FString TemporaryPath = ArchivePath + TEXT(".tmp.")
            + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        bool bPublished = false;
        ON_SCOPE_EXIT { if (!bPublished) FileManager.Delete(*TemporaryPath, false, true, true); };
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        TUniquePtr<IFileHandle> Output(PlatformFile.OpenWrite(*TemporaryPath, false, false));
        if (!Output.IsValid())
        {
            OutError = TEXT("Could not create the temporary .gwd file");
            return false;
        }
        TArray<uint8> HeaderPlaceholder; HeaderPlaceholder.SetNumZeroed(HeaderBytes);
        if (!WriteExact(*Output, HeaderPlaceholder.GetData(), HeaderPlaceholder.Num()))
        {
            OutError = TEXT("Could not reserve the .gwd header");
            return false;
        }

        int64 Offset = HeaderBytes;
        const FString ConfigJson = WorldConfigJson.TrimStartAndEnd().IsEmpty()
            ? FString::Printf(TEXT("{\"WorldName\":\"%s\"}"), *FPaths::GetBaseFilename(ArchivePath))
            : WorldConfigJson;
        FGWorldArchiveRange ConfigRange;
        TArray<uint8> ConfigBytes;
        if (!SerializeWorldConfig(ConfigJson, ConfigBytes, OutError)
            || !WriteDatMember(*Output, Offset, TEXT("config.json"), ConfigBytes, ConfigRange, OutError))
        {
            return false;
        }

        TArray<FGWorldModelRecord> Records;
        Records.Reserve(Sorted.Num());
        for (const FGWorldBuildModel* Model : Sorted)
        {
            if (Cancelled()) { OutError = TEXT("The .gwd build was cancelled"); return false; }
            const int64 CurrentSourceSize = FileManager.FileSize(*Model->Definition.GlbPath);
            const FDateTime CurrentSourceTime = FileManager.GetTimeStamp(*Model->Definition.GlbPath);
            if (CurrentSourceSize != Model->SourceFileSize
                || (Model->SourceTimestamp != FDateTime() && CurrentSourceTime != Model->SourceTimestamp))
            {
                OutError = FString::Printf(TEXT("Source GLB changed after decoding: %s"), *Model->Definition.GlbPath);
                return false;
            }

            FGWorldModelRecord& Record = Records.AddDefaulted_GetRef();
            Record.Definition.UUID = Model->Definition.UUID;
            Record.Definition.Name = Model->Definition.Name;
            Record.Definition.DisplayName = Model->Definition.DisplayName;
            Record.Definition.ModelType = Model->Definition.ModelType;
            Record.Definition.EntityType = Model->Definition.EntityType;
            Record.Definition.ItemType = Model->Definition.ItemType;
            Record.Summary.SourceNodeCount = Model->Metadata.SourceNodeCount;
            Record.Summary.SourceMeshCount = Model->Metadata.SourceMeshCount;
            Record.Summary.SourceMaterialCount = Model->Metadata.SourceMaterialCount;
            Record.Summary.SourceTextureCount = Model->Metadata.SourceTextureCount;
            Record.Summary.Center = Model->Metadata.SceneData.ModelData.Center;
            Record.Summary.Size = Model->Metadata.SceneData.ModelData.Size;

            const FString Prefix = FString::Printf(TEXT("models/%s/"),
                *Model->Definition.UUID.ToString(EGuidFormats::Digits));
            TArray<uint8> Bytes;
            if (!SerializeDefinitionData(Model->Definition, Model->DefinitionJson, Bytes, OutError)
                || !WriteDatMember(*Output, Offset, Prefix + TEXT("definition.dat"), Bytes,
                    Record.DefinitionRange, OutError)
                || !SerializeMetadataData(Model->Metadata, Record.Summary, Bytes, OutError)
                || !WriteDatMember(*Output, Offset, Prefix + TEXT("metadata.dat"), Bytes,
                    Record.MetadataRange, OutError))
            {
                return false;
            }

            FGWorldModelManifest Manifest;
            for (const FGWorldBakedMesh& Mesh : Model->BakedData.Meshes)
            {
                FGWorldArchiveRange Range;
                const FString Name = Prefix + FString::Printf(TEXT("meshes/%d.dat"), Mesh.MeshIndex);
                if (!WorldArchiveCodec::SerializeMesh(Mesh, Bytes, OutError)
                    || !WriteDatMember(*Output, Offset, Name, Bytes, Range, OutError, true)) return false;
                Manifest.MeshRanges.Add(Mesh.MeshIndex, MoveTemp(Range));
                Manifest.MeshNames.Add(Mesh.MeshIndex, Mesh.Name);
            }
            for (const FGWorldBakedSkin& Skin : Model->BakedData.Skins)
            {
                FGWorldArchiveRange Range;
                const FString Name = Prefix + FString::Printf(TEXT("skins/%d.dat"), Skin.SkinIndex);
                if (!WorldArchiveCodec::SerializeSkin(Skin, Bytes, OutError)
                    || !WriteDatMember(*Output, Offset, Name, Bytes, Range, OutError, true)) return false;
                Manifest.SkinRanges.Add(Skin.SkinIndex, MoveTemp(Range));
            }
            for (const FGWorldBakedMaterial& Material : Model->BakedData.Materials)
            {
                FGWorldArchiveRange Range;
                const FString Name = Prefix + FString::Printf(TEXT("materials/%d.dat"), Material.MaterialId);
                if (!WorldArchiveCodec::SerializeMaterial(Material, Bytes, OutError)
                    || !WriteDatMember(*Output, Offset, Name, Bytes, Range, OutError, true)) return false;
                Manifest.MaterialRanges.Add(Material.MaterialId, MoveTemp(Range));
            }
            for (const FGWorldBakedTexture& Texture : Model->BakedData.Textures)
            {
                FGWorldArchiveRange Range;
                const FString Name = Prefix + FString::Printf(TEXT("textures/%d.dat"), Texture.TextureId);
                if (!WorldArchiveCodec::SerializeTexture(Texture, Bytes, OutError)
                    || !WriteDatMember(*Output, Offset, Name, Bytes, Range, OutError, true)) return false;
                Manifest.TextureRanges.Add(Texture.TextureId, MoveTemp(Range));
            }
            if (!Manifest.IsSane(Record.Summary, &OutError)
                || !WorldArchiveCodec::SerializeManifest(Manifest, Bytes, OutError)
                || !WriteDatMember(*Output, Offset, Prefix + TEXT("manifest.dat"), Bytes,
                    Record.ManifestRange, OutError)) return false;

            // Catch an editor save that happened while this model's members were being written.
            if (FileManager.FileSize(*Model->Definition.GlbPath) != Model->SourceFileSize
                || (Model->SourceTimestamp != FDateTime()
                    && FileManager.GetTimeStamp(*Model->Definition.GlbPath) != Model->SourceTimestamp))
            {
                OutError = FString::Printf(TEXT("Source GLB changed during archive construction: %s"),
                    *Model->Definition.GlbPath);
                return false;
            }
        }

        if (!WritePadding(*Output, Offset, PayloadAlignment))
        { OutError = TEXT("Could not align the .gwd directory"); return false; }
        const uint64 DirectoryOffset = static_cast<uint64>(Offset);
        TArray<uint8> Directory;
        if (!SerializeDirectory(ConfigRange, Records, Directory, OutError)
            || Offset > MaxArchiveBytes - Directory.Num()
            || !WriteExact(*Output, Directory.GetData(), Directory.Num()))
        { if (OutError.IsEmpty()) OutError = TEXT("Could not write the .gwd directory"); return false; }

        const FGuid BuildId = FGuid::NewGuid();
        const TArray<uint8> Header = BuildHeader(DirectoryOffset, Directory.Num(),
            FCrc::MemCrc32(Directory.GetData(), Directory.Num()), Records.Num(), BuildId);
        if (Header.Num() != HeaderBytes || !Output->Seek(0)
            || !WriteExact(*Output, Header.GetData(), Header.Num()) || !Output->Flush(true))
        { OutError = TEXT("Could not flush the .gwd transaction"); return false; }
        Output.Reset();

        FString VerifyError;
        TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Verification =
            FGWorldArchiveReader::Open(TemporaryPath, VerifyError);
        if (!Verification.IsValid())
        { OutError = FString::Printf(TEXT("Temporary .gwd verification failed: %s"), *VerifyError); return false; }
        const int64 TemporaryFileSize = FileManager.FileSize(*TemporaryPath);
        TUniquePtr<IFileHandle> VerificationHandle(PlatformFile.OpenRead(*TemporaryPath));
        if (!VerificationHandle.IsValid() || VerificationHandle->Size() != TemporaryFileSize)
        {
            OutError = TEXT("Temporary .gwd could not be reopened for payload verification");
            return false;
        }
        {
            FString VerifiedConfig;
            if (!Verification->ReadWorldConfig(VerifiedConfig, VerifyError) || VerifiedConfig != ConfigJson)
            {
                OutError = FString::Printf(TEXT("config.json verification failed: %s"), *VerifyError);
                return false;
            }
        }
        for (const FGWorldModelRecord& Record : Records)
        {
            if (Cancelled())
            {
                OutError = TEXT("The .gwd build was cancelled during verification");
                return false;
            }

            // Root members are payloads too. Open() deliberately validates only the fixed header
            // and compact directory, so publication must explicitly range-read their flushed bytes
            // before claiming that the temporary archive is complete.
            {
                TMap<FString, FString> VerifiedBones;
                FString VerifiedDefinitionJson;
                if (!Verification->ReadModelDefinition(
                        Record.Definition.UUID,
                        VerifiedBones,
                        VerifiedDefinitionJson,
                        VerifyError))
                {
                    OutError = FString::Printf(
                        TEXT("definition.dat verification failed: %s"), *VerifyError);
                    return false;
                }
            }
            {
                FGWorldModelMetadata VerifiedMetadata;
                if (!Verification->ReadModelMetadata(
                        Record.Definition.UUID,
                        VerifiedMetadata,
                        VerifyError))
                {
                    OutError = FString::Printf(
                        TEXT("metadata.dat verification failed: %s"), *VerifyError);
                    return false;
                }
            }

            FGWorldModelManifest Manifest;
            if (!Verification->ReadModelManifest(Record.Definition.UUID, Manifest, VerifyError))
            { OutError = FString::Printf(TEXT("Manifest verification failed: %s"), *VerifyError); return false; }

            // Verify every payload from the bytes actually flushed to disk. Only one member is
            // decompressed at a time, avoiding a second full-model copy during publication.
            TArray<uint8> VerifiedBytes;
            for (const TPair<int32, FGWorldArchiveRange>& Pair : Manifest.MeshRanges)
            {
                if (Cancelled()) { OutError = TEXT("The .gwd build was cancelled during verification"); return false; }
                FGWorldBakedMesh Mesh;
                if (!ReadChecksummedRange(*VerificationHandle, TemporaryFileSize, DirectoryOffset,
                        Pair.Value, FGWorldArchive::MaxDatMemberBytes, VerifiedBytes, VerifyError)
                    || !WorldArchiveCodec::DeserializeMesh(VerifiedBytes, Mesh, VerifyError)
                    || Mesh.MeshIndex != Pair.Key)
                {
                    OutError = FString::Printf(TEXT("mesh.dat verification failed: %s"), *VerifyError);
                    return false;
                }
                VerifiedBytes.Empty();
                for (const FGWorldBakedPrimitive& Primitive : Mesh.Primitives)
                {
                    if (Primitive.MaterialId != INDEX_NONE
                        && !Manifest.MaterialRanges.Contains(Primitive.MaterialId))
                    {
                        OutError = TEXT("mesh.dat verification found a missing material reference");
                        return false;
                    }
                }
            }
            for (const TPair<int32, FGWorldArchiveRange>& Pair : Manifest.MaterialRanges)
            {
                FGWorldBakedMaterial Material;
                if (!ReadChecksummedRange(*VerificationHandle, TemporaryFileSize, DirectoryOffset,
                        Pair.Value, FGWorldArchive::MaxDatMemberBytes, VerifiedBytes, VerifyError)
                    || !WorldArchiveCodec::DeserializeMaterial(VerifiedBytes, Material, VerifyError)
                    || Material.MaterialId != Pair.Key)
                {
                    OutError = FString::Printf(TEXT("material.dat verification failed: %s"), *VerifyError);
                    return false;
                }
                VerifiedBytes.Empty();
                for (const FGWorldBakedTextureParameter& Parameter : Material.Textures)
                {
                    if (Parameter.TextureId != INDEX_NONE
                        && !Manifest.TextureRanges.Contains(Parameter.TextureId))
                    {
                        OutError = TEXT("material.dat verification found a missing texture reference");
                        return false;
                    }
                }
            }
            for (const TPair<int32, FGWorldArchiveRange>& Pair : Manifest.TextureRanges)
            {
                FGWorldBakedTexture Texture;
                if (!ReadChecksummedRange(*VerificationHandle, TemporaryFileSize, DirectoryOffset,
                        Pair.Value, FGWorldArchive::MaxDatMemberBytes, VerifiedBytes, VerifyError)
                    || !WorldArchiveCodec::DeserializeTexture(VerifiedBytes, Texture, VerifyError)
                    || Texture.TextureId != Pair.Key)
                {
                    OutError = FString::Printf(TEXT("texture.dat verification failed: %s"), *VerifyError);
                    return false;
                }
                VerifiedBytes.Empty();
            }
            for (const TPair<int32, FGWorldArchiveRange>& Pair : Manifest.SkinRanges)
            {
                FGWorldBakedSkin Skin;
                if (!ReadChecksummedRange(*VerificationHandle, TemporaryFileSize, DirectoryOffset,
                        Pair.Value, FGWorldArchive::MaxDatMemberBytes, VerifiedBytes, VerifyError)
                    || !WorldArchiveCodec::DeserializeSkin(VerifiedBytes, Skin, VerifyError)
                    || Skin.SkinIndex != Pair.Key)
                {
                    OutError = FString::Printf(TEXT("skin.dat verification failed: %s"), *VerifyError);
                    return false;
                }
                VerifiedBytes.Empty();
            }
        }
        VerificationHandle.Reset();
        Verification.Reset();

        // A model written early in a large world may be edited while later models are being
        // serialized and verified. Recheck the complete source snapshot at the publication
        // boundary so a successful build always describes one stable authoring generation.
        for (const FGWorldBuildModel* Model : Sorted)
        {
            if (Cancelled())
            {
                OutError = TEXT("The .gwd build was cancelled before publication");
                return false;
            }
            if (!Model
                || FileManager.FileSize(*Model->Definition.GlbPath) != Model->SourceFileSize
                || (Model->SourceTimestamp != FDateTime()
                    && FileManager.GetTimeStamp(*Model->Definition.GlbPath)
                        != Model->SourceTimestamp))
            {
                OutError = FString::Printf(
                    TEXT("Source GLB changed before archive publication: %s"),
                    Model ? *Model->Definition.GlbPath : TEXT("<null>"));
                return false;
            }
        }

        if (Cancelled()) { OutError = TEXT("The .gwd build was cancelled before publication"); return false; }
        const bool bHadPrevious = FileManager.FileExists(*ArchivePath);
        const FString BackupPath = ArchivePath + TEXT(".bak.") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        if (bHadPrevious && !FileManager.Move(*BackupPath, *ArchivePath, true, true, false, true))
        {
            OutError = TEXT("Could not preserve the previous .gwd before publication");
            return false;
        }
        if (!FileManager.Move(*ArchivePath, *TemporaryPath, true, true, false, true))
        {
            if (bHadPrevious) FileManager.Move(*ArchivePath, *BackupPath, true, true, false, true);
            OutError = TEXT("Could not publish the verified .gwd file");
            return false;
        }
        bPublished = true;
        TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Published = FGWorldArchiveReader::Open(ArchivePath, VerifyError);
        FString PublishedConfig;
        if (!Published.IsValid() || !Published->ReadWorldConfig(PublishedConfig, VerifyError) || PublishedConfig != ConfigJson)
        {
            FileManager.Delete(*ArchivePath, false, true, true);
            if (bHadPrevious) FileManager.Move(*ArchivePath, *BackupPath, true, true, false, true);
            OutError = FString::Printf(TEXT("Published .gwd verification failed: %s"), *VerifyError);
            return false;
        }
        if (bHadPrevious) FileManager.Delete(*BackupPath, false, true, true);
        OutArchivePath = ArchivePath;
        return true;
    }
}

bool FGWorldModelSummary::IsSane(FString* OutError) const
{
    using namespace GWorldArchivePrivate;
    const auto Fail = [OutError](const TCHAR* Message)
    {
        if (OutError) *OutError = Message;
        return false;
    };

    if (SourceNodeCount < 0 || SourceNodeCount > MaxNodesPerModel
        || SourceMeshCount < 0 || SourceMeshCount > MaxMeshesPerModel
        || SourceMaterialCount < 0 || SourceMaterialCount > MaxMeshesPerModel
        || SourceTextureCount < 0 || SourceTextureCount > MaxMeshesPerModel)
    {
        return Fail(TEXT("source object counts are outside their safety limits"));
    }
    if (!IsFiniteVector(Center) || !IsFiniteVector(Size) || Size.GetMin() < 0.0)
    {
        return Fail(TEXT("model bounds are invalid"));
    }
    return true;
}

bool FGWorldModelMetadata::IsSane(FString* OutError) const
{
    using namespace GWorldArchivePrivate;
    FGWorldModelSummary Summary;
    Summary.SourceNodeCount = SourceNodeCount;
    Summary.SourceMeshCount = SourceMeshCount;
    Summary.SourceMaterialCount = SourceMaterialCount;
    Summary.SourceTextureCount = SourceTextureCount;
    Summary.Center = SceneData.ModelData.Center;
    Summary.Size = SceneData.ModelData.Size;
    if (!Summary.IsSane(OutError)) return false;

    const auto Fail = [OutError](const TCHAR* Message)
    {
        if (OutError) *OutError = Message;
        return false;
    };
    if (NodeTransforms.Num() != SourceNodeCount
        || SceneData.NodeMap.Num() > MaxNodesPerModel || SceneData.WaterNodeMap.Num() > MaxNodesPerModel
        || SceneData.MeshMap.Num() > MaxMeshesPerModel || SceneData.ModelData.MeshData.Num() > MaxMeshesPerModel)
    {
        return Fail(TEXT("model metadata collection counts are invalid"));
    }

    // NodeIndex is the stable identity because glTF names may be empty or duplicated. Validate the
    // complete graph with a compact bit array and reject dangling parent/mesh references before it
    // can be consumed by gameplay code.
    TBitArray<> SeenNodeIndices(false, SourceNodeCount);
    TArray<int32> ParentByNode;
    ParentByNode.Init(INDEX_NONE, SourceNodeCount);
    for (const FGWorldNodeTransform& Node : NodeTransforms)
    {
        if (Node.NodeIndex < 0 || Node.NodeIndex >= SourceNodeCount
            || SeenNodeIndices[Node.NodeIndex]
            || (Node.ParentIndex != INDEX_NONE
                && (Node.ParentIndex < 0 || Node.ParentIndex >= SourceNodeCount
                    || Node.ParentIndex == Node.NodeIndex))
            || (Node.MeshIndex != INDEX_NONE
                && (Node.MeshIndex < 0 || Node.MeshIndex >= SourceMeshCount))
            || Node.SkinIndex < INDEX_NONE
            || !IsFiniteTransform(Node.LocalTransform))
        {
            return Fail(TEXT("the complete glTF node-transform table is invalid"));
        }
        SeenNodeIndices[Node.NodeIndex] = true;
        ParentByNode[Node.NodeIndex] = Node.ParentIndex;
    }

    // Parent bounds alone do not rule out A->B->A. A three-state walk rejects every cycle in O(N)
    // without recursion, so a malicious hierarchy cannot overflow the stack of later consumers.
    TArray<uint8> VisitState;
    VisitState.Init(0, SourceNodeCount); // 0=unseen, 1=current chain, 2=finished.
    for (int32 StartNode = 0; StartNode < SourceNodeCount; ++StartNode)
    {
        int32 NodeIndex = StartNode;
        while (NodeIndex != INDEX_NONE && VisitState[NodeIndex] == 0)
        {
            VisitState[NodeIndex] = 1;
            NodeIndex = ParentByNode[NodeIndex];
        }
        if (NodeIndex != INDEX_NONE && VisitState[NodeIndex] == 1)
        {
            return Fail(TEXT("the complete glTF node-transform table contains a parent cycle"));
        }

        NodeIndex = StartNode;
        while (NodeIndex != INDEX_NONE && VisitState[NodeIndex] == 1)
        {
            VisitState[NodeIndex] = 2;
            NodeIndex = ParentByNode[NodeIndex];
        }
    }
    if (static_cast<int64>(SceneData.NodeMap.Num()) + SceneData.WaterNodeMap.Num() > SourceNodeCount
        || SceneData.MeshMap.Num() > SourceMeshCount)
    {
        return Fail(TEXT("baked metadata refers to more nodes or meshes than the source GLB"));
    }
    for (const TPair<FName, FModelNodeData>& Pair : SceneData.NodeMap)
    {
        const FModelMeshData* Mesh = SceneData.MeshMap.Find(Pair.Value.MeshName);
        if (Pair.Key.IsNone() || Pair.Value.MeshName.IsNone()
            || !IsFiniteTransform(Pair.Value.Transform) || !Mesh)
        {
            return Fail(TEXT("a model node has an invalid name, mesh key or transform"));
        }

        if (Pair.Value.FineChunk.X < 0 || Pair.Value.FineChunk.X >= FinePerCoarse
            || Pair.Value.FineChunk.Y < 0 || Pair.Value.FineChunk.Y >= FinePerCoarse
            || Pair.Value.FineChunk.Z < 0 || Pair.Value.FineChunk.Z >= FinePerCoarse)
        {
            return Fail(TEXT("a model node has an invalid 512 m fine-chunk coordinate"));
        }

        const FBox TransformedMeshBounds =
            FBox(-Mesh->Extent.GetAbs(), Mesh->Extent.GetAbs()).TransformBy(Pair.Value.Transform);
        const FVector EffectiveSize = TransformedMeshBounds.IsValid
            ? TransformedMeshBounds.GetSize().GetAbs()
            : FVector::ZeroVector;
        if (!TransformedMeshBounds.IsValid || !IsFiniteVector(EffectiveSize))
            return Fail(TEXT("a model node has a non-finite transformed mesh size"));

        if (!Pair.Value.bAlwaysLoaded)
        {
            FIntVector ExpectedCoarse = FIntVector::ZeroValue;
            FIntVector ExpectedFine = FIntVector::ZeroValue;
            if (EffectiveSize.GetMax() > CoarseChunkCm
                || !TryComputeSpatialChunk(
                    Pair.Value.Transform.GetLocation(), ExpectedCoarse, ExpectedFine)
                || Pair.Value.CoarseChunk != ExpectedCoarse
                || Pair.Value.FineChunk != ExpectedFine)
            {
                return Fail(TEXT("a streamed model node has inconsistent hierarchical chunk metadata"));
            }
        }
    }
    for (const TPair<FName, FWaterStreamNodeData>& Pair : SceneData.WaterNodeMap)
    {
        if (Pair.Key.IsNone() || !IsFiniteTransform(Pair.Value.Transform)
            || !FMath::IsFinite(Pair.Value.StreamRadius) || Pair.Value.StreamRadius <= 0.0f)
            return Fail(TEXT("a water node has an invalid name, transform or radius"));
    }
    for (const TPair<FName, FModelMeshData>& Pair : SceneData.MeshMap)
    {
        const auto IsValidLod = [this](const int32 Index)
        {
            return Index == INDEX_NONE || (Index >= 0 && Index < SourceMeshCount);
        };
        const bool bHasLod = Pair.Value.LOD0 != INDEX_NONE || Pair.Value.LOD1 != INDEX_NONE
            || Pair.Value.LOD2 != INDEX_NONE || Pair.Value.LOD3 != INDEX_NONE;
        if (Pair.Key.IsNone() || !IsFiniteVector(Pair.Value.Extent) || !IsFiniteVector(Pair.Value.Size)
            || Pair.Value.Extent.GetMin() < 0.0 || Pair.Value.Size.GetMin() < 0.0
            || !bHasLod || !IsValidLod(Pair.Value.LOD0) || !IsValidLod(Pair.Value.LOD1)
            || !IsValidLod(Pair.Value.LOD2) || !IsValidLod(Pair.Value.LOD3)
            || !IsSaneMeshSettings(Pair.Value.Data))
            return Fail(TEXT("a mesh row has an invalid key or size"));
    }
    for (const TPair<FName, FMeshData>& Pair : SceneData.ModelData.MeshData)
    {
        if (Pair.Key.IsNone() || !IsSaneMeshSettings(Pair.Value))
            return Fail(TEXT("a model mesh setting row is invalid"));
    }
    return true;
}

bool FGWorldModelManifest::IsSane(
    const FGWorldModelSummary& Summary,
    FString* OutError) const
{
    const auto Fail = [OutError](const TCHAR* Message)
    {
        if (OutError) *OutError = Message;
        return false;
    };
    if (!Summary.IsSane(OutError)
        || MeshRanges.Num() != Summary.SourceMeshCount
        || MaterialRanges.Num() != Summary.SourceMaterialCount
        || TextureRanges.Num() != Summary.SourceTextureCount
        || MeshNames.Num() != MeshRanges.Num()
        || SkinRanges.Num() > GWorldArchivePrivate::MaxBonesPerModel)
    {
        return Fail(TEXT("manifest collection counts do not match the model summary"));
    }

    TSet<FString> Names;
    const auto ValidateMap = [&Names](
        const TMap<int32, FGWorldArchiveRange>& Map,
        const FString& Prefix)
    {
        for (const TPair<int32, FGWorldArchiveRange>& Pair : Map)
        {
            const FGWorldArchiveRange& Range = Pair.Value;
            if (Pair.Key < 0 || Range.Name.IsEmpty()
                || !Range.Name.StartsWith(Prefix, ESearchCase::CaseSensitive)
                || !Range.Name.EndsWith(TEXT(".dat"), ESearchCase::CaseSensitive)
                || Range.Offset < static_cast<uint64>(GWorldArchivePrivate::HeaderBytes)
                || Range.Offset % static_cast<uint64>(GWorldArchivePrivate::PayloadAlignment) != 0
                || Range.StoredSize == 0
                || Range.UncompressedSize == 0
                || Range.StoredSize > static_cast<uint64>(FGWorldArchive::MaxDatMemberBytes)
                || Range.UncompressedSize > static_cast<uint64>(FGWorldArchive::MaxDatMemberBytes)
                || Range.Codec > 1
                || (Range.Codec == 0 && Range.StoredSize != Range.UncompressedSize)
                || (Range.Codec == 1 && Range.StoredSize >= Range.UncompressedSize)
                || Range.Name.Contains(TEXT("..")) || Range.Name.Contains(TEXT("\\"))
                || Names.Contains(Range.Name))
            {
                return false;
            }
            Names.Add(Range.Name);
        }
        return true;
    };
    if (!ValidateMap(MeshRanges, TEXT("models/"))
        || !ValidateMap(SkinRanges, TEXT("models/"))
        || !ValidateMap(MaterialRanges, TEXT("models/"))
        || !ValidateMap(TextureRanges, TEXT("models/")))
    {
        return Fail(TEXT("manifest contains an invalid or duplicate .dat member range"));
    }
    for (int32 MeshIndex = 0; MeshIndex < Summary.SourceMeshCount; ++MeshIndex)
    {
        const FString* MeshName = MeshNames.Find(MeshIndex);
        if (!MeshRanges.Contains(MeshIndex) || !MeshName || MeshName->IsEmpty())
            return Fail(TEXT("manifest mesh indices are not contiguous"));
    }
    for (int32 MaterialIndex = 0; MaterialIndex < Summary.SourceMaterialCount; ++MaterialIndex)
    {
        if (!MaterialRanges.Contains(MaterialIndex))
            return Fail(TEXT("manifest material indices are not contiguous"));
    }
    for (int32 TextureIndex = 0; TextureIndex < Summary.SourceTextureCount; ++TextureIndex)
    {
        if (!TextureRanges.Contains(TextureIndex))
            return Fail(TEXT("manifest texture indices are not contiguous"));
    }
    if (OutError) OutError->Reset();
    return true;
}

FString FGWorldArchive::MakeArchivePath(const FString& WorldRoot)
{
    const FString Normalized = FSafeFileIO::NormalizeFilePath(WorldRoot);
    if (Normalized.IsEmpty()) return FString();
    if (Normalized.EndsWith(TEXT(".gwd"), ESearchCase::IgnoreCase)) return Normalized;
    const FString WorldName = FPaths::GetCleanFilename(Normalized);
    const FString Parent = FPaths::GetPath(Normalized);
    return WorldName.IsEmpty() ? FString() : FPaths::Combine(Parent, WorldName + TEXT(".gwd"));
}

FString FGWorldArchive::MakeModelReference(const FGuid& UUID)
{
    return UUID.IsValid()
        ? FString::Printf(TEXT("gwd://%s"), *UUID.ToString(EGuidFormats::DigitsWithHyphensLower))
        : FString();
}

bool FGWorldArchive::ParseModelReference(const FString& Reference, FGuid& OutUUID)
{
    OutUUID.Invalidate();
    constexpr TCHAR Prefix[] = TEXT("gwd://");
    constexpr TCHAR LegacyPrefix[] = TEXT("gworld://");
    FString Clean = Reference.TrimStartAndEnd();
    if (Clean.StartsWith(Prefix, ESearchCase::IgnoreCase))
        Clean.RightChopInline(UE_ARRAY_COUNT(Prefix) - 1, EAllowShrinking::No);
    else if (Clean.StartsWith(LegacyPrefix, ESearchCase::IgnoreCase))
        Clean.RightChopInline(UE_ARRAY_COUNT(LegacyPrefix) - 1, EAllowShrinking::No);
    else return false;
    const int32 Fragment = Clean.Find(TEXT("#"));
    if (Fragment != INDEX_NONE) Clean.LeftInline(Fragment, EAllowShrinking::No);
    return FGuid::Parse(Clean, OutUUID) && OutUUID.IsValid();
}

bool FGWorldArchive::BuildBlocking(
    const FString& WorldRoot,
    const TArray<FGWorldBuildModel>& Models,
    FString& OutArchivePath,
    FString& OutError,
    TFunction<bool()> ShouldCancel,
    const FString& WorldConfigJson)
{
    using namespace GWorldArchivePrivate;
    return GWorldArchivePrivate::BuildBlocking(
        MakeArchivePath(WorldRoot), Models, OutArchivePath, OutError, ShouldCancel, WorldConfigJson);
}

bool FGWorldArchive::BuildBlockingToArchivePath(
    const FString& ArchivePath,
    const TArray<FGWorldBuildModel>& Models,
    FString& OutArchivePath,
    FString& OutError,
    TFunction<bool()> ShouldCancel,
    const FString& ArchiveConfigJson)
{
    using namespace GWorldArchivePrivate;
    return GWorldArchivePrivate::BuildBlocking(
        ArchivePath, Models, OutArchivePath, OutError, ShouldCancel, ArchiveConfigJson);
}

TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> FGWorldArchiveReader::Open(
    const FString& ArchivePath,
    FString& OutError)
{
    using namespace GWorldArchivePrivate;
    OutError.Reset();
    const FString NormalizedPath = FSafeFileIO::NormalizeFilePath(ArchivePath);
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*NormalizedPath));
    if (!Handle.IsValid())
    {
        OutError = FString::Printf(TEXT("The archive file is missing or unreadable: %s"), *NormalizedPath);
        return nullptr;
    }
    const int64 TotalSize = Handle->Size();
    if (TotalSize < HeaderBytes || TotalSize > MaxArchiveBytes)
    {
        OutError = TEXT("The archive file is outside its bounded size");
        return nullptr;
    }

    TArray<uint8> HeaderBytesArray;
    HeaderBytesArray.SetNumUninitialized(HeaderBytes);
    if (!ReadExact(*Handle, HeaderBytesArray.GetData(), HeaderBytesArray.Num()))
    {
        OutError = TEXT("The .gwd header could not be read");
        return nullptr;
    }
    FReader Header(HeaderBytesArray);
    const uint64 ReadMagic = Header.U64();
    const uint32 ReadHeaderBytes = Header.U32();
    const uint32 ReservedHeaderFlags = Header.U32();
    const uint64 DirectoryOffset = Header.U64();
    const uint64 DirectorySize = Header.U64();
    const uint32 DirectoryCrc = Header.U32();
    const uint32 ModelCount = Header.U32();
    const FGuid BuildId = Header.Guid();
    (void)Header.U64();

    if (!Header.IsAtEnd() || ReadMagic != Magic || ReadHeaderBytes != HeaderBytes
        || ReservedHeaderFlags != 0 || !BuildId.IsValid()
        || ModelCount == 0 || ModelCount > FGWorldArchive::MaxModels
        || DirectorySize == 0 || DirectorySize > static_cast<uint64>(FGWorldArchive::MaxDirectoryBytes)
        || DirectoryOffset < static_cast<uint64>(HeaderBytes)
        || DirectoryOffset > static_cast<uint64>(TotalSize)
        || DirectorySize > static_cast<uint64>(TotalSize) - DirectoryOffset
        || DirectoryOffset + DirectorySize != static_cast<uint64>(TotalSize)
        || DirectorySize > static_cast<uint64>(MAX_int32))
    {
        OutError = TEXT("The .gwd header or bounded offsets are invalid");
        return nullptr;
    }

    TArray<uint8> Directory;
    Directory.SetNumUninitialized(static_cast<int32>(DirectorySize));
    if (!Handle->Seek(static_cast<int64>(DirectoryOffset))
        || !ReadExact(*Handle, Directory.GetData(), Directory.Num()))
    {
        OutError = TEXT("The .gwd directory range could not be read");
        return nullptr;
    }
    if (FCrc::MemCrc32(Directory.GetData(), Directory.Num()) != DirectoryCrc)
    {
        OutError = TEXT("The .gwd directory CRC check failed");
        return nullptr;
    }

    TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Result =
        MakeShared<FGWorldArchiveReader, ESPMode::ThreadSafe>();
    if (!DeserializeDirectory(Directory, TotalSize, DirectoryOffset, ModelCount,
            Result->WorldConfigRange, Result->Records, OutError))
    {
        return nullptr;
    }
    Result->Path = NormalizedPath;
    Result->BuildId = BuildId;
    Result->FileSize = TotalSize;
    Result->DataEndOffset = DirectoryOffset;
    Result->RootMemberRanges.Reserve(Result->Records.Num() * 3 + 1);
    Result->RootMemberRanges.Emplace(Result->WorldConfigRange.Offset, Result->WorldConfigRange.StoredSize);
    for (const TPair<FGuid, FGWorldModelRecord>& Pair : Result->Records)
    {
        const FGWorldModelRecord& Record = Pair.Value;
        Result->RootMemberRanges.Emplace(
            Record.DefinitionRange.Offset, Record.DefinitionRange.StoredSize);
        Result->RootMemberRanges.Emplace(
            Record.MetadataRange.Offset, Record.MetadataRange.StoredSize);
        Result->RootMemberRanges.Emplace(
            Record.ManifestRange.Offset, Record.ManifestRange.StoredSize);
    }
    Result->RootMemberRanges.Sort(
        [](const TPair<uint64, uint64>& A, const TPair<uint64, uint64>& B)
        {
            return A.Key < B.Key;
        });
    return Result;
}

void FGWorldArchiveReader::GetRecords(TArray<FGWorldModelRecord>& OutRecords) const
{
    Records.GenerateValueArray(OutRecords);
    OutRecords.Sort([](const FGWorldModelRecord& A, const FGWorldModelRecord& B)
    {
        return GWorldArchivePrivate::IsGuidLess(A.Definition.UUID, B.Definition.UUID);
    });
}

bool FGWorldArchiveReader::FindRecord(const FGuid& UUID, FGWorldModelRecord& OutRecord) const
{
    const FGWorldModelRecord* Record = Records.Find(UUID);
    if (!Record) return false;
    OutRecord = *Record;
    return true;
}

bool FGWorldArchiveReader::FindRecordByReference(
    const FString& Reference,
    FGWorldModelRecord& OutRecord) const
{
    FGuid UUID;
    return FGWorldArchive::ParseModelReference(Reference, UUID) && FindRecord(UUID, OutRecord);
}

bool FGWorldArchiveReader::GetSummary(
    const FGuid& UUID,
    FGWorldModelSummary& OutSummary) const
{
    const FGWorldModelRecord* Record = Records.Find(UUID);
    if (!Record) return false;
    OutSummary = Record->Summary;
    return true;
}

bool FGWorldArchiveReader::ReadWorldConfig(FString& OutConfigJson, FString& OutError) const
{
    using namespace GWorldArchivePrivate;
    OutConfigJson.Reset();
    TArray<uint8> Bytes;
    return ReadChecksummedRange(Path, FileSize, DataEndOffset, WorldConfigRange,
            FGWorldArchive::MaxDirectoryBytes, Bytes, OutError)
        && DeserializeWorldConfig(Bytes, OutConfigJson, OutError);
}

bool FGWorldArchiveReader::ReadModelDefinition(
    const FGuid& UUID,
    TMap<FString, FString>& OutBones,
    FString& OutDefinitionJson,
    FString& OutError) const
{
    using namespace GWorldArchivePrivate;
    OutBones.Reset();
    OutDefinitionJson.Reset();
    OutError.Reset();
    const FGWorldModelRecord* Record = Records.Find(UUID);
    if (!Record)
    {
        OutError = FString::Printf(
            TEXT("Model UUID is absent from the .gwd directory: %s"), *UUID.ToString());
        return false;
    }

    TArray<uint8> Bytes;
    return ReadChecksummedRange(
            Path, FileSize, DataEndOffset, Record->DefinitionRange, FGWorldArchive::MaxDirectoryBytes,
            Bytes, OutError)
        && DeserializeDefinitionData(Bytes, OutBones, OutDefinitionJson, OutError);
}

bool FGWorldArchiveReader::ReadModelMetadata(
    const FGuid& UUID,
    FGWorldModelMetadata& OutMetadata,
    FString& OutError) const
{
    using namespace GWorldArchivePrivate;
    OutMetadata = FGWorldModelMetadata();
    OutError.Reset();
    const FGWorldModelRecord* Record = Records.Find(UUID);
    if (!Record)
    {
        OutError = FString::Printf(
            TEXT("Model UUID is absent from the .gwd directory: %s"), *UUID.ToString());
        return false;
    }

    {
        FScopeLock Lock(&ResidentTableCacheLock);
        if (const FGWorldModelMetadata* Cached = ResidentMetadataCache.Find(UUID))
        {
            OutMetadata = *Cached;
            return true;
        }
    }

    TArray<uint8> Bytes;
    if (!ReadChecksummedRange(
            Path, FileSize, DataEndOffset, Record->MetadataRange, FGWorldArchive::MaxDirectoryBytes,
            Bytes, OutError)
        || !DeserializeMetadataData(Bytes, OutMetadata, OutError))
    {
        return false;
    }
    if (OutMetadata.SourceNodeCount != Record->Summary.SourceNodeCount
        || OutMetadata.SourceMeshCount != Record->Summary.SourceMeshCount
        || OutMetadata.SourceMaterialCount != Record->Summary.SourceMaterialCount
        || OutMetadata.SourceTextureCount != Record->Summary.SourceTextureCount
        || OutMetadata.SceneData.ModelData.Center != Record->Summary.Center
        || OutMetadata.SceneData.ModelData.Size != Record->Summary.Size)
    {
        OutMetadata = FGWorldModelMetadata();
        OutError = TEXT("The streamed metadata does not match its .gwd directory summary");
        return false;
    }
    {
        FScopeLock Lock(&ResidentTableCacheLock);
        if (!ResidentMetadataCache.Contains(UUID)
            && ResidentMetadataCache.Num() >= MaxResidentModelTableEntries)
        {
            auto It = ResidentMetadataCache.CreateIterator();
            if (It) It.RemoveCurrent();
        }
        ResidentMetadataCache.Add(UUID, OutMetadata);
    }
    return true;
}

bool FGWorldArchiveReader::ReadModelManifest(
    const FGuid& UUID,
    FGWorldModelManifest& OutManifest,
    FString& OutError) const
{
    using namespace GWorldArchivePrivate;
    OutManifest = FGWorldModelManifest();
    OutError.Reset();
    const FGWorldModelRecord* Record = Records.Find(UUID);
    if (!Record)
    {
        OutError = FString::Printf(TEXT("Model UUID is absent from .gwd: %s"), *UUID.ToString());
        return false;
    }
    {
        FScopeLock Lock(&ResidentTableCacheLock);
        if (const FGWorldModelManifest* Cached = ResidentManifestCache.Find(UUID))
        {
            OutManifest = *Cached;
            return true;
        }
    }
    TArray<uint8> Bytes;
    if (!ReadChecksummedRange(Path, FileSize, DataEndOffset, Record->ManifestRange,
            FGWorldArchive::MaxDirectoryBytes, Bytes, OutError)
        || !WorldArchiveCodec::DeserializeManifest(Bytes, OutManifest, OutError)
        || !OutManifest.IsSane(Record->Summary, &OutError))
    {
        OutManifest = FGWorldModelManifest();
        return false;
    }

    const FString ExpectedPrefix = FString::Printf(TEXT("models/%s/"),
        *UUID.ToString(EGuidFormats::Digits));
    TArray<TPair<uint64, uint64>> Ranges;
    const auto ValidateMap = [this, &ExpectedPrefix, &Ranges](
        const TMap<int32, FGWorldArchiveRange>& Map,
        const TCHAR* Category)
    {
        for (const TPair<int32, FGWorldArchiveRange>& Pair : Map)
        {
            const FGWorldArchiveRange& Range = Pair.Value;
            const FString ExpectedName = ExpectedPrefix + Category
                + FString::Printf(TEXT("%d.dat"), Pair.Key);
            if (!Range.Name.Equals(ExpectedName, ESearchCase::CaseSensitive)
                || Range.Offset > DataEndOffset || Range.StoredSize > DataEndOffset - Range.Offset)
                return false;
            Ranges.Emplace(Range.Offset, Range.StoredSize);
        }
        return true;
    };
    if (!ValidateMap(OutManifest.MeshRanges, TEXT("meshes/"))
        || !ValidateMap(OutManifest.SkinRanges, TEXT("skins/"))
        || !ValidateMap(OutManifest.MaterialRanges, TEXT("materials/"))
        || !ValidateMap(OutManifest.TextureRanges, TEXT("textures/")))
    {
        OutManifest = FGWorldModelManifest();
        OutError = TEXT("A manifest .dat range escapes its model namespace or archive data area");
        return false;
    }
    Ranges.Sort([](const TPair<uint64,uint64>& A,const TPair<uint64,uint64>& B){return A.Key<B.Key;});
    for (int32 Index = 1; Index < Ranges.Num(); ++Index)
    {
        if (Ranges[Index-1].Key + Ranges[Index-1].Value > Ranges[Index].Key)
        {
            OutManifest = FGWorldModelManifest();
            OutError = TEXT("Manifest .dat ranges overlap");
            return false;
        }
    }

    // Child members must also stay disjoint from every model's definition/metadata/manifest rows.
    // Both interval lists are sorted, so one cursor replaces the previous model-count × child-count
    // search and keeps large-world manifest initialization predictable.
    const auto Overlaps = [](const uint64 AOffset, const uint64 ASize,
        const uint64 BOffset, const uint64 BSize)
    {
        return AOffset < BOffset + BSize && BOffset < AOffset + ASize;
    };
    int32 RootIndex = 0;
    for (const TPair<uint64, uint64>& Child : Ranges)
    {
        while (RootMemberRanges.IsValidIndex(RootIndex)
            && RootMemberRanges[RootIndex].Key + RootMemberRanges[RootIndex].Value
                <= Child.Key)
        {
            ++RootIndex;
        }
        if (RootMemberRanges.IsValidIndex(RootIndex)
            && Overlaps(Child.Key, Child.Value,
                RootMemberRanges[RootIndex].Key, RootMemberRanges[RootIndex].Value))
        {
            OutManifest = FGWorldModelManifest();
            OutError = TEXT("A manifest .dat member overlaps an archive root member");
            return false;
        }
    }
    {
        FScopeLock Lock(&ResidentTableCacheLock);
        if (!ResidentManifestCache.Contains(UUID)
            && ResidentManifestCache.Num() >= MaxResidentModelTableEntries)
        {
            auto It = ResidentManifestCache.CreateIterator();
            if (It) It.RemoveCurrent();
        }
        ResidentManifestCache.Add(UUID, OutManifest);
    }
    return true;
}

bool FGWorldArchiveReader::ReadMeshBundle(
    const FGuid& UUID,
    const FGWorldModelManifest& Manifest,
    const TArray<int32>& MeshIndices,
    const int32 SkinIndex,
    const bool bLoadMaterialDependencies,
    FGWorldBakedAssetBundle& OutBundle,
    FString& OutError,
    const TSet<int32>* SkipTextureIds,
    const TSet<int32>* SkipMaterialIds) const
{
    using namespace GWorldArchivePrivate;
    OutBundle.Reset();
    OutError.Reset();
    const FGWorldModelRecord* Record = Records.Find(UUID);
    const bool bManifestCountsMatch = Record
        && Manifest.MeshRanges.Num() == Record->Summary.SourceMeshCount
        && Manifest.MaterialRanges.Num() == Record->Summary.SourceMaterialCount
        && Manifest.TextureRanges.Num() == Record->Summary.SourceTextureCount;
    if (!Record || MeshIndices.IsEmpty() || !bManifestCountsMatch)
    {
        OutError = TEXT("Invalid model, manifest counts, or empty mesh request");
        return false;
    }

    // Runtime bundle reads are pure file/CPU work and are frequently dominated by texture members.
    // Do NOT open one OS file handle per .dat member: large materials can fan out to hundreds of
    // texture ranges and handle churn becomes measurable even on NVMe. Instead partition each stage
    // into a few independent shards. Every shard owns exactly one seek cursor and consumes several
    // members serially, while CRC + binary deserialization still run across shards in parallel.
    // Keeping the shard count modest is important because several baked-model requests may already
    // be running on the engine thread pool at the same time.
    auto ParallelReadShards = [this](const int32 ItemCount, auto&& ReadOne)
    {
        if (ItemCount <= 0)
        {
            return;
        }
        const int32 HardwareWorkers = FMath::Max(1, FPlatformMisc::NumberOfCores() - 1);
        const int32 ShardCount = FMath::Clamp(FMath::Min(ItemCount, HardwareWorkers), 1, 4);
        ParallelFor(ShardCount, [this, ItemCount, ShardCount, &ReadOne](const int32 ShardIndex)
        {
            IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
            TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*Path));
            FString OpenError;
            if (!Handle.IsValid() || Handle->Size() != FileSize)
            {
                OpenError = TEXT("The .gwd file changed or could not be opened for a sharded bundle read");
                Handle.Reset();
            }

            for (int32 Index = ShardIndex; Index < ItemCount; Index += ShardCount)
            {
                ReadOne(Index, Handle.Get(), OpenError);
            }
        });
    };

    auto ReadMemberBytes = [this](IFileHandle* Handle, const FGWorldArchiveRange& Range,
        TArray<uint8>& Bytes, FString& Error) -> bool
    {
        if (!Handle)
        {
            if (Error.IsEmpty())
            {
                Error = TEXT("The .gwd shard file handle is unavailable");
            }
            return false;
        }
        return ReadChecksummedRange(*Handle, FileSize, DataEndOffset, Range,
            FGWorldArchive::MaxDatMemberBytes, Bytes, Error);
    };

    TArray<int32> UniqueMeshes;
    UniqueMeshes.Reserve(MeshIndices.Num());
    for (const int32 MeshIndex : MeshIndices)
    {
        UniqueMeshes.AddUnique(MeshIndex);
    }
    UniqueMeshes.Sort();

    TArray<FGWorldBakedMesh> MeshResults;
    MeshResults.SetNum(UniqueMeshes.Num());
    TArray<FString> MeshErrors;
    MeshErrors.SetNum(UniqueMeshes.Num());
    TArray<uint8> MeshSuccess;
    MeshSuccess.Init(0, UniqueMeshes.Num());

    ParallelReadShards(UniqueMeshes.Num(),
        [this, &Manifest, &UniqueMeshes, &MeshResults, &MeshErrors, &MeshSuccess, &ReadMemberBytes](
            const int32 Index, IFileHandle* Handle, const FString& OpenError)
    {
        const int32 MeshIndex = UniqueMeshes[Index];
        const FGWorldArchiveRange* Range = Manifest.MeshRanges.Find(MeshIndex);
        if (!Range)
        {
            MeshErrors[Index] = TEXT("Requested mesh.dat is missing");
            return;
        }
        TArray<uint8> Bytes;
        FGWorldBakedMesh Mesh;
        FString Error = OpenError;
        if (!ReadMemberBytes(Handle, *Range, Bytes, Error)
            || !WorldArchiveCodec::DeserializeMesh(Bytes, Mesh, Error)
            || Mesh.MeshIndex != MeshIndex)
        {
            MeshErrors[Index] = Error.IsEmpty()
                ? TEXT("Requested mesh.dat has a mismatched id") : MoveTemp(Error);
            return;
        }
        MeshResults[Index] = MoveTemp(Mesh);
        MeshSuccess[Index] = 1;
    });

    TSet<int32> MaterialIds;
    for (int32 Index = 0; Index < MeshResults.Num(); ++Index)
    {
        if (MeshSuccess[Index] == 0)
        {
            OutError = MeshErrors[Index].IsEmpty()
                ? TEXT("Requested mesh.dat could not be read") : MeshErrors[Index];
            OutBundle.Reset();
            return false;
        }
        for (const FGWorldBakedPrimitive& Primitive : MeshResults[Index].Primitives)
        {
            if (Primitive.MaterialId == INDEX_NONE)
            {
                continue;
            }
            if (!Manifest.MaterialRanges.Contains(Primitive.MaterialId))
            {
                OutBundle.Reset();
                OutError = TEXT("Requested mesh.dat refers to a missing material member");
                return false;
            }
            if (bLoadMaterialDependencies
                && (!SkipMaterialIds || !SkipMaterialIds->Contains(Primitive.MaterialId)))
            {
                MaterialIds.Add(Primitive.MaterialId);
            }
        }
    }
    OutBundle.Meshes = MoveTemp(MeshResults);

    TArray<int32> SortedMaterials = MaterialIds.Array();
    SortedMaterials.Sort();
    TArray<FGWorldBakedMaterial> MaterialResults;
    MaterialResults.SetNum(SortedMaterials.Num());
    TArray<FString> MaterialErrors;
    MaterialErrors.SetNum(SortedMaterials.Num());
    TArray<uint8> MaterialSuccess;
    MaterialSuccess.Init(0, SortedMaterials.Num());

    ParallelReadShards(SortedMaterials.Num(),
        [this, &Manifest, &SortedMaterials, &MaterialResults, &MaterialErrors, &MaterialSuccess, &ReadMemberBytes](
            const int32 Index, IFileHandle* Handle, const FString& OpenError)
    {
        const int32 MaterialId = SortedMaterials[Index];
        const FGWorldArchiveRange* Range = Manifest.MaterialRanges.Find(MaterialId);
        if (!Range)
        {
            MaterialErrors[Index] = TEXT("Referenced material.dat is missing");
            return;
        }
        TArray<uint8> Bytes;
        FGWorldBakedMaterial Material;
        FString Error = OpenError;
        if (!ReadMemberBytes(Handle, *Range, Bytes, Error)
            || !WorldArchiveCodec::DeserializeMaterial(Bytes, Material, Error)
            || Material.MaterialId != MaterialId)
        {
            MaterialErrors[Index] = Error.IsEmpty()
                ? TEXT("Referenced material.dat has a mismatched id") : MoveTemp(Error);
            return;
        }
        MaterialResults[Index] = MoveTemp(Material);
        MaterialSuccess[Index] = 1;
    });

    TSet<int32> TextureIds;
    for (int32 Index = 0; Index < MaterialResults.Num(); ++Index)
    {
        if (MaterialSuccess[Index] == 0)
        {
            OutError = MaterialErrors[Index].IsEmpty()
                ? TEXT("Referenced material.dat could not be read") : MaterialErrors[Index];
            OutBundle.Reset();
            return false;
        }
        for (const FGWorldBakedTextureParameter& Parameter : MaterialResults[Index].Textures)
        {
            if (Parameter.TextureId != INDEX_NONE)
            {
                if (!Manifest.TextureRanges.Contains(Parameter.TextureId))
                {
                    OutBundle.Reset();
                    OutError = TEXT("Referenced material.dat refers to a missing texture member");
                    return false;
                }
                TextureIds.Add(Parameter.TextureId);
            }
        }
    }
    OutBundle.Materials = MoveTemp(MaterialResults);

    // A facade-level runtime cache can guarantee that selected materials/textures remain alive.
    // Cached materials were removed before material.dat I/O above, which also removes all of their
    // texture dependencies from this request. For uncached materials, skip any texture.dat member
    // already retained by the texture cache.
    // Dependency ids were still validated against the manifest above before any id is skipped.
    if (SkipTextureIds)
    {
        for (const int32 TextureId : *SkipTextureIds)
        {
            TextureIds.Remove(TextureId);
        }
    }

    TArray<int32> SortedTextures = TextureIds.Array();
    SortedTextures.Sort();
    TArray<FGWorldBakedTexture> TextureResults;
    TextureResults.SetNum(SortedTextures.Num());
    TArray<FString> TextureErrors;
    TextureErrors.SetNum(SortedTextures.Num());
    TArray<uint8> TextureSuccess;
    TextureSuccess.Init(0, SortedTextures.Num());

    ParallelReadShards(SortedTextures.Num(),
        [this, &Manifest, &SortedTextures, &TextureResults, &TextureErrors, &TextureSuccess, &ReadMemberBytes](
            const int32 Index, IFileHandle* Handle, const FString& OpenError)
    {
        const int32 TextureId = SortedTextures[Index];
        const FGWorldArchiveRange* Range = Manifest.TextureRanges.Find(TextureId);
        if (!Range)
        {
            TextureErrors[Index] = TEXT("Referenced texture.dat is missing");
            return;
        }
        TArray<uint8> Bytes;
        FGWorldBakedTexture Texture;
        FString Error = OpenError;
        if (!ReadMemberBytes(Handle, *Range, Bytes, Error)
            || !WorldArchiveCodec::DeserializeTexture(Bytes, Texture, Error)
            || Texture.TextureId != TextureId)
        {
            TextureErrors[Index] = Error.IsEmpty()
                ? TEXT("Referenced texture.dat has a mismatched id") : MoveTemp(Error);
            return;
        }
        TextureResults[Index] = MoveTemp(Texture);
        TextureSuccess[Index] = 1;
    });

    for (int32 Index = 0; Index < TextureResults.Num(); ++Index)
    {
        if (TextureSuccess[Index] == 0)
        {
            OutError = TextureErrors[Index].IsEmpty()
                ? TEXT("Referenced texture.dat could not be read") : TextureErrors[Index];
            OutBundle.Reset();
            return false;
        }
    }
    OutBundle.Textures = MoveTemp(TextureResults);

    if (SkinIndex != INDEX_NONE)
    {
        const FGWorldArchiveRange* Range = Manifest.SkinRanges.Find(SkinIndex);
        if (!Range)
        {
            OutBundle.Reset();
            OutError = TEXT("Requested skin.dat is missing");
            return false;
        }
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*Path));
        if (!Handle.IsValid() || Handle->Size() != FileSize)
        {
            OutBundle.Reset();
            OutError = TEXT("The .gwd file changed or could not be opened for skin.dat");
            return false;
        }
        TArray<uint8> Bytes;
        FGWorldBakedSkin Skin;
        if (!ReadMemberBytes(Handle.Get(), *Range, Bytes, OutError)
            || !WorldArchiveCodec::DeserializeSkin(Bytes, Skin, OutError)
            || Skin.SkinIndex != SkinIndex)
        {
            OutBundle.Reset();
            if (OutError.IsEmpty())
            {
                OutError = TEXT("Requested skin.dat has a mismatched id");
            }
            return false;
        }
        OutBundle.Skins.Add(MoveTemp(Skin));
    }
    return true;
}
