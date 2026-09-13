// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldSourceModelBuilder.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "System/WorldSourceModelBuilder.h"

#include "Containers/Ticker.h"
#include "Async/ParallelFor.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "Model/glTFMaterialOverrideUtils.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Setting/GameSettings.h"
#include "Simulator/NodeTokenLibrary.h"
#include "System/FileFunctionLibrary.h"
#include "System/GameManagerSubSystem.h"
#include "System/GlbValidation.h"
#include "System/SafeFileIO.h"
#include "System/StringHelper.h"
#include "System/glTFRuntimeSafety.h"
#include "UObject/UObjectGlobals.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeParser.h"

namespace WorldSourceModelBuilderPrivate
{
    constexpr double FineChunkCm = 512.0 * 100.0;
    constexpr double CoarseChunkCm = 8192.0 * 100.0;
    constexpr int32 FinePerCoarse = 16;

    bool TryFloorChunk(const double Value, const double ChunkSize, int64& OutValue)
    {
        if (!FMath::IsFinite(Value) || !FMath::IsFinite(ChunkSize) || ChunkSize <= 0.0) return false;
        const double FloorValue = FMath::FloorToDouble(Value / ChunkSize);
        if (!FMath::IsFinite(FloorValue)
            || FloorValue < static_cast<double>(MIN_int32)
            || FloorValue > static_cast<double>(MAX_int32))
        {
            return false;
        }
        OutValue = static_cast<int64>(FloorValue);
        return true;
    }

    bool ComputeSpatialChunk(const FVector& Location, FIntVector& OutCoarse, FIntVector& OutFine)
    {
        int64 CoarseX = 0, CoarseY = 0, CoarseZ = 0;
        int64 GlobalFineX = 0, GlobalFineY = 0, GlobalFineZ = 0;
        if (!TryFloorChunk(Location.X, CoarseChunkCm, CoarseX)
            || !TryFloorChunk(Location.Y, CoarseChunkCm, CoarseY)
            || !TryFloorChunk(Location.Z, CoarseChunkCm, CoarseZ)
            || !TryFloorChunk(Location.X, FineChunkCm, GlobalFineX)
            || !TryFloorChunk(Location.Y, FineChunkCm, GlobalFineY)
            || !TryFloorChunk(Location.Z, FineChunkCm, GlobalFineZ))
        {
            OutCoarse = FIntVector::ZeroValue;
            OutFine = FIntVector::ZeroValue;
            return false;
        }

        const int64 FineX = GlobalFineX - CoarseX * FinePerCoarse;
        const int64 FineY = GlobalFineY - CoarseY * FinePerCoarse;
        const int64 FineZ = GlobalFineZ - CoarseZ * FinePerCoarse;
        if (FineX < 0 || FineX >= FinePerCoarse
            || FineY < 0 || FineY >= FinePerCoarse
            || FineZ < 0 || FineZ >= FinePerCoarse)
        {
            OutCoarse = FIntVector::ZeroValue;
            OutFine = FIntVector::ZeroValue;
            return false;
        }

        OutCoarse = FIntVector(static_cast<int32>(CoarseX), static_cast<int32>(CoarseY), static_cast<int32>(CoarseZ));
        OutFine = FIntVector(static_cast<int32>(FineX), static_cast<int32>(FineY), static_cast<int32>(FineZ));
        return true;
    }
    constexpr int32 MaxDefinitionCharacters = 16 * 1024 * 1024;

    bool IsFiniteVector(const FVector& Value)
    {
        return FMath::IsFinite(Value.X)
            && FMath::IsFinite(Value.Y)
            && FMath::IsFinite(Value.Z);
    }

    bool IsFiniteTransform(const FTransform& Value)
    {
        const FQuat Rotation = Value.GetRotation();
        return !Value.ContainsNaN()
            && IsFiniteVector(Value.GetLocation())
            && IsFiniteVector(Value.GetScale3D())
            && FMath::IsFinite(Rotation.X)
            && FMath::IsFinite(Rotation.Y)
            && FMath::IsFinite(Rotation.Z)
            && FMath::IsFinite(Rotation.W)
            && Rotation.IsNormalized();
    }

    bool IsWaterNode(const FString& Name)
    {
        return USimulatorNodeTokenLibrary::HasEffectiveToken(
            Name, FName(TEXT("WATER")));
    }

    bool FindUnsupportedNumericLodSegment(const FString& NodeName, FString& OutSegment)
    {
        OutSegment.Reset();
        int32 Delimiter = INDEX_NONE;
        if (!NodeName.FindChar(TEXT(';'), Delimiter)) return false;

        TArray<FString> Segments;
        NodeName.Mid(Delimiter + 1).ParseIntoArray(Segments, TEXT(";"), false);
        for (const FString& Raw : Segments)
        {
            const FString Token = Raw.TrimStartAndEnd().ToUpper();
            if (!Token.StartsWith(TEXT("LOD"), ESearchCase::CaseSensitive) || Token.Len() <= 3) continue;

            bool bNumeric = true;
            for (int32 Index = 3; Index < Token.Len(); ++Index)
            {
                if (!FChar::IsDigit(Token[Index]))
                {
                    bNumeric = false;
                    break;
                }
            }
            if (!bNumeric) continue;
            if (Token == TEXT("LOD0") || Token == TEXT("LOD1")
                || Token == TEXT("LOD2") || Token == TEXT("LOD3")) continue;

            OutSegment = Token;
            return true;
        }
        return false;
    }

    /** Build-only material policy shared by every mesh decoded from the current source GLB. */
    FglTFRuntimeMaterialsConfig MakeMaterialsConfig(UWorldSourceModelBuilder* Builder)
    {
        FglTFRuntimeMaterialsConfig Config;
        Config.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
        if (UGameManagerSubSystem* Manager =
                UGameManagerSubSystem::GetSubSystem(Builder))
        {
            glTFMaterialOverrideUtils::ApplyOverrides(
                Manager->GetMaterialDefaultReferences(), Config);
        }

        Config.bGeneratesMipMaps = true;
        Config.bLoadMipMaps = true;
        Config.SpecularFactor = 0.0f;
        Config.ImagesConfig.bCompressMips = true;
        // Build-time textures are captured immediately from FTexturePlatformData on the game thread.
        // Keep glTFRuntime texture streaming disabled here. On UE 5.8, bStreaming=true attaches
        // glTFRuntime's custom mip provider before UpdateResource(); the provider can report a
        // different tiled layout from the transient texture and triggers StreamableTextureResource
        // validation. CaptureTexture() locks BulkData directly and no longer relies on the provider.
        Config.ImagesConfig.bStreaming = false;
        const int32 TextureLimit = UGameSettings::ResolveMaxTextureResolution(Builder);
        Config.ImagesConfig.MaxWidth = TextureLimit;
        Config.ImagesConfig.MaxHeight = TextureLimit;
        return Config;
    }

    bool ParseModelSettings(
        const FString& DefinitionJson,
        FModelData& OutData,
        FString& OutError)
    {
        OutData = FModelData();
        if (DefinitionJson.IsEmpty()
            || DefinitionJson.Len() > MaxDefinitionCharacters)
        {
            OutError = TEXT("validated model JSON snapshot is empty or too large");
            return false;
        }

        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader =
            TJsonReaderFactory<>::Create(DefinitionJson);
        if (!FJsonSerializer::Deserialize(Reader, Root)
            || !Root.IsValid()
            || !OutData.Deserialization(Root))
        {
            OutError = TEXT("validated model JSON could not be decoded into model settings");
            return false;
        }
        return true;
    }
}

bool UWorldSourceModelBuilder::Start(
    const FModelDefinition& InDefinition,
    const FString& InDefinitionJson)
{
    check(IsInGameThread());
    if (bRunning || bCompletionSent)
    {
        return false;
    }

    const FString SourcePath = GlbValidation::NormalizePath(InDefinition.GlbPath);
    if (!InDefinition.UUID.IsValid()
        || SourcePath.IsEmpty()
        || InDefinitionJson.IsEmpty())
    {
        Complete(false, TEXT("source definition, GLB path, or JSON snapshot is invalid"));
        return false;
    }

    Definition = InDefinition;
    Definition.GlbPath = SourcePath;
    DefinitionJson = InDefinitionJson;
    SourceFileSize = IFileManager::Get().FileSize(*SourcePath);
    SourceTimestamp = IFileManager::Get().GetTimeStamp(*SourcePath);
    if (SourceFileSize <= 0)
    {
        Complete(false, TEXT("source GLB is missing or empty"));
        return false;
    }

    bRunning = true;
    bCancelled = false;
    bCompletionSent = false;
    ProgressValue = 0.01f;
    FailureReason.Reset();
    const uint64 Generation = ++RequestGeneration;
    BeginParserLoad(Generation);
    return true;
}

void UWorldSourceModelBuilder::BeginParserLoad(const uint64 Generation)
{
    check(IsInGameThread());
    const FString SourcePath = Definition.GlbPath;
    FglTFRuntimeConfig Config;
    Config.bAllowExternalFiles = true;
    TWeakObjectPtr<UWorldSourceModelBuilder> WeakThis(this);

    const bool bQueued = FSafeFileIO::RunTrackedWorker(
        [WeakThis, SourcePath, Generation, Config]()
        {
            FString Error;
            TSharedPtr<FglTFRuntimeParser> Parser;
            // Validate the bounded GLB container, then let glTFRuntime interpret the format and the
            // extensions it actually supports. The former semantic pre-validator rejected valid
            // compressed/extension-based meshes before glTFRuntime was allowed to read them.
            if (!GlbValidation::ValidateFile(SourcePath, Error))
            {
                Error = FString::Printf(
                    TEXT("source GLB preflight failed: %s"), *Error);
            }
            else
            {
                Parser = FglTFRuntimeSafety::CreateParserSafely(SourcePath, Config, &Error);
                if (!Parser.IsValid() && Error.IsEmpty())
                {
                    Error = TEXT("glTFRuntime could not open the validated source GLB");
                }
            }

            FSafeFileIO::DispatchTrackedGameThread(
                [WeakThis, Generation, Parser, Config, Error]()
                {
                    if (UWorldSourceModelBuilder* StrongThis = WeakThis.Get())
                    {
                        StrongThis->CompleteParserLoad(
                            Generation, Parser, Config, Error);
                    }
                });
        });

    if (!bQueued)
    {
        Complete(false, TEXT("source parser worker queue is shutting down"));
    }
}

void UWorldSourceModelBuilder::CompleteParserLoad(
    const uint64 Generation,
    const TSharedPtr<FglTFRuntimeParser>& Parser,
    const FglTFRuntimeConfig& Config,
    const FString& Error)
{
    check(IsInGameThread());
    if (!bRunning || bCancelled || Generation != RequestGeneration)
    {
        return;
    }

    // UObject allocation is illegal while GC is marking. Retain only native parser state in the
    // ticker and retry when the collection boundary has completed.
    if (IsGarbageCollecting())
    {
        TWeakObjectPtr<UWorldSourceModelBuilder> WeakThis(this);
        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
            [WeakThis, Generation, Parser, Config, Error](float)
            {
                if (IsGarbageCollecting())
                {
                    return true;
                }
                if (UWorldSourceModelBuilder* StrongThis = WeakThis.Get())
                {
                    StrongThis->CompleteParserLoad(
                        Generation, Parser, Config, Error);
                }
                return false;
            }));
        return;
    }

    if (!Parser.IsValid())
    {
        Complete(false, Error.IsEmpty()
            ? TEXT("source parser creation failed") : Error);
        return;
    }

    SourceAsset = NewObject<UglTFRuntimeAsset>(this, NAME_None, RF_Transient);
    if (!IsValid(SourceAsset))
    {
        Complete(false, TEXT("could not allocate the build-only glTFRuntime asset"));
        return;
    }
    SourceAsset->RuntimeContextObject = Config.RuntimeContextObject;
    SourceAsset->RuntimeContextString = Config.RuntimeContextString;
    // SetParser also touches native state. Queue behind other parser/mesh operations rather
    // than racing them or treating a busy native gate as a broken GLB.
    TWeakObjectPtr<UWorldSourceModelBuilder> WeakThis(this);
    FglTFRuntimeSafety::EnqueueOperation(this, SourceAsset, TEXT("Attach source parser"),
        [WeakThis, Generation, Parser](uint64 Ticket)
        {
            if (auto* Self = WeakThis.Get(); Self && Self->bRunning
                && !Self->bCancelled && Self->RequestGeneration == Generation)
            {
                if (Self->SourceAsset->SetParser(Parser.ToSharedRef()))
                {
                    Self->ProgressValue = 0.05f;
                    Self->BeginDecodedCapture();
                }
                else Self->Complete(false, TEXT("glTFRuntime rejected the source parser"));
            }
            FglTFRuntimeSafety::CompleteOperation(Ticket);
        },
        [WeakThis](const FString& Reason)
        {
            if (auto* Self = WeakThis.Get(); Self && Self->bRunning && !Self->bCancelled)
                Self->Complete(false, Reason);
        });
}

void UWorldSourceModelBuilder::BeginDecodedCapture()
{
    check(IsInGameThread());
    if (!bRunning || bCancelled || !IsValid(SourceAsset))
    {
        Complete(false, TEXT("source asset disappeared before decoding"));
        return;
    }

    BakedData.Reset();
    MaterialIds.Reset();
    TextureIds.Reset();
    SourceMeshNames.Reset();
    SourceToBakedMeshIndex.Reset();
    NodeToBakedMeshIndex.Reset();
    SourceMeshesToCapture.Reset();
    SourceMeshSizes.Reset();
    SkinMeshIndices.Reset();
    CurrentMeshIndex = 0;

    const int32 MeshCount = SourceAsset->GetNumMeshes();
    SourceMeshNames.SetNum(MeshCount);
    for (int32 MeshIndex = 0; MeshIndex < MeshCount; ++MeshIndex)
    {
        SourceMeshNames[MeshIndex] = FString::Printf(
            TEXT("Mesh_%d"), MeshIndex);
    }

    if (SourceAsset->GetParser().IsValid())
    {
        const TArray<TSharedRef<FJsonObject>> MeshObjects =
            SourceAsset->GetParser()->GetMeshes();
        const int32 NamedMeshCount = FMath::Min(MeshCount, MeshObjects.Num());
        for (int32 MeshIndex = 0; MeshIndex < NamedMeshCount; ++MeshIndex)
        {
            FString Name;
            if (MeshObjects[MeshIndex]->TryGetStringField(TEXT("name"), Name)
                && !Name.TrimStartAndEnd().IsEmpty())
            {
                SourceMeshNames[MeshIndex] = Name.TrimStartAndEnd();
            }
        }
    }

    const TArray<FglTFRuntimeNode>& Nodes = SourceAsset->GetNodes();
    for (const FglTFRuntimeNode& Node : Nodes)
    {
        FString UnsupportedLod;
        if (WorldSourceModelBuilderPrivate::FindUnsupportedNumericLodSegment(Node.Name, UnsupportedLod))
        {
            Complete(false, FString::Printf(
                TEXT("node '%s' uses unsupported %s; only LOD0 through LOD3 are supported"),
                *Node.Name, *UnsupportedLod));
            return;
        }
    }

    TMap<FString, int32> CanonicalSourceByKey;
    TMap<FString, int32> CanonicalScoreByKey;
    TArray<bool> SourceHasNonInstanceUse;
    SourceHasNonInstanceUse.Init(false, MeshCount);

    auto MakeCanonicalKey = [](const FglTFRuntimeNode& Node, int32& OutLod, bool& bOutInstance)
    {
        const FSimulatorParsedNodeName Parsed = USimulatorNodeTokenLibrary::ParseNodeName(Node.Name);
        bOutInstance = Parsed.HasEffectiveToken(FName(TEXT("INST")));
        OutLod = 0;
        if (Parsed.HasEffectiveToken(FName(TEXT("LOD1")))) OutLod = 1;
        else if (Parsed.HasEffectiveToken(FName(TEXT("LOD2")))) OutLod = 2;
        else if (Parsed.HasEffectiveToken(FName(TEXT("LOD3")))) OutLod = 3;
        const FString Base = Parsed.BaseName.TrimStartAndEnd().ToLower();
        return Base.IsEmpty() ? FString() : FString::Printf(TEXT("%s#%d"), *Base, OutLod);
    };

    // Pass 1: choose a deterministic non-;INST canonical payload for each BaseName + LOD.
    // Explicit ;LOD0 wins over a bare node, otherwise the lowest source mesh index wins.
    for (const FglTFRuntimeNode& Node : Nodes)
    {
        if (Node.MeshIndex < 0 || Node.MeshIndex >= MeshCount) continue;
        int32 Lod = 0; bool bInstance = false;
        const FString Key = MakeCanonicalKey(Node, Lod, bInstance);
        if (!bInstance) SourceHasNonInstanceUse[Node.MeshIndex] = true;
        if (bInstance || Key.IsEmpty()) continue;
        const bool bExplicitLod0 = USimulatorNodeTokenLibrary::HasEffectiveToken(Node.Name, FName(TEXT("LOD0")));
        const int32 Score = bExplicitLod0 ? 2 : 1;
        const int32* ExistingScore = CanonicalScoreByKey.Find(Key);
        const int32* ExistingSource = CanonicalSourceByKey.Find(Key);
        if (!ExistingScore || Score > *ExistingScore
            || (Score == *ExistingScore && ExistingSource && Node.MeshIndex < *ExistingSource))
        {
            CanonicalScoreByKey.Add(Key, Score);
            CanonicalSourceByKey.Add(Key, Node.MeshIndex);
        }
    }

    // Source payloads are preserved unless they are used only by ;INST rows and every such row can
    // resolve to a different canonical payload. This avoids storing duplicate instance geometry.
    SourceMeshesToCapture.Reserve(MeshCount);
    for (int32 MeshIndex = 0; MeshIndex < MeshCount; ++MeshIndex) SourceMeshesToCapture.Add(MeshIndex);
    TArray<bool> InstanceOnlyCanAlias;
    TArray<bool> SourceSeenByNode;
    InstanceOnlyCanAlias.Init(true, MeshCount);
    SourceSeenByNode.Init(false, MeshCount);
    for (const FglTFRuntimeNode& Node : Nodes)
    {
        if (Node.MeshIndex < 0 || Node.MeshIndex >= MeshCount) continue;
        SourceSeenByNode[Node.MeshIndex] = true;
        int32 Lod = 0; bool bInstance = false;
        const FString Key = MakeCanonicalKey(Node, Lod, bInstance);
        if (!bInstance)
        {
            InstanceOnlyCanAlias[Node.MeshIndex] = false;
            continue;
        }
        const int32* Canonical = CanonicalSourceByKey.Find(Key);
        if (!Canonical || *Canonical == Node.MeshIndex) InstanceOnlyCanAlias[Node.MeshIndex] = false;
    }
    for (int32 MeshIndex = 0; MeshIndex < MeshCount; ++MeshIndex)
    {
        if (SourceSeenByNode[MeshIndex] && !SourceHasNonInstanceUse[MeshIndex]
            && InstanceOnlyCanAlias[MeshIndex])
        {
            SourceMeshesToCapture.Remove(MeshIndex);
        }
    }

    TArray<int32> CapturedSources = SourceMeshesToCapture.Array();
    CapturedSources.Sort();
    SourceToBakedMeshIndex.Init(INDEX_NONE, MeshCount);
    for (int32 BakedIndex = 0; BakedIndex < CapturedSources.Num(); ++BakedIndex)
    {
        SourceToBakedMeshIndex[CapturedSources[BakedIndex]] = BakedIndex;
    }
    for (const FglTFRuntimeNode& Node : Nodes)
    {
        if (Node.MeshIndex < 0 || Node.MeshIndex >= MeshCount) continue;
        int32 Lod = 0; bool bInstance = false;
        const FString Key = MakeCanonicalKey(Node, Lod, bInstance);
        int32 SourceForNode = Node.MeshIndex;
        if (bInstance)
        {
            if (const int32* Canonical = CanonicalSourceByKey.Find(Key)) SourceForNode = *Canonical;
        }
        if (!SourceToBakedMeshIndex.IsValidIndex(SourceForNode)
            || SourceToBakedMeshIndex[SourceForNode] == INDEX_NONE)
        {
            // An unresolved instance source is deliberately retained, so this is a structural error.
            Complete(false, FString::Printf(TEXT("node '%s' could not resolve a baked mesh payload"), *Node.Name));
            return;
        }
        const int32 BakedIndex = SourceToBakedMeshIndex[SourceForNode];
        NodeToBakedMeshIndex.Add(Node.Index, BakedIndex);
        if (Node.SkinIndex >= 0) SkinMeshIndices.FindOrAdd(Node.SkinIndex).Add(BakedIndex);
    }

    UE_LOG(LogTemp, Display,
        TEXT("World build mesh canonicalization: source=%d baked=%d omitted-inst=%d"),
        MeshCount, CapturedSources.Num(), MeshCount - CapturedSources.Num());
    ScheduleNextMesh();
}

bool UWorldSourceModelBuilder::CaptureMeshBounds(
    const int32 MeshIndex,
    const FglTFRuntimeMeshLOD& RuntimeLOD,
    FString& OutError)
{
    using namespace WorldSourceModelBuilderPrivate;
    FBox RawBounds(ForceInit);
    for (const FglTFRuntimePrimitive& Primitive : RuntimeLOD.Primitives)
    {
        // Large position buffers are immutable during this call. Limit temporary storage and
        // worker fan-out to four lanes; reduce deterministically after the workers join.
        const int32 LaneCount = Primitive.Positions.Num() >= 65536 ? 4 : 1;
        FBox Bounds[4] = { FBox(ForceInit), FBox(ForceInit), FBox(ForceInit), FBox(ForceInit) };
        uint8 Valid[4] = { 1, 1, 1, 1 };
        ParallelFor(LaneCount, [&Primitive, &Bounds, &Valid, LaneCount](int32 Lane)
        {
            const int32 Begin = static_cast<int32>(static_cast<int64>(Primitive.Positions.Num()) * Lane / LaneCount);
            const int32 End = static_cast<int32>(static_cast<int64>(Primitive.Positions.Num()) * (Lane + 1) / LaneCount);
            for (int32 Index = Begin; Index < End; ++Index)
            {
                const FVector& Position = Primitive.Positions[Index];
                if (!IsFiniteVector(Position)) { Valid[Lane] = 0; return; }
                Bounds[Lane] += Position;
            }
        });
        for (int32 Lane = 0; Lane < LaneCount; ++Lane)
        {
            if (!Valid[Lane])
            {
                OutError = TEXT("decoded mesh contains a non-finite position");
                return false;
            }
            if (Bounds[Lane].IsValid) RawBounds += Bounds[Lane];
        }
    }
    if (!RawBounds.IsValid)
    {
        OutError = TEXT("decoded mesh contains no bounded vertices");
        return false;
    }

    FBox FinalBounds = RawBounds;
    if (!RuntimeLOD.AdditionalTransforms.IsEmpty())
    {
        FinalBounds = FBox(ForceInit);
        for (const FTransform& Transform : RuntimeLOD.AdditionalTransforms)
        {
            if (!IsFiniteTransform(Transform))
            {
                OutError = TEXT("decoded mesh contains an invalid additional transform");
                return false;
            }
            FinalBounds += RawBounds.TransformBy(Transform);
        }
    }
    if (!FinalBounds.IsValid || !IsFiniteVector(FinalBounds.GetSize()))
    {
        OutError = TEXT("decoded mesh bounds are invalid");
        return false;
    }
    SourceMeshSizes.Add(MeshIndex, FinalBounds.GetSize().GetAbs());
    return true;
}

void UWorldSourceModelBuilder::CaptureNextMesh()
{
    check(IsInGameThread());
    if (!bRunning || bCancelled || !IsValid(SourceAsset)) return;
    const uint64 Generation = RequestGeneration;
    TWeakObjectPtr<UWorldSourceModelBuilder> WeakThis(this);
    FglTFRuntimeSafety::EnqueueOperation(this, SourceAsset, TEXT("Capture source mesh/metadata"),
        [WeakThis, Generation](uint64 Ticket)
        {
            if (auto* Self = WeakThis.Get(); Self && Self->bRunning
                && !Self->bCancelled && Self->RequestGeneration == Generation)
                Self->CaptureNextMeshUnderGate();
            // Every plugin call has returned. Pending cache release is now safe.
            FglTFRuntimeSafety::CompleteOperation(Ticket);
        },
        [WeakThis](const FString& Reason)
        {
            if (auto* Self = WeakThis.Get(); Self && Self->bRunning && !Self->bCancelled)
                Self->Complete(false, Reason);
        });
}

void UWorldSourceModelBuilder::CaptureNextMeshUnderGate()
{
    check(IsInGameThread());
    if (!bRunning || bCancelled || !IsValid(SourceAsset))
    {
        Complete(false, TEXT("source model build was cancelled during mesh decoding"));
        return;
    }

    const int32 MeshCount = SourceAsset->GetNumMeshes();
    if (CurrentMeshIndex >= MeshCount)
    {
        CaptureSkinsAndMetadata();
        return;
    }

    while (CurrentMeshIndex < MeshCount && !SourceMeshesToCapture.Contains(CurrentMeshIndex))
    {
        ++CurrentMeshIndex;
    }
    if (CurrentMeshIndex >= MeshCount)
    {
        CaptureSkinsAndMetadata();
        return;
    }

    const int32 MeshIndex = CurrentMeshIndex;
    const int32 BakedMeshIndex = SourceToBakedMeshIndex.IsValidIndex(MeshIndex)
        ? SourceToBakedMeshIndex[MeshIndex] : INDEX_NONE;
    if (BakedMeshIndex == INDEX_NONE)
    {
        Complete(false, TEXT("internal source-to-baked mesh map is invalid"));
        return;
    }
    FglTFRuntimeMeshLOD RuntimeLOD;
    FglTFRuntimeMaterialsConfig MaterialsConfig =
        WorldSourceModelBuilderPrivate::MakeMaterialsConfig(this);
    // The queue owns the native gate and GC references for the entire capture.
    // Build-only glTFRuntime texture streaming stays disabled; CaptureTexture() copies mip BulkData
    // immediately after this synchronous decode returns.
    const bool bDecoded = SourceAsset->LoadMeshAsRuntimeLOD(
        MeshIndex, RuntimeLOD, MaterialsConfig);

    FString Error;
    if (!bDecoded
        || !CaptureMeshBounds(BakedMeshIndex, RuntimeLOD, Error)
        || !FGWorldBakedDataCapture::CaptureMesh(
            BakedMeshIndex,
            SourceMeshNames.IsValidIndex(MeshIndex)
                ? SourceMeshNames[MeshIndex] : FString(),
            RuntimeLOD,
            BakedData,
            MaterialIds,
            TextureIds,
            Error))
    {
        Complete(false, FString::Printf(
            TEXT("mesh %d decode/capture failed: %s"),
            MeshIndex,
            Error.IsEmpty() ? TEXT("glTFRuntime decode failed") : *Error));
        return;
    }

    ++CurrentMeshIndex;
    const float MeshFraction = MeshCount > 0
        ? static_cast<float>(CurrentMeshIndex) / static_cast<float>(MeshCount)
        : 1.0f;
    ProgressValue = FMath::Clamp(0.05f + MeshFraction * 0.85f, 0.05f, 0.90f);
    ScheduleNextMesh();
}

void UWorldSourceModelBuilder::ScheduleNextMesh()
{
    check(IsInGameThread());
    if (NextMeshTicker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(NextMeshTicker);
    const uint64 Generation = RequestGeneration;
    TWeakObjectPtr<UWorldSourceModelBuilder> WeakThis(this);
    // DispatchTrackedGameThread runs inline on GT: using it here recursively decoded every mesh
    // without presenting a frame. A retained ticker yields and can be removed during cancellation.
    NextMeshTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
        [WeakThis, Generation](float)
        {
            if (auto* Self = WeakThis.Get())
            {
                Self->NextMeshTicker.Reset();
                if (Self->bRunning && !Self->bCancelled && Self->RequestGeneration == Generation
                    && !FSafeFileIO::IsShuttingDown()) Self->CaptureNextMesh();
            }
            return false;
        }));
}

void UWorldSourceModelBuilder::CaptureSkinsAndMetadata()
{
    check(IsInGameThread());
    if (!bRunning || bCancelled || !IsValid(SourceAsset))
    {
        Complete(false, TEXT("source model build was cancelled before skin capture"));
        return;
    }

    FString Error;
    TArray<int32> SkinIndices;
    SkinMeshIndices.GetKeys(SkinIndices);
    SkinIndices.Sort();
    for (const int32 SkinIndex : SkinIndices)
    {
        // Called only from CaptureNextMeshUnderGate; do not acquire the same gate twice.
        const bool bCaptured = FGWorldBakedDataCapture::CaptureSkin(
            SourceAsset, SkinIndex, SkinMeshIndices.FindChecked(SkinIndex),
            Definition.Bones, BakedData, Error);
        if (!bCaptured)
        {
            Complete(false, FString::Printf(
                TEXT("skin %d capture failed: %s"), SkinIndex, *Error));
            return;
        }
    }

    FGWorldModelMetadata Metadata;
    if (!BakedData.IsSane(&Error)
        || !BuildMetadata(Metadata, Error)
        || !Metadata.IsSane(&Error)
        || Metadata.SourceMeshCount != BakedData.Meshes.Num()
        || Metadata.SourceMaterialCount != BakedData.Materials.Num()
        || Metadata.SourceTextureCount != BakedData.Textures.Num())
    {
        Complete(false, Error.IsEmpty()
            ? TEXT("decoded model validation failed") : Error);
        return;
    }

    CompletedModel.Definition = Definition;
    CompletedModel.DefinitionJson = DefinitionJson;
    CompletedModel.Metadata = MoveTemp(Metadata);
    CompletedModel.BakedData = MoveTemp(BakedData);
    CompletedModel.SourceFileSize = SourceFileSize;
    CompletedModel.SourceTimestamp = SourceTimestamp;
    ProgressValue = 0.98f;
    Complete(true);
}

bool UWorldSourceModelBuilder::BuildMetadata(
    FGWorldModelMetadata& OutMetadata,
    FString& OutError) const
{
    using namespace WorldSourceModelBuilderPrivate;
    check(IsInGameThread());
    OutMetadata = FGWorldModelMetadata();

    FModelData AuthoredSettings;
    if (!ParseModelSettings(DefinitionJson, AuthoredSettings, OutError)) return false;

    const TArray<FglTFRuntimeNode>& Nodes = SourceAsset->GetNodes();
    const int32 BakedMeshCount = BakedData.Meshes.Num();
    OutMetadata.SourceNodeCount = Nodes.Num();
    OutMetadata.SourceMeshCount = BakedMeshCount;
    OutMetadata.SourceMaterialCount = BakedData.Materials.Num();
    OutMetadata.SourceTextureCount = BakedData.Textures.Num();
    OutMetadata.NodeTransforms.Reserve(Nodes.Num());

    TMap<int32, const FglTFRuntimeNode*> NodeByIndex;
    NodeByIndex.Reserve(Nodes.Num());
    for (const FglTFRuntimeNode& Node : Nodes)
    {
        if (NodeByIndex.Contains(Node.Index))
        {
            OutError = FString::Printf(TEXT("duplicate glTF node index %d"), Node.Index);
            return false;
        }
        NodeByIndex.Add(Node.Index, &Node);
    }

    // Resolve model-space transforms iteratively. Parent chains are cached so large scenes remain
    // O(nodes + hierarchy edges) in the common case without risking recursion-stack overflow.
    TMap<int32, FTransform> ModelTransforms;
    ModelTransforms.Reserve(Nodes.Num());
    for (const FglTFRuntimeNode& StartNode : Nodes)
    {
        if (ModelTransforms.Contains(StartNode.Index)) continue;
        TArray<const FglTFRuntimeNode*> Chain;
        TSet<int32> ChainIds;
        const FglTFRuntimeNode* Current = &StartNode;
        FTransform ParentWorld = FTransform::Identity;
        while (Current)
        {
            if (!IsFiniteTransform(Current->Transform))
            {
                OutError = FString::Printf(TEXT("node %d has a non-finite local transform"), Current->Index);
                return false;
            }
            if (const FTransform* Cached = ModelTransforms.Find(Current->Index))
            {
                ParentWorld = *Cached;
                break;
            }
            if (ChainIds.Contains(Current->Index))
            {
                OutError = FString::Printf(TEXT("cycle detected in glTF node hierarchy at node %d"), Current->Index);
                return false;
            }
            ChainIds.Add(Current->Index);
            Chain.Add(Current);
            if (Current->ParentIndex < 0)
            {
                ParentWorld = FTransform::Identity;
                break;
            }
            const FglTFRuntimeNode* const* ParentPtr = NodeByIndex.Find(Current->ParentIndex);
            if (!ParentPtr || !*ParentPtr)
            {
                OutError = FString::Printf(TEXT("node %d references missing parent %d"), Current->Index, Current->ParentIndex);
                return false;
            }
            Current = *ParentPtr;
        }
        for (int32 Index = Chain.Num() - 1; Index >= 0; --Index)
        {
            ParentWorld = Chain[Index]->Transform * ParentWorld;
            if (!IsFiniteTransform(ParentWorld))
            {
                OutError = FString::Printf(TEXT("node %d produced an invalid model transform"), Chain[Index]->Index);
                return false;
            }
            ModelTransforms.Add(Chain[Index]->Index, ParentWorld);
        }
    }

    auto MakeUniqueStorageName = [](const FString& Preferred, int32 NodeIndex,
        const TMap<FName, FModelNodeData>& RenderMap,
        const TMap<FName, FWaterStreamNodeData>& WaterMap) -> FName
    {
        FString Base = Preferred.TrimStartAndEnd();
        if (Base.IsEmpty()) Base = FString::Printf(TEXT("Node_%d"), NodeIndex);
        FName Candidate(*Base);
        if (!RenderMap.Contains(Candidate) && !WaterMap.Contains(Candidate)) return Candidate;
        for (int32 Suffix = 0; Suffix < 1024; ++Suffix)
        {
            Candidate = FName(*FString::Printf(TEXT("%s__Node%d_%d"), *Base, NodeIndex, Suffix));
            if (!RenderMap.Contains(Candidate) && !WaterMap.Contains(Candidate)) return Candidate;
        }
        return NAME_None;
    };

    auto AssignLod = [&OutError](int32& Slot, const int32 Value, const FString& Base, const int32 Lod)
    {
        if (Slot != INDEX_NONE && Slot != Value)
        {
            OutError = FString::Printf(
                TEXT("ambiguous mesh family '%s': LOD%d resolves to multiple mesh payloads (%d/%d)"),
                *Base, Lod, Slot, Value);
            return false;
        }
        Slot = Value;
        return true;
    };

    TSet<FName> AuthoredBaseMeshNames;
    for (const FglTFRuntimeNode& SourceNode : Nodes)
    {
        if (IsWaterNode(SourceNode.Name)) continue;
        const FSimulatorParsedNodeName Parsed = USimulatorNodeTokenLibrary::ParseNodeName(SourceNode.Name);
        const FName BaseName(*Parsed.BaseName.TrimStartAndEnd());
        if (!BaseName.IsNone()) AuthoredBaseMeshNames.Add(BaseName);
    }

    // Explicit ;LOD0 is authoritative for the unsuffixed family name. Bare nodes with the same
    // base name are ordinary independent glTF nodes, so a distinct payload must not be forced into
    // the same LOD family. Pre-resolve explicit LOD0 rows so input node order cannot change which
    // payload keeps the canonical base name.
    TMap<FName, int32> ExplicitLod0ByBase;
    for (const FglTFRuntimeNode& SourceNode : Nodes)
    {
        if (IsWaterNode(SourceNode.Name)) continue;
        const int32* RuntimeMeshPtr = NodeToBakedMeshIndex.Find(SourceNode.Index);
        if (!RuntimeMeshPtr || *RuntimeMeshPtr < 0 || *RuntimeMeshPtr >= BakedMeshCount) continue;

        const FSimulatorParsedNodeName Parsed = USimulatorNodeTokenLibrary::ParseNodeName(SourceNode.Name);
        if (!Parsed.HasEffectiveToken(FName(TEXT("LOD0")))) continue;

        const FString BaseText = Parsed.BaseName.TrimStartAndEnd();
        const FName BaseName(*BaseText);
        if (BaseName.IsNone()) continue;

        if (const int32* Existing = ExplicitLod0ByBase.Find(BaseName))
        {
            if (*Existing != *RuntimeMeshPtr)
            {
                OutError = FString::Printf(
                    TEXT("ambiguous mesh family '%s': explicit LOD0 resolves to multiple mesh payloads (%d/%d)"),
                    *BaseText, *Existing, *RuntimeMeshPtr);
                return false;
            }
        }
        else
        {
            ExplicitLod0ByBase.Add(BaseName, *RuntimeMeshPtr);
        }
    }

    // If no explicit ;LOD0 exists, keep the lowest baked payload on the unsuffixed base name so
    // duplicate bare-node handling is deterministic even if glTF node traversal order changes.
    TMap<FName, int32> CanonicalLod0ByBase = ExplicitLod0ByBase;
    for (const FglTFRuntimeNode& SourceNode : Nodes)
    {
        if (IsWaterNode(SourceNode.Name)) continue;
        const int32* RuntimeMeshPtr = NodeToBakedMeshIndex.Find(SourceNode.Index);
        if (!RuntimeMeshPtr || *RuntimeMeshPtr < 0 || *RuntimeMeshPtr >= BakedMeshCount) continue;

        const FSimulatorParsedNodeName Parsed = USimulatorNodeTokenLibrary::ParseNodeName(SourceNode.Name);
        if (Parsed.HasEffectiveToken(FName(TEXT("LOD1")))
            || Parsed.HasEffectiveToken(FName(TEXT("LOD2")))
            || Parsed.HasEffectiveToken(FName(TEXT("LOD3"))))
        {
            continue;
        }

        const FName BaseName(*Parsed.BaseName.TrimStartAndEnd());
        if (BaseName.IsNone() || ExplicitLod0ByBase.Contains(BaseName)) continue;

        if (int32* Canonical = CanonicalLod0ByBase.Find(BaseName))
        {
            *Canonical = FMath::Min(*Canonical, *RuntimeMeshPtr);
        }
        else
        {
            CanonicalLod0ByBase.Add(BaseName, *RuntimeMeshPtr);
        }
    }

    // Generated disambiguation names inherit authored settings from their original base name.
    TMap<FName, FName> SettingsKeyByMeshFamily;

    auto MakeDistinctBareFamilyName = [&OutMetadata, &AuthoredBaseMeshNames](
        const FString& BaseText,
        const int32 RuntimeMeshIndex) -> FName
    {
        FString Stem = BaseText.TrimStartAndEnd();
        if (Stem.IsEmpty()) Stem = TEXT("Mesh");

        FName Candidate(*FString::Printf(TEXT("%s__Mesh%d"), *Stem, RuntimeMeshIndex));
        const auto CanUseCandidate = [&OutMetadata, &AuthoredBaseMeshNames, RuntimeMeshIndex](const FName Name)
        {
            if (AuthoredBaseMeshNames.Contains(Name)) return false;
            if (const FModelMeshData* Existing = OutMetadata.SceneData.MeshMap.Find(Name))
            {
                return Existing->LOD0 == INDEX_NONE || Existing->LOD0 == RuntimeMeshIndex;
            }
            return true;
        };
        if (CanUseCandidate(Candidate)) return Candidate;

        for (int32 Suffix = 1; Suffix < 1024; ++Suffix)
        {
            Candidate = FName(*FString::Printf(
                TEXT("%s__Mesh%d_%d"), *Stem, RuntimeMeshIndex, Suffix));
            if (CanUseCandidate(Candidate)) return Candidate;
        }
        return NAME_None;
    };

    for (const FglTFRuntimeNode& SourceNode : Nodes)
    {
        const int32* RuntimeMeshPtr = NodeToBakedMeshIndex.Find(SourceNode.Index);
        const int32 RuntimeMeshIndex = RuntimeMeshPtr ? *RuntimeMeshPtr : INDEX_NONE;
        FGWorldNodeTransform& TransformRow = OutMetadata.NodeTransforms.AddDefaulted_GetRef();
        TransformRow.NodeIndex = SourceNode.Index;
        TransformRow.ParentIndex = SourceNode.ParentIndex;
        TransformRow.MeshIndex = RuntimeMeshIndex;
        TransformRow.SkinIndex = SourceNode.SkinIndex;
        TransformRow.Name = SourceNode.Name;
        TransformRow.LocalTransform = SourceNode.Transform;

        const FTransform* ModelTransform = ModelTransforms.Find(SourceNode.Index);
        if (!ModelTransform) continue;
        const FString NodeNameText = SourceNode.Name.TrimStartAndEnd();
        const bool bWater = IsWaterNode(NodeNameText);
        const bool bValidMesh = RuntimeMeshIndex >= 0 && RuntimeMeshIndex < BakedMeshCount;
        if (!bWater && !bValidMesh) continue;

        const FName StorageName = MakeUniqueStorageName(
            NodeNameText, SourceNode.Index, OutMetadata.SceneData.NodeMap, OutMetadata.SceneData.WaterNodeMap);
        if (StorageName.IsNone())
        {
            OutError = FString::Printf(TEXT("could not create a unique storage key for node %d"), SourceNode.Index);
            return false;
        }

        if (bWater)
        {
            FWaterStreamNodeData Water;
            Water.Transform = *ModelTransform;
            Water.Transform.SetScale3D(ModelTransform->GetScale3D() * 100.0f);
            const FVector AbsScale = ModelTransform->GetScale3D().GetAbs();
            Water.StreamRadius = FMath::Max(2048.0f,
                FMath::Max3(AbsScale.X, AbsScale.Y, AbsScale.Z) * 65536.0f);
            OutMetadata.SceneData.WaterNodeMap.Add(StorageName, MoveTemp(Water));
            continue;
        }

        const FSimulatorParsedNodeName Parsed = USimulatorNodeTokenLibrary::ParseNodeName(NodeNameText);
        const FString BaseText = Parsed.BaseName.TrimStartAndEnd();
        const FName BaseMeshName(*BaseText);
        if (BaseMeshName.IsNone()) continue;

        FName MeshName = BaseMeshName;
        FModelMeshData* Mesh = &OutMetadata.SceneData.MeshMap.FindOrAdd(MeshName);
        SettingsKeyByMeshFamily.FindOrAdd(MeshName) = BaseMeshName;

        const bool bNoCollision = Parsed.HasEffectiveToken(FName(TEXT("NCOL")));
        if (Parsed.HasEffectiveToken(FName(TEXT("LOD1"))))
        {
            if (bNoCollision)
            {
                Mesh->Data.bComplexCollision = false;
                Mesh->Data.bSimpleCollision = false;
            }
            if (!AssignLod(Mesh->LOD1, RuntimeMeshIndex, BaseText, 1)) return false;
            continue;
        }
        if (Parsed.HasEffectiveToken(FName(TEXT("LOD2"))))
        {
            if (bNoCollision)
            {
                Mesh->Data.bComplexCollision = false;
                Mesh->Data.bSimpleCollision = false;
            }
            if (!AssignLod(Mesh->LOD2, RuntimeMeshIndex, BaseText, 2)) return false;
            continue;
        }
        if (Parsed.HasEffectiveToken(FName(TEXT("LOD3"))))
        {
            if (bNoCollision)
            {
                Mesh->Data.bComplexCollision = false;
                Mesh->Data.bSimpleCollision = false;
            }
            if (!AssignLod(Mesh->LOD3, RuntimeMeshIndex, BaseText, 3)) return false;
            continue;
        }

        const FVector* MeshSize = SourceMeshSizes.Find(RuntimeMeshIndex);
        if (!MeshSize || !IsFiniteVector(*MeshSize) || MeshSize->GetMin() < 0.0) continue;

        const bool bExplicitLod0 = Parsed.HasEffectiveToken(FName(TEXT("LOD0")));
        const int32* CanonicalLod0 = CanonicalLod0ByBase.Find(BaseMeshName);
        const bool bBareUsesDistinctPayload =
            !bExplicitLod0 && CanonicalLod0 && *CanonicalLod0 != RuntimeMeshIndex;
        const bool bBareCollidesWithExisting =
            !bExplicitLod0 && Mesh->LOD0 != INDEX_NONE && Mesh->LOD0 != RuntimeMeshIndex;

        if (bBareUsesDistinctPayload || bBareCollidesWithExisting)
        {
            MeshName = MakeDistinctBareFamilyName(BaseText, RuntimeMeshIndex);
            if (MeshName.IsNone())
            {
                OutError = FString::Printf(
                    TEXT("could not disambiguate duplicate bare mesh family '%s' for payload %d"),
                    *BaseText, RuntimeMeshIndex);
                return false;
            }

            Mesh = &OutMetadata.SceneData.MeshMap.FindOrAdd(MeshName);
            SettingsKeyByMeshFamily.FindOrAdd(MeshName) = BaseMeshName;
            UE_LOG(LogTemp, Display,
                TEXT("World build split duplicate bare mesh family: base=%s node=%d payload=%d family=%s"),
                *BaseText, SourceNode.Index, RuntimeMeshIndex, *MeshName.ToString());
        }

        if (bNoCollision)
        {
            Mesh->Data.bComplexCollision = false;
            Mesh->Data.bSimpleCollision = false;
        }
        if (!AssignLod(Mesh->LOD0, RuntimeMeshIndex, MeshName.ToString(), 0)) return false;
        Mesh->Size = *MeshSize;
        Mesh->Extent = *MeshSize * 0.5f;

        FModelNodeData Node;
        Node.MeshName = MeshName;
        Node.Transform = *ModelTransform;
        const bool bChunkAddressable =
            ComputeSpatialChunk(Node.Transform.GetLocation(), Node.CoarseChunk, Node.FineChunk);
        const FBox TransformedMeshBounds =
            FBox(-Mesh->Extent.GetAbs(), Mesh->Extent.GetAbs()).TransformBy(Node.Transform);
        const FVector EffectiveSize = TransformedMeshBounds.IsValid
            ? TransformedMeshBounds.GetSize().GetAbs()
            : FVector::ZeroVector;
        Node.bAlwaysLoaded = !bChunkAddressable || !TransformedMeshBounds.IsValid
            || !IsFiniteVector(EffectiveSize) || EffectiveSize.GetMax() > CoarseChunkCm;
        OutMetadata.SceneData.NodeMap.Add(StorageName, MoveTemp(Node));
    }

    // JSON settings override only author-controlled behavior. Generated split families inherit the
    // authored settings of their original base name unless an exact generated-family key exists.
    for (auto It = OutMetadata.SceneData.MeshMap.CreateIterator(); It; ++It)
    {
        FModelMeshData& Mesh = It.Value();
        if (Mesh.LOD0 == INDEX_NONE || !IsFiniteVector(Mesh.Extent) || !IsFiniteVector(Mesh.Size)
            || Mesh.Extent.GetMin() < 0.0 || Mesh.Size.GetMin() < 0.0)
        {
            It.RemoveCurrent();
            continue;
        }

        const FMeshData* Settings = AuthoredSettings.MeshData.Find(It.Key());
        if (!Settings)
        {
            if (const FName* BaseSettingsKey = SettingsKeyByMeshFamily.Find(It.Key()))
            {
                Settings = AuthoredSettings.MeshData.Find(*BaseSettingsKey);
            }
        }
        if (Settings) Mesh.Data = *Settings;
    }
    for (auto It = OutMetadata.SceneData.NodeMap.CreateIterator(); It; ++It)
        if (!OutMetadata.SceneData.MeshMap.Contains(It.Value().MeshName)) It.RemoveCurrent();

    TSet<FName> ReferencedMeshes;
    for (const TPair<FName, FModelNodeData>& Pair : OutMetadata.SceneData.NodeMap) ReferencedMeshes.Add(Pair.Value.MeshName);
    for (auto It = OutMetadata.SceneData.MeshMap.CreateIterator(); It; ++It)
        if (!ReferencedMeshes.Contains(It.Key())) It.RemoveCurrent();

    AuthoredSettings.Center = FVector::ZeroVector;
    AuthoredSettings.Size = FVector::ZeroVector;
    FBox WorldBounds(ForceInit);
    for (const TPair<FName, FModelNodeData>& Pair : OutMetadata.SceneData.NodeMap)
    {
        const FModelMeshData* Mesh = OutMetadata.SceneData.MeshMap.Find(Pair.Value.MeshName);
        if (!Mesh) continue;
        const FVector Extent = Mesh->Extent.GetAbs();
        WorldBounds += FBox(-Extent, Extent).TransformBy(Pair.Value.Transform);
    }
    for (const TPair<FName, FWaterStreamNodeData>& Pair : OutMetadata.SceneData.WaterNodeMap)
    {
        // StreamRadius is already expressed in model-space centimetres and includes the authored
        // node scale. Transforming an extent box by Water.Transform would apply that scale a
        // second time and can inflate world bounds by orders of magnitude.
        WorldBounds += FBox::BuildAABB(Pair.Value.Transform.GetLocation(), FVector(Pair.Value.StreamRadius));
    }
    if (WorldBounds.IsValid)
    {
        AuthoredSettings.Center = WorldBounds.GetCenter();
        AuthoredSettings.Size = WorldBounds.GetSize();
    }
    // Keep complete source-node rows deterministic for byte-stable metadata and easier corruption diagnostics.
    OutMetadata.NodeTransforms.Sort([](const FGWorldNodeTransform& A, const FGWorldNodeTransform& B)
    {
        return A.NodeIndex < B.NodeIndex;
    });
    OutMetadata.SceneData.ModelData = MoveTemp(AuthoredSettings);
    OutMetadata.SceneData.bSuccess = true;
    return true;
}

void UWorldSourceModelBuilder::Complete(
    bool bSuccess,
    const FString& Error)
{
    check(IsInGameThread());
    if (bCompletionSent)
    {
        return;
    }

    if (bSuccess)
    {
        const int64 CurrentSize =
            IFileManager::Get().FileSize(*Definition.GlbPath);
        const FDateTime CurrentTimestamp =
            IFileManager::Get().GetTimeStamp(*Definition.GlbPath);
        if (CurrentSize != SourceFileSize
            || (SourceTimestamp != FDateTime()
                && CurrentTimestamp != SourceTimestamp))
        {
            bSuccess = false;
            FailureReason = TEXT("source GLB changed while it was being decoded");
        }
    }
    if (!bSuccess)
    {
        FailureReason = FailureReason.IsEmpty()
            ? (Error.IsEmpty() ? TEXT("source model build failed") : Error)
            : FailureReason;
        CompletedModel = FGWorldBuildModel();
        BakedData.Reset();
        FString ExistingQuarantineReason;
        if (!FglTFRuntimeSafety::IsPathQuarantined(
                Definition.GlbPath, &ExistingQuarantineReason))
        {
            FglTFRuntimeSafety::ReportRecoverableFailure(
                Definition.GlbPath, FailureReason);
        }
        UFileFunctionLibrary::WriteSimulatorLogAsync(
            TEXT("WorldSourceModelBuilder"),
            FString::Printf(TEXT("Build failed. GLB=%s Reason=%s"),
                *Definition.GlbPath, *FailureReason));
    }

    if (bSuccess)
    {
        FglTFRuntimeSafety::ClearRecoverableFailure(Definition.GlbPath);
    }

    bRunning = false;
    bCompletionSent = true;
    ProgressValue = 1.0f;
    ReleaseSourceAsset();
    OnFinished.Broadcast(this, bSuccess);
}

void UWorldSourceModelBuilder::ReleaseSourceAsset()
{
    check(IsInGameThread());
    if (IsValid(SourceAsset))
    {
        FglTFRuntimeSafety::RequestAssetRelease(SourceAsset);
    }
    SourceAsset = nullptr;
    MaterialIds.Reset();
    TextureIds.Reset();
    SourceMeshNames.Reset();
    SourceToBakedMeshIndex.Reset();
    NodeToBakedMeshIndex.Reset();
    SourceMeshesToCapture.Reset();
    SourceMeshSizes.Reset();
    SkinMeshIndices.Reset();
}

FGWorldBuildModel UWorldSourceModelBuilder::TakeBuildModel()
{
    check(IsInGameThread());
    return MoveTemp(CompletedModel);
}

void UWorldSourceModelBuilder::Cancel()
{
    check(IsInGameThread());
    if (bCancelled)
    {
        return;
    }
    bCancelled = true;
    bRunning = false;
    if (NextMeshTicker.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(NextMeshTicker);
        NextMeshTicker.Reset();
    }
    CompletedModel = FGWorldBuildModel();
    BakedData.Reset();
    ++RequestGeneration;
    FglTFRuntimeSafety::CancelQueuedOperations(this);
    ReleaseSourceAsset();
    OnFinished.Clear();
}

void UWorldSourceModelBuilder::BeginDestroy()
{
    if (IsInGameThread())
    {
        Cancel();
    }
    Super::BeginDestroy();
}
