/**
 * @file ModelDefinitionJson.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Simulator/ModelDefinitionJson.h"

#include "Character/CharacterBoneSchema.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "System/SafeFileIO.h"

namespace
{
    bool WriteGeneratedDefinitionAtomically(const FString& GlbPath, const FString& JsonPath)
    {
        // A unique sibling temporary file keeps partial JSON out of the model
        // directory even if the process terminates while the write is active.
        const FString TemporaryPath = JsonPath + TEXT(".tmp.")
            + FGuid::NewGuid().ToString(EGuidFormats::Digits);

        const FString BaseName = FPaths::GetBaseFilename(GlbPath);
        const TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
        RootObject->SetStringField(TEXT("UUID"),
            FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
        RootObject->SetStringField(TEXT("Name"), BaseName);
        RootObject->SetStringField(TEXT("DisplayName"), BaseName);
        // A newly discovered GLB is immediately usable as part of the authored map. Developers
        // can change this to Dynamic or Character in the generated sibling JSON before the
        // next build; no hidden editor actor or category folder is required.
        RootObject->SetStringField(TEXT("ModelType"), TEXT("Static"));

        FString SerializedJson;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&SerializedJson);
        if (!FJsonSerializer::Serialize(RootObject, Writer))
        {
            UE_LOG(LogTemp, Error,
                TEXT("Failed to serialize an automatically generated model definition: %s"),
                *JsonPath);
            return false;
        }

        if (!FFileHelper::SaveStringToFile(
                SerializedJson,
                *TemporaryPath,
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            UE_LOG(LogTemp, Error,
                TEXT("Failed to write a temporary model definition: %s"),
                *TemporaryPath);
            return false;
        }

        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

        // Another scan or process may have created the sibling JSON after our
        // initial existence check. Preserve that file and discard our temp.
        if (PlatformFile.FileExists(*JsonPath))
        {
            PlatformFile.DeleteFile(*TemporaryPath);
            return true;
        }

        if (!PlatformFile.MoveFile(*JsonPath, *TemporaryPath))
        {
            const bool bCreatedByAnotherWriter = PlatformFile.FileExists(*JsonPath);
            PlatformFile.DeleteFile(*TemporaryPath);
            if (bCreatedByAnotherWriter)
            {
                return true;
            }

            UE_LOG(LogTemp, Error,
                TEXT("Failed to commit an automatically generated model definition: %s"),
                *JsonPath);
            return false;
        }

        UE_LOG(LogTemp, Display,
            TEXT("A GLB had no sibling JSON. Generated a ModelType=Static definition. GLB='%s', JSON='%s'"),
            *GlbPath,
            *JsonPath);
        return true;
    }
}

bool ModelDefinitionJson::EnsureMissingDefinitions(
    const FString& ModelRootDirectory,
    TArray<FString>* OutCreatedJsonFiles,
    TArray<FString>* OutDiscoveredGlbFiles)
{
    if (OutCreatedJsonFiles != nullptr)
    {
        OutCreatedJsonFiles->Reset();
    }
    if (OutDiscoveredGlbFiles != nullptr)
    {
        OutDiscoveredGlbFiles->Reset();
    }

    if (ModelRootDirectory.IsEmpty()
        || !IFileManager::Get().DirectoryExists(*ModelRootDirectory))
    {
        return false;
    }

    TArray<FString> GlbFiles;
    IFileManager::Get().FindFilesRecursive(
        GlbFiles,
        *ModelRootDirectory,
        // *.* is used instead of an extension wildcard so .glb, .GLB and mixed-case variants are
        // returned consistently by platform file implementations. The extension is filtered below.
        TEXT("*.*"),
        true,
        false,
        false);

    // Wildcard extension matching is case-sensitive on some packaged platforms. Enumerate the
    // authoring root once, then apply the extension contract explicitly and consistently.
    GlbFiles.RemoveAllSwap(
        [](const FString& Path)
        {
            return !FPaths::GetExtension(Path).Equals(TEXT("glb"), ESearchCase::IgnoreCase);
        },
        EAllowShrinking::No);

    // Normalize and de-duplicate before JSON generation. This exact ordered list is handed back to
    // ModelDatabaseSubsystem, so discovery and build cannot accidentally operate on different
    // recursive scans while an authoring tool is saving the resources tree.
    TSet<FString> SeenPaths;
    TArray<FString> NormalizedGlbFiles;
    NormalizedGlbFiles.Reserve(GlbFiles.Num());
    for (const FString& CandidatePath : GlbFiles)
    {
        FString Path = FSafeFileIO::NormalizeFilePath(CandidatePath);
        if (!Path.IsEmpty() && IFileManager::Get().FileExists(*Path)
            && !SeenPaths.Contains(Path))
        {
            SeenPaths.Add(Path);
            NormalizedGlbFiles.Add(MoveTemp(Path));
        }
    }
    GlbFiles = MoveTemp(NormalizedGlbFiles);
    constexpr int32 MaxGeneratedDefinitions = 100000; // Must not exceed .gwd's model cap.
    if (GlbFiles.Num() > MaxGeneratedDefinitions)
    {
        UE_LOG(LogTemp, Error,
            TEXT("Model definition generation rejected more than %d GLB files under %s"),
            MaxGeneratedDefinitions, *ModelRootDirectory);
        return false;
    }

    GlbFiles.Sort();
    if (OutDiscoveredGlbFiles != nullptr)
    {
        *OutDiscoveredGlbFiles = GlbFiles;
    }
    bool bAllWritesSucceeded = true;
    for (const FString& GlbPath : GlbFiles)
    {
        const FString JsonPath = FPaths::ChangeExtension(GlbPath, TEXT("json"));
        if (IFileManager::Get().FileExists(*JsonPath))
        {
            continue;
        }

        if (WriteGeneratedDefinitionAtomically(GlbPath, JsonPath))
        {
            if (OutCreatedJsonFiles != nullptr)
            {
                OutCreatedJsonFiles->Add(JsonPath);
            }
        }
        else
        {
            bAllWritesSucceeded = false;
        }
    }

    return bAllWritesSucceeded;
}

bool ModelDefinitionJson::IsLoadableModelType(const FString& ModelType)
{
    return ModelType.Equals(TEXT("Static"), ESearchCase::CaseSensitive)
        || ModelType.Equals(TEXT("Dynamic"), ESearchCase::CaseSensitive)
        || ModelType.Equals(TEXT("Character"), ESearchCase::CaseSensitive)
        // Legacy authoring values are accepted only so old projects can be rebuilt.
        || ModelType.Equals(TEXT("Scene"), ESearchCase::CaseSensitive)
        || ModelType.Equals(TEXT("Entity"), ESearchCase::CaseSensitive)
        || ModelType.Equals(TEXT("Item"), ESearchCase::CaseSensitive);
}

FString ModelDefinitionJson::ModelTypeToString(const EModelDefinitionType Type)
{
    switch (Type)
    {
    case EModelDefinitionType::Static: return TEXT("Static");
    case EModelDefinitionType::Dynamic: return TEXT("Dynamic");
    case EModelDefinitionType::Character: return TEXT("Character");
    default: return TEXT("Invalid");
    }
}

bool ModelDefinitionJson::LoadDefinition(
    const FString& JsonPath,
    const FString& ExpectedGlbPath,
    FModelDefinition& OutDefinition,
    FString& OutError,
    FString* OutCanonicalJson)
{
    OutDefinition = FModelDefinition();
    OutError.Reset();
    if (OutCanonicalJson) OutCanonicalJson->Reset();

    FSafeJsonLimits Limits;
    Limits.MaxFileBytes = 16ll * 1024ll * 1024ll;
    Limits.MaxDepth = 32;
    Limits.MaxValues = 131072;
    Limits.MaxContainerEntries = 65536;
    Limits.MaxStringCharacters = 32768;
    Limits.bAllowBackupRecovery = false;
    const FSafeJsonLoadResult Loaded = FSafeFileIO::LoadJsonBlocking(JsonPath, Limits);
    if (!Loaded.IsSuccess() || !Loaded.JsonObject.IsValid())
    {
        OutError = Loaded.Error.IsEmpty() ? TEXT("JSON document is invalid") : Loaded.Error;
        return false;
    }

    const TSharedPtr<FJsonObject>& Root = Loaded.JsonObject;
    FString UUIDText;
    FString TypeText;
    if (!Root->TryGetStringField(TEXT("UUID"), UUIDText) || !FGuid::Parse(UUIDText, OutDefinition.UUID))
    {
        OutError = TEXT("required UUID is missing or is not a valid UUID");
        return false;
    }
    if (!Root->TryGetStringField(TEXT("Name"), OutDefinition.Name) || OutDefinition.Name.TrimStartAndEnd().IsEmpty()
        || !Root->TryGetStringField(TEXT("DisplayName"), OutDefinition.DisplayName) || OutDefinition.DisplayName.TrimStartAndEnd().IsEmpty())
    {
        OutError = TEXT("required Name or DisplayName field is missing/empty");
        return false;
    }
    OutDefinition.Name.TrimStartAndEndInline();
    OutDefinition.DisplayName.TrimStartAndEndInline();
    if (FPaths::GetCleanFilename(OutDefinition.Name) != OutDefinition.Name
        || OutDefinition.Name == TEXT(".") || OutDefinition.Name == TEXT("..")
        || OutDefinition.Name.Contains(TEXT("/")) || OutDefinition.Name.Contains(TEXT("\\")))
    {
        OutError = TEXT("Name must be a single safe identifier, without path separators");
        return false;
    }

    Root->TryGetStringField(TEXT("ModelType"), TypeText);
    TypeText.TrimStartAndEndInline();
    // New authoring schema has only Static, Dynamic and Character. Legacy Scene/Entity/Item
    // remain readable so an existing project can be rebuilt without a destructive migration.
    const FString AuthoredType = TypeText;
    if (TypeText.IsEmpty() || TypeText == TEXT("None") || TypeText == TEXT("Scene"))
    {
        TypeText = TEXT("Static");
    }
    else if (TypeText == TEXT("Entity") || TypeText == TEXT("Item"))
    {
        TypeText = TEXT("Dynamic");
    }
    Root->SetStringField(TEXT("ModelType"), TypeText);

    if (TypeText == TEXT("Static")) OutDefinition.ModelType = EModelDefinitionType::Static;
    else if (TypeText == TEXT("Dynamic")) OutDefinition.ModelType = EModelDefinitionType::Dynamic;
    else if (TypeText == TEXT("Character")) OutDefinition.ModelType = EModelDefinitionType::Character;
    else
    {
        OutError = FString::Printf(
            TEXT("unsupported or case-mismatched ModelType '%s'; expected Static, Dynamic, or Character"),
            *TypeText);
        return false;
    }

    const bool bLegacyEntity = AuthoredType == TEXT("Entity");
    const bool bLegacyItem = AuthoredType == TEXT("Item");
    if (OutDefinition.ModelType == EModelDefinitionType::Dynamic)
    {
        FString EntitySubtype;
        FString ItemSubtype;
        const bool bHasEntityType = Root->TryGetStringField(TEXT("EntityType"), EntitySubtype);
        const bool bHasItemType = Root->TryGetStringField(TEXT("ItemType"), ItemSubtype);
        if (bHasEntityType && bHasItemType)
        {
            OutError = TEXT("Dynamic cannot specify both EntityType and ItemType");
            return false;
        }
        if (bLegacyEntity && !bHasEntityType)
        {
            OutError = TEXT("legacy Entity requires EntityType=Prop, Vehicle, or Animal");
            return false;
        }
        if (bLegacyItem && !bHasItemType)
        {
            OutError = TEXT("legacy Item requires ItemType=Weapon, Tool, or Misc");
            return false;
        }
        if (bHasEntityType)
        {
            if (EntitySubtype == TEXT("Vehicle")) OutDefinition.EntityType = EModelEntityType::Vehicle;
            else if (EntitySubtype == TEXT("Prop")) OutDefinition.EntityType = EModelEntityType::Prop;
            else if (EntitySubtype == TEXT("Animal")) OutDefinition.EntityType = EModelEntityType::Animal;
            else
            {
                OutError = FString::Printf(TEXT("unsupported EntityType '%s'"), *EntitySubtype);
                return false;
            }
        }
        if (bHasItemType)
        {
            if (ItemSubtype == TEXT("Weapon")) OutDefinition.ItemType = EModelItemType::Weapon;
            else if (ItemSubtype == TEXT("Tool")) OutDefinition.ItemType = EModelItemType::Tool;
            else if (ItemSubtype == TEXT("Misc")) OutDefinition.ItemType = EModelItemType::Misc;
            else
            {
                OutError = FString::Printf(TEXT("unsupported ItemType '%s'"), *ItemSubtype);
                return false;
            }
        }
    }
    else if (Root->HasField(TEXT("EntityType")) || Root->HasField(TEXT("ItemType")))
    {
        OutError = TEXT("EntityType and ItemType are only valid when ModelType is Dynamic");
        return false;
    }

    if (OutDefinition.ModelType == EModelDefinitionType::Character)
    {
        const TSharedPtr<FJsonObject>* BonesObject = nullptr;
        if (!Root->TryGetObjectField(TEXT("Bones"), BonesObject)
            || BonesObject == nullptr
            || !BonesObject->IsValid())
        {
            OutError = TEXT("Character requires a Bones object.");
            return false;
        }

        // The disk schema is canonical-key -> source-bone-name. Canonical keys are fixed and
        // source values are the only author-controlled part. glTFRuntime expects the inverse
        // source -> canonical alias map, so conversion happens only after strict validation.
        if (!CharacterBoneSchema::BuildSourceToCanonicalMap(
                *BonesObject, OutDefinition.Bones, OutError))
        {
            return false;
        }
    }

    OutDefinition.GlbPath = FSafeFileIO::NormalizeFilePath(ExpectedGlbPath);
    OutDefinition.JsonPath = FSafeFileIO::NormalizeFilePath(JsonPath);

    if (OutCanonicalJson)
    {
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(
                OutCanonicalJson);
        if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
        {
            OutCanonicalJson->Reset();
            OutError = TEXT("validated model definition could not be serialized for the archive");
            OutDefinition = FModelDefinition();
            return false;
        }
    }
    return true;
}
