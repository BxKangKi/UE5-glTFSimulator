/**
 * @file WorldArchiveTests.cpp
 * 역할: 월드 아카이브·청크 저장 회귀 테스트입니다.
 * 핵심 기능: 바이너리 검증, 커밋 복구, 월드 폴더 검증.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "System/EntityArchive.h"
#include "System/GameManagerSubSystem.h"
#include "System/WorldArchive.h"
#include "System/WorldBakedModelAsset.h"
#include "System/WorldObjectStreamingSubsystem.h"
#include "Simulator/ModelDefinitionJson.h"
#include "Simulator/RuntimeModelResolver.h"

#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PixelFormat.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace WorldArchiveTestsPrivate
{
    void AppendU32(TArray<uint8>& Bytes, const uint32 Value)
    {
        Bytes.Add(static_cast<uint8>(Value));
        Bytes.Add(static_cast<uint8>(Value >> 8u));
        Bytes.Add(static_cast<uint8>(Value >> 16u));
        Bytes.Add(static_cast<uint8>(Value >> 24u));
    }

    /** Creates the smallest useful GLB 2.0: one JSON chunk and no binary payload. */
    TArray<uint8> MakeMinimalGlb()
    {
        const ANSICHAR Json[] =
            "{\"asset\":{\"version\":\"2.0\"},\"nodes\":[{\"name\":\"Root\"}]}";
        TArray<uint8> JsonBytes;
        JsonBytes.Append(reinterpret_cast<const uint8*>(Json), UE_ARRAY_COUNT(Json) - 1);
        while ((JsonBytes.Num() & 3) != 0) JsonBytes.Add(static_cast<uint8>(' '));

        TArray<uint8> Bytes;
        AppendU32(Bytes, 0x46546C67u); // glTF
        AppendU32(Bytes, 2u);
        AppendU32(Bytes, static_cast<uint32>(20 + JsonBytes.Num()));
        AppendU32(Bytes, static_cast<uint32>(JsonBytes.Num()));
        AppendU32(Bytes, 0x4E4F534Au); // JSON
        Bytes.Append(JsonBytes);
        return Bytes;
    }

    FString MakeTestRoot(const TCHAR* Prefix)
    {
        return FPaths::Combine(
            FPaths::ProjectSavedDir(),
            TEXT("Automation"),
            FString::Printf(TEXT("%s-%s"), Prefix,
                *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldFolderNameValidationTest,
    "glTFSimulator.World.WorldFolderNameValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldFolderNameValidationTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    FString Normalized;

    // The transport accepts an opaque child-folder key and normalizes only surrounding whitespace.
    TestTrue(TEXT("Valid direct world key is accepted"),
        UGameManagerSubSystem::TryNormalizeWorldFolderName(
            TEXT("  DemoWorld  "), Normalized, false));
    TestEqual(TEXT("World key whitespace is normalized"),
        Normalized, FString(TEXT("DemoWorld")));

    // Exercise every path-escape class independently so future travel/reflection edits cannot
    // accidentally weaken the common validator used by URL and replicated world names.
    TestFalse(TEXT("Parent traversal is rejected"),
        UGameManagerSubSystem::TryNormalizeWorldFolderName(
            TEXT("../Outside"), Normalized, false));
    TestFalse(TEXT("Nested forward-slash path is rejected"),
        UGameManagerSubSystem::TryNormalizeWorldFolderName(
            TEXT("Nested/World"), Normalized, false));
    TestFalse(TEXT("Nested backslash path is rejected"),
        UGameManagerSubSystem::TryNormalizeWorldFolderName(
            TEXT("Nested\\World"), Normalized, false));
    TestFalse(TEXT("Empty key is rejected"),
        UGameManagerSubSystem::TryNormalizeWorldFolderName(
            FString(), Normalized, false));

    // These reflected values existed before the object-authoring item was removed. Changing them
    // would reinterpret serialized Blueprint defaults even though the C++ enum names still compile.
    TestEqual(TEXT("Static toolbar wire value remains compatible"),
        static_cast<uint8>(EToolbarItemKind::Static), static_cast<uint8>(2));
    TestEqual(TEXT("Weapon toolbar wire value remains compatible"),
        static_cast<uint8>(EToolbarItemKind::Weapon), static_cast<uint8>(3));
    TestEqual(TEXT("Vehicle toolbar wire value remains compatible"),
        static_cast<uint8>(EToolbarItemKind::Vehicle), static_cast<uint8>(4));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecursiveWorldSourceDiscoveryTest,
    "glTFSimulator.Streaming.RecursiveWorldSourceDiscovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRecursiveWorldSourceDiscoveryTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    using namespace WorldArchiveTestsPrivate;
    const FString Root = MakeTestRoot(TEXT("RecursiveSources"));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*Root, false, true);
    };

    const FString Resources = FPaths::Combine(Root, TEXT("resources"));
    const FString NestedA = FPaths::Combine(Resources, TEXT("model"), TEXT("city"));
    const FString NestedB = FPaths::Combine(Resources, TEXT("props"), TEXT("vehicles"), TEXT("land"));
    TestTrue(TEXT("Create first recursive source directory"),
        IFileManager::Get().MakeDirectory(*NestedA, true));
    TestTrue(TEXT("Create second recursive source directory"),
        IFileManager::Get().MakeDirectory(*NestedB, true));

    const TArray<uint8> MinimalGlb = MakeMinimalGlb();
    const FString DirectGlb = FPaths::Combine(Resources, TEXT("direct.glb"));
    const FString UpperGlb = FPaths::Combine(NestedA, TEXT("upper.GLB"));
    const FString MixedGlb = FPaths::Combine(NestedB, TEXT("mixed.GlB"));
    TestTrue(TEXT("Write direct GLB"), FFileHelper::SaveArrayToFile(MinimalGlb, *DirectGlb));
    TestTrue(TEXT("Write upper-case nested GLB"), FFileHelper::SaveArrayToFile(MinimalGlb, *UpperGlb));
    TestTrue(TEXT("Write mixed-case deeply nested GLB"), FFileHelper::SaveArrayToFile(MinimalGlb, *MixedGlb));
    TestTrue(TEXT("Write an unrelated file"), FFileHelper::SaveStringToFile(
        TEXT("not a model"), *FPaths::Combine(NestedB, TEXT("ignore.txt"))));

    // None is deliberately treated as the all-GLB policy's untyped Static default. This keeps
    // an existing author JSON from preventing its sibling GLB from entering the world build.
    const FGuid DirectUUID = FGuid::NewGuid();
    const FString DirectJson = FPaths::ChangeExtension(DirectGlb, TEXT("json"));
    const FString UntypedDefinition = FString::Printf(
        TEXT("{\"UUID\":\"%s\",\"Name\":\"direct\",\"DisplayName\":\"Direct\",\"ModelType\":\"None\"}"),
        *DirectUUID.ToString(EGuidFormats::DigitsWithHyphensLower));
    TestTrue(TEXT("Write existing untyped definition"),
        FFileHelper::SaveStringToFile(UntypedDefinition, *DirectJson));

    TArray<FString> CreatedJsonFiles;
    TArray<FString> DiscoveredGlbFiles;
    TestTrue(TEXT("Recursively discover every GLB and create missing definitions"),
        ModelDefinitionJson::EnsureMissingDefinitions(
            Resources, &CreatedJsonFiles, &DiscoveredGlbFiles));
    TestEqual(TEXT("Every extension/case/depth is discovered"), DiscoveredGlbFiles.Num(), 3);
    TestEqual(TEXT("Only missing sibling definitions are generated"), CreatedJsonFiles.Num(), 2);
    TestTrue(TEXT("Upper-case GLB gets a sibling JSON"),
        IFileManager::Get().FileExists(*FPaths::ChangeExtension(UpperGlb, TEXT("json"))));
    TestTrue(TEXT("Mixed-case GLB gets a sibling JSON"),
        IFileManager::Get().FileExists(*FPaths::ChangeExtension(MixedGlb, TEXT("json"))));

    FModelDefinition ParsedDefinition;
    FString ParseError;
    FString CanonicalJson;
    TestTrue(TEXT("Untyped definition remains buildable"),
        ModelDefinitionJson::LoadDefinition(
            DirectJson, DirectGlb, ParsedDefinition, ParseError, &CanonicalJson));
    TestTrue(TEXT("Untyped definition normalizes to Static"),
        ParsedDefinition.ModelType == EModelDefinitionType::Static);
    TestTrue(TEXT("Archive snapshot contains normalized Static type"),
        CanonicalJson.Contains(TEXT("\"ModelType\":\"Static\"")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGWorldArchiveRoundTripTest,
    "glTFSimulator.Streaming.GwdRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGWorldArchiveRoundTripTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    using namespace WorldArchiveTestsPrivate;
    const FString Sandbox = MakeTestRoot(TEXT("Gwd"));
    const FString Root = FPaths::Combine(Sandbox, TEXT("Worlds"), TEXT("TestWorld"));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*Sandbox, false, true);
    };

    const FString ModelDirectory = FPaths::Combine(
        Sandbox, TEXT("Projects"), TEXT("TestWorld"), TEXT("resources"), TEXT("model"));
    TestTrue(TEXT("Create model source directory"),
        IFileManager::Get().MakeDirectory(*ModelDirectory, true));
    const FString GlbPath = FPaths::Combine(ModelDirectory, TEXT("minimal.glb"));
    const TArray<uint8> OriginalGlb = MakeMinimalGlb();
    TestTrue(TEXT("Write minimal source GLB"),
        FFileHelper::SaveArrayToFile(OriginalGlb, *GlbPath));

    FGWorldBuildModel Model;
    Model.Definition.UUID = FGuid::NewGuid();
    Model.Definition.Name = TEXT("Minimal");
    Model.Definition.DisplayName = TEXT("Minimal Test Model");
    Model.Definition.GlbPath = GlbPath;
    Model.Definition.JsonPath = FPaths::ChangeExtension(GlbPath, TEXT("json"));
    Model.Definition.ModelType = EModelDefinitionType::Dynamic;
    Model.Definition.EntityType = EModelEntityType::Prop;
    Model.DefinitionJson = FString::Printf(
        TEXT("{\"UUID\":\"%s\",\"Name\":\"Minimal\",\"DisplayName\":\"Minimal Test Model\",\"ModelType\":\"Dynamic\",\"EntityType\":\"Prop\"}"),
        *Model.Definition.UUID.ToString(EGuidFormats::DigitsWithHyphensLower));
    Model.Metadata.SourceNodeCount = 1;
    Model.Metadata.SourceMeshCount = 2;
    Model.Metadata.SourceMaterialCount = 2;
    Model.Metadata.SourceTextureCount = 2;

    FModelMeshData& SceneMesh = Model.Metadata.SceneData.MeshMap.Add(FName(TEXT("Triangle")));
    SceneMesh.LOD0 = 0;
    SceneMesh.Size = FVector(100.0, 100.0, 0.0);
    SceneMesh.Extent = SceneMesh.Size * 0.5;
    FModelNodeData& SceneNode = Model.Metadata.SceneData.NodeMap.Add(FName(TEXT("Root")));
    SceneNode.MeshName = FName(TEXT("Triangle"));
    SceneNode.Transform = FTransform(
        FQuat(FVector::UpVector, 0.125).GetNormalized(),
        FVector(10.0, 20.0, 30.0),
        FVector::OneVector);
    SceneNode.CoarseChunk = FIntVector::ZeroValue;
    SceneNode.FineChunk = FIntVector::ZeroValue;
    SceneNode.bAlwaysLoaded = false;

    FGWorldNodeTransform& RootNode = Model.Metadata.NodeTransforms.AddDefaulted_GetRef();
    RootNode.NodeIndex = 0;
    RootNode.Name = TEXT("Root");
    RootNode.MeshIndex = 0;
    RootNode.LocalTransform = FTransform(
        FQuat(FVector::UpVector, 0.125).GetNormalized(),
        FVector(10.0, 20.0, 30.0),
        FVector::OneVector);
    Model.SourceFileSize = IFileManager::Get().FileSize(*GlbPath);
    Model.SourceTimestamp = IFileManager::Get().GetTimeStamp(*GlbPath);

    // Feed the archive writer the same decoded, Unreal-coordinate data produced by the build-time
    // glTFRuntime capture pass. The source GLB is only an identity stamp from this point onward.
    FGWorldBakedMesh& BakedMesh = Model.BakedData.Meshes.AddDefaulted_GetRef();
    BakedMesh.MeshIndex = 0;
    BakedMesh.Name = TEXT("Triangle");
    FGWorldBakedPrimitive& Primitive = BakedMesh.Primitives.AddDefaulted_GetRef();
    Primitive.Positions = {
        FVector3f(0.0f, 0.0f, 0.0f),
        FVector3f(100.0f, 0.0f, 0.0f),
        FVector3f(0.0f, 100.0f, 0.0f)};
    Primitive.Indices = {0u, 1u, 2u};
    Primitive.bHasIndices = true;
    Primitive.bHasMaterial = true;
    Primitive.MaterialId = 0;
    Primitive.MaterialName = TEXT("TestMaterial");

    // A second unrequested mesh with distinct dependencies proves that a bundle read does not
    // materialize every model member merely because they share one .gwd container.
    FGWorldBakedMesh SecondMesh = BakedMesh;
    SecondMesh.MeshIndex = 1;
    SecondMesh.Name = TEXT("UnrequestedTriangle");
    SecondMesh.Primitives[0].MaterialId = 1;
    SecondMesh.Primitives[0].MaterialName = TEXT("UnrequestedMaterial");
    Model.BakedData.Meshes.Add(MoveTemp(SecondMesh));

    FGWorldBakedMaterial& BakedMaterial = Model.BakedData.Materials.AddDefaulted_GetRef();
    BakedMaterial.MaterialId = 0;
    BakedMaterial.Name = TEXT("TestMaterial");
    BakedMaterial.BaseMaterialPath = TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial");
    FGWorldBakedTextureParameter& TextureParameter = BakedMaterial.Textures.AddDefaulted_GetRef();
    TextureParameter.Name = TEXT("BaseColorTexture");
    TextureParameter.TextureId = 0;

    FGWorldBakedMaterial SecondMaterial = BakedMaterial;
    SecondMaterial.MaterialId = 1;
    SecondMaterial.Name = TEXT("UnrequestedMaterial");
    SecondMaterial.Textures[0].TextureId = 1;
    Model.BakedData.Materials.Add(MoveTemp(SecondMaterial));

    FGWorldBakedTexture& BakedTexture = Model.BakedData.Textures.AddDefaulted_GetRef();
    BakedTexture.TextureId = 0;
    BakedTexture.Name = TEXT("WhitePixel");
    BakedTexture.SizeX = 1;
    BakedTexture.SizeY = 1;
    BakedTexture.PixelFormat = PF_B8G8R8A8;
    FGWorldBakedTextureMip& BakedMip = BakedTexture.Mips.AddDefaulted_GetRef();
    BakedMip.SizeX = 1;
    BakedMip.SizeY = 1;
    BakedMip.Bytes = {255u, 255u, 255u, 255u};

    FGWorldBakedTexture SecondTexture = BakedTexture;
    SecondTexture.TextureId = 1;
    SecondTexture.Name = TEXT("UnrequestedPixel");
    SecondTexture.Mips[0].Bytes = {0u, 0u, 0u, 255u};
    Model.BakedData.Textures.Add(MoveTemp(SecondTexture));

    TArray<FGWorldBuildModel> Models;
    Models.Add(Model);
    FString ArchivePath;
    FString Error;
    const FString WorldConfigJson = TEXT("{\"WorldName\":\"Archive Test World\"}");
    TestTrue(TEXT("Build immutable archive"),
        FGWorldArchive::BuildBlocking(
            Root, Models, ArchivePath, Error, TFunction<bool()>(), WorldConfigJson));
    TestTrue(TEXT("Archive was published"), IFileManager::Get().FileExists(*ArchivePath));

    TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Reader =
        FGWorldArchiveReader::Open(ArchivePath, Error);
    TestTrue(TEXT("Open archive directory"), Reader.IsValid());
    if (!Reader.IsValid()) return false;
    TestEqual(TEXT("Directory model count"), Reader->Num(), 1);
    FString EmbeddedConfig;
    TestTrue(TEXT("Range-read embedded config.json"),
        Reader->ReadWorldConfig(EmbeddedConfig, Error));
    TestEqual(TEXT("Embedded config.json round-trips"), EmbeddedConfig, WorldConfigJson);

    TMap<FString, FString> Bones;
    FString DefinitionJson;
    TestTrue(TEXT("Range-read model definition"), Reader->ReadModelDefinition(
        Model.Definition.UUID, Bones, DefinitionJson, Error));
    TestEqual(TEXT("Definition JSON round-trips"), DefinitionJson, Model.DefinitionJson);

    FGWorldModelMetadata Metadata;
    TestTrue(TEXT("Range-read model metadata"), Reader->ReadModelMetadata(
        Model.Definition.UUID, Metadata, Error));
    TestEqual(TEXT("Complete source-node count round-trips"), Metadata.SourceNodeCount, 1);
    TestEqual(TEXT("Complete node-transform row count"), Metadata.NodeTransforms.Num(), 1);
    TestEqual(TEXT("Static placement row count"), Metadata.SceneData.NodeMap.Num(), 1);
    if (const FModelNodeData* LoadedNode = Metadata.SceneData.NodeMap.Find(FName(TEXT("Root"))))
    {
        TestTrue(TEXT("8192 m coarse chunk round-trips"),
            LoadedNode->CoarseChunk == FIntVector::ZeroValue);
        TestTrue(TEXT("512 m fine chunk round-trips"),
            LoadedNode->FineChunk == FIntVector::ZeroValue);
        TestFalse(TEXT("Small mesh remains distance-streamed"), LoadedNode->bAlwaysLoaded);
    }
    if (Metadata.NodeTransforms.Num() == 1)
    {
        TestEqual(TEXT("Complete node index"), Metadata.NodeTransforms[0].NodeIndex, 0);
        TestEqual(TEXT("Complete node name"), Metadata.NodeTransforms[0].Name, FString(TEXT("Root")));
        TestTrue(TEXT("Complete node local transform"),
            Metadata.NodeTransforms[0].LocalTransform.Equals(RootNode.LocalTransform));
    }

    FGWorldModelManifest Manifest;
    TestTrue(TEXT("Range-read model manifest"), Reader->ReadModelManifest(
        Model.Definition.UUID, Manifest, Error));
    TestEqual(TEXT("Every mesh member is indexed"), Manifest.MeshRanges.Num(), 2);
    TestEqual(TEXT("Every material member is indexed"), Manifest.MaterialRanges.Num(), 2);
    TestEqual(TEXT("Every texture member is indexed"), Manifest.TextureRanges.Num(), 2);
    if (const FGWorldArchiveRange* MeshRange = Manifest.MeshRanges.Find(0))
    {
        TestTrue(TEXT("Mesh is an independently addressable archive member"),
            MeshRange->Name.EndsWith(TEXT("/meshes/0.dat")));
    }

    FGWorldBakedAssetBundle Bundle;
    const TArray<int32> RequestedMeshes{0};
    TestTrue(TEXT("Range-read one mesh and only its dependencies"), Reader->ReadMeshBundle(
        Model.Definition.UUID, Manifest, RequestedMeshes, INDEX_NONE, true, Bundle, Error));
    TestEqual(TEXT("One decoded mesh was loaded"), Bundle.Meshes.Num(), 1);
    TestEqual(TEXT("Referenced material was loaded"), Bundle.Materials.Num(), 1);
    TestEqual(TEXT("Referenced texture was loaded"), Bundle.Textures.Num(), 1);
    if (Bundle.Meshes.Num() == 1 && Bundle.Meshes[0].Primitives.Num() == 1)
    {
        TestEqual(TEXT("Decoded vertex count round-trips"),
            Bundle.Meshes[0].Primitives[0].Positions.Num(), 3);
        TestEqual(TEXT("Decoded index count round-trips"),
            Bundle.Meshes[0].Primitives[0].Indices.Num(), 3);
    }

    Bundle.Reset();
    TestTrue(TEXT("Material-skipping read loads only mesh bytes"), Reader->ReadMeshBundle(
        Model.Definition.UUID, Manifest, RequestedMeshes, INDEX_NONE, false, Bundle, Error));
    TestEqual(TEXT("Skipped material table stays empty"), Bundle.Materials.Num(), 0);
    TestEqual(TEXT("Skipped texture table stays empty"), Bundle.Textures.Num(), 0);

    // Runtime must remain independent from authoring resources after a verified build exists.
    TestTrue(TEXT("Delete source after build"), IFileManager::Get().Delete(*GlbPath));
    Bundle.Reset();
    TestTrue(TEXT("Range-read decoded bundle without source GLB"), Reader->ReadMeshBundle(
        Model.Definition.UUID, Manifest, RequestedMeshes, INDEX_NONE, true, Bundle, Error));
    TestEqual(TEXT("Runtime never needs source GLB bytes"), Bundle.Meshes.Num(), 1);

    // Exercise the real gameplay path, not only the codecs: the facade must rebuild a native
    // UStaticMesh (including the baked material/texture dependencies) while the source is absent.
    FResolvedRuntimeModel RuntimeModel;
    RuntimeModel.UUID = Model.Definition.UUID;
    RuntimeModel.Definition = Model.Definition;
    RuntimeModel.Reference = FGWorldArchive::MakeModelReference(Model.Definition.UUID);
    RuntimeModel.DefinitionJson = Model.DefinitionJson;
    RuntimeModel.ArchiveReader = Reader;
    UWorldBakedModelAsset* RuntimeAsset =
        FRuntimeModelResolver::LoadAssetSynchronously(RuntimeModel, Error);
    TestNotNull(TEXT("Create baked runtime facade without source GLB"), RuntimeAsset);
    if (IsValid(RuntimeAsset))
    {
        FglTFRuntimeStaticMeshConfig MeshConfig;
        MeshConfig.Outer = GetTransientPackage();
        MeshConfig.CacheMode = EglTFRuntimeCacheMode::None;
        MeshConfig.MaterialsConfig.CacheMode = EglTFRuntimeCacheMode::None;
        UStaticMesh* RuntimeMesh = RuntimeAsset->LoadStaticMesh(0, MeshConfig, &Error);
        TestNotNull(TEXT("Build native static mesh from decoded archive members"), RuntimeMesh);
    }

    FString RebuildPath;
    FString RebuildError;
    TestFalse(TEXT("Existing build blocks source rebuild"),
        FGWorldArchive::BuildBlocking(Root, Models, RebuildPath, RebuildError));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityArchiveRecoveryTest,
    "glTFSimulator.Streaming.EntityCommitRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEntityArchiveRecoveryTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    using namespace WorldArchiveTestsPrivate;
    const FString Sandbox = MakeTestRoot(TEXT("Dat"));
    const FString Root = FPaths::Combine(Sandbox, TEXT("Worlds"), TEXT("EntityTest"));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*Sandbox, false, true);
    };

    FString Error;
    TSharedPtr<FEntityArchiveStore, ESPMode::ThreadSafe> Store =
        FEntityArchiveStore::Open(Root, true, Error);
    TestTrue(TEXT("Create entity archive"), Store.IsValid());
    if (!Store.IsValid()) return false;

    const FWorldChunkCoordinate SourceCoordinate{2, -3, 1};
    const FWorldChunkCoordinate DestinationCoordinate{3, -3, 1};
    TestFalse(TEXT("Fresh archive directory has no source chunk"),
        Store->ContainsChunk(SourceCoordinate));
    FWorldChunkObject Object;
    Object.EntityUUID = FGuid::NewGuid();
    Object.ModelUUID = FGuid::NewGuid();
    Object.Location = FVector(123.0, -456.0, 789.0);
    Object.Rotation = FQuat(FVector::UpVector, 0.25).GetNormalized();
    Object.Scale = FVector(1.0, 2.0, 1.0);
    Object.Velocity = FVector(10.0, 20.0, 30.0);
    Object.AngularVelocity = FVector(0.1, 0.2, 0.3);

    TArray<FWorldChunkObject> Objects;
    Objects.Add(Object);
    TestTrue(TEXT("Commit entity chunk"),
        Store->SaveChunk(SourceCoordinate, Objects).IsSuccess());
    TestTrue(TEXT("Committed source chunk is present in the directory"),
        Store->ContainsChunk(SourceCoordinate));

    FWorldRuntimeState State;
    State.WorldTime = 42.0f;
    State.SelectedPlayer = TEXT("Player");
    TestTrue(TEXT("Commit runtime state"), Store->SaveRuntimeState(State).IsSuccess());

    // Publish both halves of a boundary move under one footer. Recovery must never expose the
    // object in both coordinates or in neither coordinate.
    FWorldChunkObject MovedObject = Object;
    MovedObject.Location.X += UWorldObjectStreamingSubsystem::ChunkSizeCentimeters;
    TMap<FWorldChunkCoordinate, TArray<FWorldChunkObject>> MovedChunks;
    MovedChunks.Add(SourceCoordinate, TArray<FWorldChunkObject>());
    MovedChunks.Add(DestinationCoordinate, TArray<FWorldChunkObject>{MovedObject});
    TMap<FWorldChunkCoordinate, uint64> WriteOrders;
    WriteOrders.Add(SourceCoordinate, Store->ReserveChunkWrite(SourceCoordinate));
    WriteOrders.Add(DestinationCoordinate, Store->ReserveChunkWrite(DestinationCoordinate));
    TestTrue(TEXT("Commit atomic boundary batch"),
        Store->SaveChunks(MovedChunks, WriteOrders).IsSuccess());
    TestTrue(TEXT("Committed empty source row remains distinguishable from a missing chunk"),
        Store->ContainsChunk(SourceCoordinate));
    TestTrue(TEXT("Committed destination chunk is present in the directory"),
        Store->ContainsChunk(DestinationCoordinate));

    const FString EntityPath = Store->GetPath();
    Store.Reset();

    // Simulate a process interruption after an append began but before its footer was durable.
    const TArray<uint8> TornTail{0x47, 0x43, 0x4f, 0x4d, 0x01, 0x02, 0x03};
    TestTrue(TEXT("Append torn tail"), FFileHelper::SaveArrayToFile(
        TornTail, *EntityPath, &IFileManager::Get(), FILEWRITE_Append));

    Store = FEntityArchiveStore::Open(Root, false, Error);
    TestTrue(TEXT("Recover preceding commit footer"), Store.IsValid());
    if (!Store.IsValid()) return false;

    TArray<FWorldChunkObject> SourceObjects;
    TestTrue(TEXT("Range-read recovered source chunk"),
        Store->LoadChunk(SourceCoordinate, SourceObjects, Error));
    TestEqual(TEXT("Recovered source is empty"), SourceObjects.Num(), 0);

    TArray<FWorldChunkObject> DestinationObjects;
    TestTrue(TEXT("Range-read recovered destination chunk"),
        Store->LoadChunk(DestinationCoordinate, DestinationObjects, Error));
    TestEqual(TEXT("Recovered destination object count"), DestinationObjects.Num(), 1);
    if (DestinationObjects.Num() == 1)
    {
        TestTrue(TEXT("Recovered entity UUID"),
            DestinationObjects[0].EntityUUID == MovedObject.EntityUUID);
        TestTrue(TEXT("Recovered model UUID"),
            DestinationObjects[0].ModelUUID == MovedObject.ModelUUID);
        TestTrue(TEXT("Recovered location"),
            DestinationObjects[0].Location.Equals(MovedObject.Location));
        TestTrue(TEXT("Recovered velocity"),
            DestinationObjects[0].Velocity.Equals(MovedObject.Velocity));
    }

    FWorldRuntimeState LoadedState;
    bool bMissing = true;
    TestTrue(TEXT("Range-read recovered runtime state"),
        Store->LoadRuntimeState(LoadedState, bMissing, Error));
    TestFalse(TEXT("Runtime state is present"), bMissing);
    TestEqual(TEXT("Runtime time round-trips"), LoadedState.WorldTime, State.WorldTime);
    return true;
}
#endif
