// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldArchiveCodec.cpp
 * 역할: gworld 내부 dat 멤버의 바이너리 코덱입니다.
 * 핵심 기능: little-endian 메시·스킨·머티리얼·텍스처·manifest 변환.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "System/WorldArchiveCodec.h"

namespace WorldArchiveCodecPrivate
{
    constexpr uint32 Version = 1;
    constexpr int32 MaxNameBytes = 4096;
    constexpr int32 MaxStringBytes = 64 * 1024 * 1024;
    constexpr uint32 MaxMeshes = 1000000;
    constexpr uint32 MaxPrimitives = 65536;
    constexpr uint32 MaxVertices = 100000000;
    constexpr uint32 MaxIndices = 300000000;
    constexpr uint32 MaxChannels = 16;
    constexpr uint32 MaxJointSets = 8;
    constexpr uint32 MaxMorphTargets = 4096;
    constexpr uint32 MaxBones = 65536;
    constexpr uint32 MaxParameters = 65536;
    constexpr uint32 MaxMips = 32;
    constexpr uint32 MaxAdditionalTransforms = 65536;
    constexpr uint32 MaxMipBytes = 1024u * 1024u * 1024u;

    class FWriter
    {
    public:
        explicit FWriter(TArray<uint8>& InBytes) : Bytes(InBytes) { Bytes.Reset(); }
        bool Ok() const { return bOk; }
        void U8(const uint8 V)
        {
            if (!CanAppend(1)) return;
            Bytes.Add(V);
        }
        void U16(const uint16 V) { U8(V & 0xff); U8((V >> 8) & 0xff); }
        void U32(const uint32 V)
        {
            U8(V & 0xff); U8((V >> 8) & 0xff); U8((V >> 16) & 0xff); U8((V >> 24) & 0xff);
        }
        void I32(const int32 V) { U32(static_cast<uint32>(V)); }
        void U64(const uint64 V)
        {
            for (int32 Shift = 0; Shift < 64; Shift += 8) U8((V >> Shift) & 0xff);
        }
        void Float(const float V)
        {
            uint32 Bits = 0; FMemory::Memcpy(&Bits, &V, sizeof(Bits)); U32(Bits);
        }
        void Double(const double V)
        {
            uint64 Bits = 0; FMemory::Memcpy(&Bits, &V, sizeof(Bits)); U64(Bits);
        }
        void String(const FString& V, const int32 Limit = MaxStringBytes)
        {
            FTCHARToUTF8 Utf8(*V);
            if (Utf8.Length() < 0 || Utf8.Length() > Limit) { bOk = false; return; }
            U32(static_cast<uint32>(Utf8.Length()));
            if (bOk && Utf8.Length() > 0)
                Raw(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
        }
        void V2(const FVector2f& V) { Float(V.X); Float(V.Y); }
        void V3(const FVector3f& V) { Float(V.X); Float(V.Y); Float(V.Z); }
        void V4(const FVector4f& V) { Float(V.X); Float(V.Y); Float(V.Z); Float(V.W); }
        void Color(const FLinearColor& V) { Float(V.R); Float(V.G); Float(V.B); Float(V.A); }
        void Vector(const FVector& V) { Double(V.X); Double(V.Y); Double(V.Z); }
        void Transform(const FTransform& V)
        {
            const FQuat Q = V.GetRotation();
            Double(Q.X); Double(Q.Y); Double(Q.Z); Double(Q.W);
            Vector(V.GetLocation()); Vector(V.GetScale3D());
        }
        void Raw(const uint8* Data, const int32 Count)
        {
            if (!CanAppend(Count) || (!Data && Count > 0)) { bOk = false; return; }
            if (Count > 0) Bytes.Append(Data, Count);
        }
    private:
        bool CanAppend(const int64 Count)
        {
            if (!bOk || Count < 0
                || Count > FGWorldArchive::MaxDatMemberBytes
                    - static_cast<int64>(Bytes.Num()))
            {
                bOk = false;
                return false;
            }
            return true;
        }

        TArray<uint8>& Bytes;
        bool bOk = true;
    };

    class FReader
    {
    public:
        explicit FReader(const TArray<uint8>& InBytes) : Bytes(InBytes) {}
        bool Ok() const { return bOk; }
        bool End() const { return bOk && Offset == Bytes.Num(); }
        int32 Remaining() const { return bOk ? Bytes.Num() - Offset : 0; }
        uint8 U8() { if (!Need(1)) return 0; return Bytes[Offset++]; }
        uint16 U16()
        {
            const uint16 A = U8();
            const uint16 B = U8();
            return A | (B << 8);
        }
        uint32 U32()
        {
            // A function-argument/declarator evaluation order must never become part of the disk
            // format. Read each byte in an explicit statement on every supported compiler.
            const uint32 A = U8();
            const uint32 B = U8();
            const uint32 C = U8();
            const uint32 D = U8();
            return A | (B << 8) | (C << 16) | (D << 24);
        }
        int32 I32() { return static_cast<int32>(U32()); }
        uint64 U64()
        {
            uint64 V = 0; for (int32 Shift = 0; Shift < 64; Shift += 8) V |= uint64(U8()) << Shift;
            return V;
        }
        float Float()
        {
            const uint32 Bits = U32(); float V = 0; FMemory::Memcpy(&V, &Bits, sizeof(V)); return V;
        }
        double Double()
        {
            const uint64 Bits = U64(); double V = 0; FMemory::Memcpy(&V, &Bits, sizeof(V)); return V;
        }
        FString String(const int32 Limit = MaxStringBytes)
        {
            const uint32 Count = U32();
            if (!bOk || Count > static_cast<uint32>(Limit) || Count > static_cast<uint32>(MAX_int32)
                || !Need(static_cast<int32>(Count))) { bOk = false; return FString(); }
            if (Count == 0) return FString();
            const ANSICHAR* Source = reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset);
            FUTF8ToTCHAR Converted(Source, static_cast<int32>(Count));
            Offset += static_cast<int32>(Count);
            if (Converted.Length() < 0) { bOk = false; return FString(); }
            return FString(Converted.Length(), Converted.Get());
        }
        FVector2f V2()
        {
            const float X = Float();
            const float Y = Float();
            return FVector2f(X, Y);
        }
        FVector3f V3()
        {
            const float X = Float();
            const float Y = Float();
            const float Z = Float();
            return FVector3f(X, Y, Z);
        }
        FVector4f V4()
        {
            const float X = Float();
            const float Y = Float();
            const float Z = Float();
            const float W = Float();
            return FVector4f(X, Y, Z, W);
        }
        FLinearColor Color()
        {
            const float R = Float();
            const float G = Float();
            const float B = Float();
            const float A = Float();
            return FLinearColor(R, G, B, A);
        }
        FVector Vector()
        {
            const double X = Double();
            const double Y = Double();
            const double Z = Double();
            return FVector(X, Y, Z);
        }
        FTransform Transform()
        {
            const double X = Double();
            const double Y = Double();
            const double Z = Double();
            const double W = Double();
            const FVector Location = Vector();
            const FVector Scale = Vector();
            return FTransform(FQuat(X, Y, Z, W), Location, Scale);
        }
        uint32 Count(const uint32 Limit, const int32 MinimumBytes = 0)
        {
            const uint32 Value = U32();
            if (!bOk || Value > Limit || (MinimumBytes > 0
                && uint64(Value) * uint64(MinimumBytes) > uint64(Remaining()))) bOk = false;
            return bOk ? Value : 0;
        }
        bool Raw(uint8* Destination, const int32 Count)
        {
            if (Count < 0 || (Count > 0 && !Destination) || !Need(Count)) return false;
            if (Count > 0) FMemory::Memcpy(Destination, Bytes.GetData() + Offset, Count);
            Offset += Count; return true;
        }
    private:
        bool Need(const int32 Count)
        {
            if (!bOk || Count < 0 || Count > Bytes.Num() - Offset) { bOk = false; return false; }
            return true;
        }
        const TArray<uint8>& Bytes;
        int32 Offset = 0;
        bool bOk = true;
    };

    bool Finite(const FVector2f& V) { return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y); }
    bool Finite(const FVector3f& V) { return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z); }
    bool Finite(const FVector4f& V)
    {
        return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z) && FMath::IsFinite(V.W);
    }
    bool Finite(const FTransform& V) { return !V.ContainsNaN() && V.GetRotation().IsNormalized(); }

    template<typename T, typename WriteOne>
    void WriteArray(FWriter& W, const TArray<T>& Values, WriteOne&& Fn)
    {
        W.U32(static_cast<uint32>(Values.Num()));
        for (const T& Value : Values)
        {
            Fn(Value);
        }
    }

    template<typename T, typename ReadOne>
    bool ReadArray(FReader& R, TArray<T>& Values, const uint32 Limit, const int32 MinBytes, ReadOne&& Fn)
    {
        Values.Reset();
        const uint32 Count = R.Count(Limit, MinBytes);
        if (!R.Ok())
        {
            return false;
        }
        Values.Reserve(static_cast<int32>(Count));
        for (uint32 Index = 0; Index < Count; ++Index)
        {
            Values.Add(Fn());
        }
        return R.Ok();
    }

    void WriteRange(FWriter& W, const FGWorldArchiveRange& R)
    {
        W.String(R.Name, MaxNameBytes); W.U64(R.Offset); W.U64(R.StoredSize);
        W.U64(R.UncompressedSize); W.U32(R.Crc); W.U8(R.Codec); W.U8(0); W.U16(0);
    }

    FGWorldArchiveRange ReadRange(FReader& R)
    {
        FGWorldArchiveRange V; V.Name = R.String(MaxNameBytes); V.Offset = R.U64();
        V.StoredSize = R.U64(); V.UncompressedSize = R.U64(); V.Crc = R.U32();
        V.Codec = R.U8(); (void)R.U8(); (void)R.U16(); return V;
    }

    bool Finish(FWriter& W, TArray<uint8>& Bytes, FString& Error)
    {
        if (!W.Ok() || Bytes.IsEmpty() || Bytes.Num() > FGWorldArchive::MaxDatMemberBytes)
        {
            Error = TEXT(".dat serialization exceeded a field or member-size limit");
            Bytes.Reset();
            return false;
        }
        Error.Reset();
        return true;
    }

    bool Header(FReader& R, const uint32 Magic, FString& Error)
    {
        if (R.U32() != Magic || R.U32() != Version)
        {
            Error = TEXT(".dat member magic/version mismatch");
            return false;
        }
        return true;
    }
}

namespace WorldArchiveCodec
{
    using namespace WorldArchiveCodecPrivate;

    bool SerializeMesh(const FGWorldBakedMesh& V, TArray<uint8>& Out, FString& Error)
    {
        FWriter W(Out); W.U32(0x4853454d); W.U32(Version); W.I32(V.MeshIndex); W.String(V.Name, MaxNameBytes);
        W.U8(V.bHasNormals); W.U8(V.bHasTangents); W.U8(V.bHasUV); W.U8(V.bHasVertexColors);
        WriteArray(W, V.AdditionalTransforms, [&W](const FTransform& T) { W.Transform(T); });
        W.U32(static_cast<uint32>(V.Primitives.Num()));
        for (const FGWorldBakedPrimitive& P : V.Primitives)
        {
            WriteArray(W, P.Positions, [&W](const FVector3f& X) { W.V3(X); });
            WriteArray(W, P.Normals, [&W](const FVector3f& X) { W.V3(X); });
            WriteArray(W, P.Tangents, [&W](const FVector4f& X) { W.V4(X); });
            W.U32(static_cast<uint32>(P.UVs.Num()));
            for (const TArray<FVector2f>& Channel : P.UVs)
                WriteArray(W, Channel, [&W](const FVector2f& X) { W.V2(X); });
            W.U32(static_cast<uint32>(P.Indices.Num()));
            for (const uint32 X : P.Indices) W.U32(X);
            W.U32(static_cast<uint32>(P.Joints.Num()));
            for (const TArray<FGWorldBakedJoint4>& Set : P.Joints)
                WriteArray(W, Set, [&W](const FGWorldBakedJoint4& X)
                { W.U16(X.X); W.U16(X.Y); W.U16(X.Z); W.U16(X.W); });
            W.U32(static_cast<uint32>(P.Weights.Num()));
            for (const TArray<FVector4f>& Set : P.Weights)
                WriteArray(W, Set, [&W](const FVector4f& X) { W.V4(X); });
            WriteArray(W, P.Colors, [&W](const FVector4f& X) { W.V4(X); });
            W.U32(static_cast<uint32>(P.MorphTargets.Num()));
            for (const FGWorldBakedMorphTarget& M : P.MorphTargets)
            {
                W.String(M.Name, MaxNameBytes);
                WriteArray(W, M.Positions, [&W](const FVector3f& X) { W.V3(X); });
                WriteArray(W, M.Normals, [&W](const FVector3f& X) { W.V3(X); });
            }
            TArray<int32> BoneKeys; P.BoneMap.GetKeys(BoneKeys); BoneKeys.Sort(); W.U32(BoneKeys.Num());
            for (const int32 Key : BoneKeys) { W.I32(Key); W.String(P.BoneMap.FindChecked(Key).ToString(), MaxNameBytes); }
            TArray<FString> WeightKeys; P.WeightMaps.GetKeys(WeightKeys); WeightKeys.Sort(); W.U32(WeightKeys.Num());
            for (const FString& Key : WeightKeys)
            {
                W.String(Key, MaxNameBytes); const TArray<float>& Values = P.WeightMaps.FindChecked(Key);
                W.U32(Values.Num()); for (const float X : Values) W.Float(X);
            }
            W.String(P.MaterialName, MaxNameBytes); W.I32(P.MaterialId); W.I32(P.Mode);
            W.U8(P.bHasMaterial); W.U8(P.bHighPrecisionUVs); W.U8(P.bHighPrecisionWeights);
            W.U8(P.bDisableShadows); W.U8(P.bHasIndices); W.U8(0); W.U16(0);
        }
        return Finish(W, Out, Error);
    }

    bool DeserializeMesh(const TArray<uint8>& Bytes, FGWorldBakedMesh& Out, FString& Error)
    {
        Out = FGWorldBakedMesh(); FReader R(Bytes); if (!Header(R, 0x4853454d, Error)) return false;
        Out.MeshIndex = R.I32(); Out.Name = R.String(MaxNameBytes);
        const uint8 HasNormals = R.U8(); const uint8 HasTangents = R.U8();
        const uint8 HasUv = R.U8(); const uint8 HasVertexColors = R.U8();
        if (HasNormals > 1 || HasTangents > 1 || HasUv > 1 || HasVertexColors > 1)
        { Error = TEXT("mesh.dat contains an invalid boolean flag"); return false; }
        Out.bHasNormals = HasNormals != 0; Out.bHasTangents = HasTangents != 0;
        Out.bHasUV = HasUv != 0; Out.bHasVertexColors = HasVertexColors != 0;
        if (!ReadArray(R, Out.AdditionalTransforms, MaxAdditionalTransforms, 80,
                [&R]() { return R.Transform(); })) return false;
        const uint32 PrimitiveCount = R.Count(MaxPrimitives, 4); if (!R.Ok()) return false;
        Out.Primitives.Reserve(PrimitiveCount);
        for (uint32 PrimitiveIndex = 0; PrimitiveIndex < PrimitiveCount; ++PrimitiveIndex)
        {
            FGWorldBakedPrimitive& P = Out.Primitives.AddDefaulted_GetRef();
            if (!ReadArray(R, P.Positions, MaxVertices, 12, [&R]() { return R.V3(); })
                || !ReadArray(R, P.Normals, MaxVertices, 12, [&R]() { return R.V3(); })
                || !ReadArray(R, P.Tangents, MaxVertices, 16, [&R]() { return R.V4(); })) return false;
            const uint32 UvCount = R.Count(MaxChannels, 4); if (!R.Ok()) return false; P.UVs.SetNum(UvCount);
            for (uint32 I = 0; I < UvCount; ++I)
                if (!ReadArray(R, P.UVs[I], MaxVertices, 8, [&R]() { return R.V2(); })) return false;
            if (!ReadArray(R, P.Indices, MaxIndices, 4, [&R]() { return R.U32(); })) return false;
            const uint32 JointSetCount = R.Count(MaxJointSets, 4); if (!R.Ok()) return false; P.Joints.SetNum(JointSetCount);
            for (uint32 I = 0; I < JointSetCount; ++I)
                if (!ReadArray(R, P.Joints[I], MaxVertices, 8, [&R]()
                    { FGWorldBakedJoint4 X; X.X=R.U16(); X.Y=R.U16(); X.Z=R.U16(); X.W=R.U16(); return X; })) return false;
            const uint32 WeightSetCount = R.Count(MaxJointSets, 4); if (!R.Ok()) return false; P.Weights.SetNum(WeightSetCount);
            for (uint32 I = 0; I < WeightSetCount; ++I)
                if (!ReadArray(R, P.Weights[I], MaxVertices, 16, [&R]() { return R.V4(); })) return false;
            if (!ReadArray(R, P.Colors, MaxVertices, 16, [&R]() { return R.V4(); })) return false;
            const uint32 MorphCount = R.Count(MaxMorphTargets, 12); if (!R.Ok()) return false; P.MorphTargets.Reserve(MorphCount);
            for (uint32 I = 0; I < MorphCount; ++I)
            {
                FGWorldBakedMorphTarget& M = P.MorphTargets.AddDefaulted_GetRef(); M.Name = R.String(MaxNameBytes);
                if (!ReadArray(R, M.Positions, MaxVertices, 12, [&R]() { return R.V3(); })
                    || !ReadArray(R, M.Normals, MaxVertices, 12, [&R]() { return R.V3(); })) return false;
            }
            const uint32 BoneCount = R.Count(MaxBones, 8); if (!R.Ok()) return false;
            for (uint32 I = 0; I < BoneCount; ++I)
            { const int32 Key = R.I32(); const FName Name(*R.String(MaxNameBytes)); if (Key < 0 || Name.IsNone() || P.BoneMap.Contains(Key)) return false; P.BoneMap.Add(Key, Name); }
            const uint32 WeightMapCount = R.Count(MaxMorphTargets, 8); if (!R.Ok()) return false;
            for (uint32 I = 0; I < WeightMapCount; ++I)
            {
                const FString Key = R.String(MaxNameBytes); TArray<float> Values;
                if (Key.IsEmpty() || P.WeightMaps.Contains(Key)
                    || !ReadArray(R, Values, MaxVertices, 4, [&R]() { return R.Float(); })) return false;
                P.WeightMaps.Add(Key, MoveTemp(Values));
            }
            P.MaterialName = R.String(MaxNameBytes); P.MaterialId = R.I32(); P.Mode = R.I32();
            const uint8 HasMaterial=R.U8(); const uint8 HighPrecisionUvs=R.U8();
            const uint8 HighPrecisionWeights=R.U8(); const uint8 DisableShadows=R.U8();
            const uint8 HasIndices=R.U8(); (void)R.U8(); (void)R.U16();
            if(HasMaterial>1||HighPrecisionUvs>1||HighPrecisionWeights>1||DisableShadows>1||HasIndices>1)
            {Error=TEXT("mesh.dat contains an invalid primitive boolean flag");Out={};return false;}
            P.bHasMaterial=HasMaterial!=0;P.bHighPrecisionUVs=HighPrecisionUvs!=0;
            P.bHighPrecisionWeights=HighPrecisionWeights!=0;P.bDisableShadows=DisableShadows!=0;
            P.bHasIndices=HasIndices!=0;
        }
        // IsSane also checks full-model contiguous IDs. Temporarily normalize this member's ID and
        // move (never copy) its potentially large arrays into the validation model.
        const int32 SerializedMeshIndex = Out.MeshIndex;
        Out.MeshIndex = 0;
        FGWorldBakedModel Check;
        Check.Meshes.Add(MoveTemp(Out));
        FString SaneError;
        if (!R.End() || !Check.IsSane(&SaneError))
        {
            Error = SaneError.IsEmpty() ? TEXT("mesh.dat is malformed") : SaneError;
            Out = FGWorldBakedMesh();
            return false;
        }
        Out = MoveTemp(Check.Meshes[0]);
        Out.MeshIndex = SerializedMeshIndex;
        Error.Reset();
        return true;
    }

    bool SerializeSkin(const FGWorldBakedSkin& V, TArray<uint8>& Out, FString& Error)
    {
        FWriter W(Out);
        W.U32(0x4e494b53); // "SKIN"
        W.U32(Version);
        W.I32(V.SkinIndex);
        W.U32(V.Bones.Num());
        for (const FGWorldBakedBone& Bone : V.Bones)
        {
            W.String(Bone.Name, MaxNameBytes);
            W.I32(Bone.ParentIndex);
            W.Transform(Bone.Transform);
        }

        // Map iteration order is not deterministic, so every map is serialized by sorted key.
        TArray<int32> Keys;
        V.JointBoneMap.GetKeys(Keys);
        Keys.Sort();
        W.U32(Keys.Num());
        for (const int32 Key : Keys)
        {
            W.I32(Key);
            W.String(V.JointBoneMap.FindChecked(Key).ToString(), MaxNameBytes);
        }
        return Finish(W, Out, Error);
    }

    bool DeserializeSkin(const TArray<uint8>& Bytes, FGWorldBakedSkin& Out, FString& Error)
    {
        Out = FGWorldBakedSkin();
        FReader R(Bytes);
        if (!Header(R, 0x4e494b53, Error))
        {
            return false;
        }
        Out.SkinIndex = R.I32();

        const uint32 BoneCount = R.Count(MaxBones, 88);
        if (!R.Ok())
        {
            Error = TEXT("skin.dat bone count is invalid or truncated");
            return false;
        }
        Out.Bones.Reserve(BoneCount);
        for (uint32 Index = 0; Index < BoneCount; ++Index)
        {
            FGWorldBakedBone& Bone = Out.Bones.AddDefaulted_GetRef();
            Bone.Name = R.String(MaxNameBytes);
            Bone.ParentIndex = R.I32();
            Bone.Transform = R.Transform();
        }

        const uint32 JointCount = R.Count(MaxBones, 8);
        if (!R.Ok())
        {
            Error = TEXT("skin.dat joint map is invalid or truncated");
            return false;
        }
        for (uint32 Index = 0; Index < JointCount; ++Index)
        {
            const int32 Key = R.I32();
            const FName Name(*R.String(MaxNameBytes));
            if (Key < 0 || Name.IsNone() || Out.JointBoneMap.Contains(Key))
            {
                Error = TEXT("skin.dat contains an invalid or duplicate joint mapping");
                Out = FGWorldBakedSkin();
                return false;
            }
            Out.JointBoneMap.Add(Key, Name);
        }

        FGWorldBakedModel Check;
        Check.Skins.Add(MoveTemp(Out));
        FString SaneError;
        if (!R.End() || !Check.IsSane(&SaneError))
        {
            Error = SaneError.IsEmpty() ? TEXT("skin.dat is malformed") : SaneError;
            Out = FGWorldBakedSkin();
            return false;
        }
        Out = MoveTemp(Check.Skins[0]);
        Error.Reset();
        return true;
    }

    bool SerializeMaterial(const FGWorldBakedMaterial& V, TArray<uint8>& Out, FString& Error)
    {
        FWriter W(Out);
        W.U32(0x4c54414d); // "MATL"
        W.U32(Version);
        W.I32(V.MaterialId);
        W.String(V.Name, MaxNameBytes);
        W.String(V.BaseMaterialPath);
        W.U8(V.MaterialType);
        W.U8(V.bUnlit);
        W.U16(0); // Reserved for a future format revision.

        W.U32(V.Scalars.Num());
        for (const FGWorldBakedScalarParameter& Parameter : V.Scalars)
        {
            W.String(Parameter.Name, MaxNameBytes);
            W.Float(Parameter.Value);
        }
        W.U32(V.Vectors.Num());
        for (const FGWorldBakedVectorParameter& Parameter : V.Vectors)
        {
            W.String(Parameter.Name, MaxNameBytes);
            W.Color(Parameter.Value);
        }
        W.U32(V.Textures.Num());
        for (const FGWorldBakedTextureParameter& Parameter : V.Textures)
        {
            W.String(Parameter.Name, MaxNameBytes);
            W.I32(Parameter.TextureId);
            W.String(Parameter.AssetPath);
        }
        return Finish(W, Out, Error);
    }

    bool DeserializeMaterial(const TArray<uint8>& Bytes, FGWorldBakedMaterial& Out, FString& Error)
    {
        Out = FGWorldBakedMaterial();
        FReader R(Bytes);
        if (!Header(R, 0x4c54414d, Error))
        {
            return false;
        }
        Out.MaterialId = R.I32();
        Out.Name = R.String(MaxNameBytes);
        Out.BaseMaterialPath = R.String();
        Out.MaterialType = R.U8();
        const uint8 Unlit = R.U8();
        Out.bUnlit = Unlit != 0;
        (void)R.U16();

        const uint32 ScalarCount = R.Count(MaxParameters, 8);
        if (!R.Ok())
        {
            Error = TEXT("material.dat scalar table is invalid or truncated");
            return false;
        }
        Out.Scalars.Reserve(ScalarCount);
        for (uint32 Index = 0; Index < ScalarCount; ++Index)
        {
            FGWorldBakedScalarParameter& Parameter = Out.Scalars.AddDefaulted_GetRef();
            Parameter.Name = R.String(MaxNameBytes);
            Parameter.Value = R.Float();
        }

        const uint32 VectorCount = R.Count(MaxParameters, 20);
        if (!R.Ok())
        {
            Error = TEXT("material.dat vector table is invalid or truncated");
            return false;
        }
        Out.Vectors.Reserve(VectorCount);
        for (uint32 Index = 0; Index < VectorCount; ++Index)
        {
            FGWorldBakedVectorParameter& Parameter = Out.Vectors.AddDefaulted_GetRef();
            Parameter.Name = R.String(MaxNameBytes);
            Parameter.Value = R.Color();
        }

        const uint32 TextureCount = R.Count(MaxParameters, 12);
        if (!R.Ok())
        {
            Error = TEXT("material.dat texture table is invalid or truncated");
            return false;
        }
        Out.Textures.Reserve(TextureCount);
        for (uint32 Index = 0; Index < TextureCount; ++Index)
        {
            FGWorldBakedTextureParameter& Parameter = Out.Textures.AddDefaulted_GetRef();
            Parameter.Name = R.String(MaxNameBytes);
            Parameter.TextureId = R.I32();
            Parameter.AssetPath = R.String();
        }

        bool bParametersValid = Unlit <= 1;
        for (const FGWorldBakedScalarParameter& Parameter : Out.Scalars)
        {
            bParametersValid = bParametersValid && !Parameter.Name.IsEmpty()
                && FMath::IsFinite(Parameter.Value);
        }
        for (const FGWorldBakedVectorParameter& Parameter : Out.Vectors)
        {
            bParametersValid = bParametersValid && !Parameter.Name.IsEmpty()
                && FMath::IsFinite(Parameter.Value.R) && FMath::IsFinite(Parameter.Value.G)
                && FMath::IsFinite(Parameter.Value.B) && FMath::IsFinite(Parameter.Value.A);
        }
        for (const FGWorldBakedTextureParameter& Parameter : Out.Textures)
        {
            const bool bBaked = Parameter.TextureId != INDEX_NONE;
            const bool bAsset = !Parameter.AssetPath.IsEmpty();
            bParametersValid = bParametersValid && !Parameter.Name.IsEmpty()
                && Parameter.TextureId >= INDEX_NONE && bBaked != bAsset;
        }

        if (!R.End() || Out.MaterialId < 0 || Out.Name.IsEmpty()
            || Out.BaseMaterialPath.IsEmpty() || Out.MaterialType > 5 || !bParametersValid)
        {
            Error = TEXT("material.dat is malformed");
            Out = FGWorldBakedMaterial();
            return false;
        }
        Error.Reset();
        return true;
    }

    bool SerializeTexture(
        const FGWorldBakedTexture& V,
        TArray<uint8>& Out,
        FString& Error)
    {
        FWriter W(Out);
        W.U32(0x52584554); // "TEXR"
        W.U32(Version);
        W.I32(V.TextureId);
        W.String(V.Name, MaxNameBytes);
        W.I32(V.SizeX);
        W.I32(V.SizeY);
        W.I32(V.PixelFormat);
        W.U8(V.AddressX);
        W.U8(V.AddressY);
        W.U8(V.Filter);
        W.U8(V.LODGroup);
        W.U8(V.bSRGB);
        W.U8(0);
        W.U16(0);
        W.U32(V.Mips.Num());
        for (const FGWorldBakedTextureMip& Mip : V.Mips)
        {
            W.I32(Mip.SizeX);
            W.I32(Mip.SizeY);
            W.I32(Mip.SizeZ);
            W.U32(Mip.Bytes.Num());
            W.Raw(Mip.Bytes.GetData(), Mip.Bytes.Num());
        }
        return Finish(W, Out, Error);
    }

    bool DeserializeTexture(
        const TArray<uint8>& Bytes,
        FGWorldBakedTexture& Out,
        FString& Error)
    {
        Out = FGWorldBakedTexture();
        FReader R(Bytes);
        if (!Header(R, 0x52584554, Error))
        {
            return false;
        }
        Out.TextureId = R.I32();
        Out.Name = R.String(MaxNameBytes);
        Out.SizeX = R.I32();
        Out.SizeY = R.I32();
        Out.PixelFormat = R.I32();
        Out.AddressX = R.U8();
        Out.AddressY = R.U8();
        Out.Filter = R.U8();
        Out.LODGroup = R.U8();
        const uint8 Srgb = R.U8();
        Out.bSRGB = Srgb != 0;
        (void)R.U8();
        (void)R.U16();

        const uint32 MipCount = R.Count(MaxMips, 16);
        if (!R.Ok())
        {
            Error = TEXT("texture.dat mip count is invalid or truncated");
            return false;
        }
        Out.Mips.Reserve(MipCount);
        for (uint32 Index = 0; Index < MipCount; ++Index)
        {
            FGWorldBakedTextureMip& Mip = Out.Mips.AddDefaulted_GetRef();
            Mip.SizeX = R.I32();
            Mip.SizeY = R.I32();
            Mip.SizeZ = R.I32();
            // Prove the bytes exist before SetNumUninitialized. A forged length must never turn a
            // short member into a gigabyte-scale allocation attempt.
            const uint32 ByteCount = R.Count(MaxMipBytes, 1);
            if (!R.Ok())
            {
                Error = TEXT("texture.dat mip size is invalid");
                Out = FGWorldBakedTexture();
                return false;
            }
            Mip.Bytes.SetNumUninitialized(static_cast<int32>(ByteCount));
            if (!R.Raw(Mip.Bytes.GetData(), Mip.Bytes.Num()))
            {
                Error = TEXT("texture.dat mip bytes are truncated");
                Out = FGWorldBakedTexture();
                return false;
            }
        }

        bool bMipsValid = !Out.Mips.IsEmpty();
        int32 ExpectedX = Out.SizeX;
        int32 ExpectedY = Out.SizeY;
        for (const FGWorldBakedTextureMip& Mip : Out.Mips)
        {
            bMipsValid = bMipsValid && Mip.SizeX == ExpectedX && Mip.SizeY == ExpectedY
                && Mip.SizeZ == 1 && !Mip.Bytes.IsEmpty();
            ExpectedX = FMath::Max(1, ExpectedX >> 1);
            ExpectedY = FMath::Max(1, ExpectedY >> 1);
        }
        if (!R.End() || Out.TextureId < 0 || Out.Name.IsEmpty()
            || Out.SizeX <= 0 || Out.SizeY <= 0 || Srgb > 1 || !bMipsValid)
        {
            Error = TEXT("texture.dat is malformed");
            Out = FGWorldBakedTexture();
            return false;
        }
        Error.Reset();
        return true;
    }

    bool SerializeManifest(
        const FGWorldModelManifest& V,
        TArray<uint8>& Out,
        FString& Error)
    {
        FWriter W(Out);
        W.U32(0x464e414d); // "MANF"
        W.U32(Version);
        const auto WriteMap = [&W](const TMap<int32, FGWorldArchiveRange>& Map)
        {
            TArray<int32> Keys;
            Map.GetKeys(Keys);
            Keys.Sort();
            W.U32(Keys.Num());
            for (const int32 Key : Keys)
            {
                W.I32(Key);
                WriteRange(W, Map.FindChecked(Key));
            }
        };
        WriteMap(V.MeshRanges);
        WriteMap(V.SkinRanges);
        WriteMap(V.MaterialRanges);
        WriteMap(V.TextureRanges);

        TArray<int32> MeshKeys;
        V.MeshNames.GetKeys(MeshKeys);
        MeshKeys.Sort();
        W.U32(MeshKeys.Num());
        for (const int32 Key : MeshKeys)
        {
            W.I32(Key);
            W.String(V.MeshNames.FindChecked(Key), MaxNameBytes);
        }
        return Finish(W, Out, Error);
    }

    bool DeserializeManifest(
        const TArray<uint8>& Bytes,
        FGWorldModelManifest& Out,
        FString& Error)
    {
        Out = FGWorldModelManifest();
        FReader R(Bytes);
        if (!Header(R, 0x464e414d, Error))
        {
            return false;
        }

        const auto ReadMap = [&R](TMap<int32, FGWorldArchiveRange>& Map)
        {
            // key(4) + the smallest range row(36)
            const uint32 Count = R.Count(MaxMeshes, 40);
            if (!R.Ok())
            {
                return false;
            }
            for (uint32 Index = 0; Index < Count; ++Index)
            {
                const int32 Key = R.I32();
                FGWorldArchiveRange Range = ReadRange(R);
                if (Key < 0 || Map.Contains(Key))
                {
                    return false;
                }
                Map.Add(Key, MoveTemp(Range));
            }
            return R.Ok();
        };

        if (!ReadMap(Out.MeshRanges) || !ReadMap(Out.SkinRanges)
            || !ReadMap(Out.MaterialRanges) || !ReadMap(Out.TextureRanges))
        {
            Error = TEXT("manifest.dat contains an invalid or duplicate range table");
            Out = FGWorldModelManifest();
            return false;
        }

        const uint32 MeshNameCount = R.Count(MaxMeshes, 8);
        if (!R.Ok())
        {
            Error = TEXT("manifest.dat mesh-name table is invalid or truncated");
            Out = FGWorldModelManifest();
            return false;
        }
        for (uint32 Index = 0; Index < MeshNameCount; ++Index)
        {
            const int32 Key = R.I32();
            FString Name = R.String(MaxNameBytes);
            if (Key < 0 || Name.IsEmpty() || Out.MeshNames.Contains(Key))
            {
                Error = TEXT("manifest.dat contains an invalid or duplicate mesh name");
                Out = FGWorldModelManifest();
                return false;
            }
            Out.MeshNames.Add(Key, MoveTemp(Name));
        }
        if (!R.End())
        {
            Error = TEXT("manifest.dat is malformed");
            Out = FGWorldModelManifest();
            return false;
        }
        Error.Reset();
        return true;
    }
}
