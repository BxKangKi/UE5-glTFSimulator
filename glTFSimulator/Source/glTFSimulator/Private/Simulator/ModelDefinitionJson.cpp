#include "Simulator/ModelDefinitionJson.h"

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
        RootObject->SetStringField(TEXT("ID"),
            FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
        RootObject->SetStringField(TEXT("Name"), BaseName);
        RootObject->SetStringField(TEXT("DisplayName"), BaseName);
        RootObject->SetStringField(TEXT("ModelType"), TEXT("None"));

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

        UE_LOG(LogTemp, Warning,
            TEXT("A GLB had no sibling JSON. Generated a non-loadable ModelType=None definition. GLB='%s', JSON='%s'"),
            *GlbPath,
            *JsonPath);
        return true;
    }
}

bool ModelDefinitionJson::EnsureMissingDefinitions(
    const FString& ModelRootDirectory,
    TArray<FString>* OutCreatedJsonFiles)
{
    if (OutCreatedJsonFiles != nullptr)
    {
        OutCreatedJsonFiles->Reset();
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
        TEXT("*.glb"),
        true,
        false,
        false);

    GlbFiles.Sort();
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
    return ModelType.Equals(TEXT("Scene"), ESearchCase::CaseSensitive)
        || ModelType.Equals(TEXT("Prefab"), ESearchCase::CaseSensitive)
        || ModelType.Equals(TEXT("Item"), ESearchCase::CaseSensitive)
        || ModelType.Equals(TEXT("Character"), ESearchCase::CaseSensitive)
        || ModelType.Equals(TEXT("Entity"), ESearchCase::CaseSensitive);
}

FString ModelDefinitionJson::ModelTypeToString(const EModelDefinitionType Type)
{
    switch (Type)
    {
    case EModelDefinitionType::Scene: return TEXT("Scene");
    case EModelDefinitionType::Prefab: return TEXT("Prefab");
    case EModelDefinitionType::Item: return TEXT("Item");
    case EModelDefinitionType::Character: return TEXT("Character");
    case EModelDefinitionType::Entity: return TEXT("Entity");
    default: return TEXT("None");
    }
}

bool ModelDefinitionJson::LoadDefinition(
    const FString& JsonPath,
    const FString& ExpectedGlbPath,
    FModelDefinition& OutDefinition,
    FString& OutError)
{
    OutDefinition = FModelDefinition();
    OutError.Reset();

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
    FString IdText;
    FString TypeText;
    if (!Root->TryGetStringField(TEXT("ID"), IdText) || !FGuid::Parse(IdText, OutDefinition.Id))
    {
        OutError = TEXT("required ID is missing or is not a UUID");
        return false;
    }
    if (!Root->TryGetStringField(TEXT("Name"), OutDefinition.Name) || OutDefinition.Name.TrimStartAndEnd().IsEmpty()
        || !Root->TryGetStringField(TEXT("DisplayName"), OutDefinition.DisplayName) || OutDefinition.DisplayName.TrimStartAndEnd().IsEmpty()
        || !Root->TryGetStringField(TEXT("ModelType"), TypeText))
    {
        OutError = TEXT("required Name, DisplayName, or ModelType field is missing/empty");
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

    if (TypeText == TEXT("None")) OutDefinition.ModelType = EModelDefinitionType::None;
    else if (TypeText == TEXT("Scene")) OutDefinition.ModelType = EModelDefinitionType::Scene;
    else if (TypeText == TEXT("Prefab")) OutDefinition.ModelType = EModelDefinitionType::Prefab;
    else if (TypeText == TEXT("Item")) OutDefinition.ModelType = EModelDefinitionType::Item;
    else if (TypeText == TEXT("Character")) OutDefinition.ModelType = EModelDefinitionType::Character;
    else if (TypeText == TEXT("Entity")) OutDefinition.ModelType = EModelDefinitionType::Entity;
    else
    {
        OutError = FString::Printf(TEXT("unknown or case-mismatched ModelType '%s'"), *TypeText);
        return false;
    }

    if (OutDefinition.ModelType == EModelDefinitionType::Entity)
    {
        FString Subtype;
        if (!Root->TryGetStringField(TEXT("EntityType"), Subtype))
        {
            OutError = TEXT("Entity requires EntityType=Prop, Vehicle, or Animal");
            return false;
        }
        if (Subtype == TEXT("Vehicle")) OutDefinition.EntityType = EModelEntityType::Vehicle;
        else if (Subtype == TEXT("Prop")) OutDefinition.EntityType = EModelEntityType::Prop;
        else if (Subtype == TEXT("Animal")) OutDefinition.EntityType = EModelEntityType::Animal;
        else
        {
            OutError = FString::Printf(TEXT("unsupported EntityType '%s'"), *Subtype);
            return false;
        }
    }
    else if (Root->HasField(TEXT("EntityType")))
    {
        OutError = TEXT("EntityType is only valid when ModelType is Entity");
        return false;
    }

    if (OutDefinition.ModelType == EModelDefinitionType::Item)
    {
        FString Subtype;
        if (!Root->TryGetStringField(TEXT("ItemType"), Subtype))
        {
            OutError = TEXT("Item requires ItemType=Weapon, Tool, or Misc");
            return false;
        }
        if (Subtype == TEXT("Weapon")) OutDefinition.ItemType = EModelItemType::Weapon;
        else if (Subtype == TEXT("Tool")) OutDefinition.ItemType = EModelItemType::Tool;
        else if (Subtype == TEXT("Misc")) OutDefinition.ItemType = EModelItemType::Misc;
        else
        {
            OutError = FString::Printf(TEXT("unsupported ItemType '%s'"), *Subtype);
            return false;
        }
    }
    else if (Root->HasField(TEXT("ItemType")))
    {
        OutError = TEXT("ItemType is only valid when ModelType is Item");
        return false;
    }

    if (OutDefinition.ModelType == EModelDefinitionType::Character)
    {
        const TSharedPtr<FJsonObject>* BonesObject = nullptr;
        if (!Root->TryGetObjectField(TEXT("Bones"), BonesObject) || !BonesObject || !BonesObject->IsValid())
        {
            OutError = TEXT("Character requires a Bones object");
            return false;
        }
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*BonesObject)->Values)
        {
            FString Bone;
            if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(Bone)
                || Pair.Key.TrimStartAndEnd().IsEmpty() || Bone.TrimStartAndEnd().IsEmpty())
            {
                OutError = TEXT("Bones must contain only non-empty string-to-string mappings");
                return false;
            }
            OutDefinition.Bones.Add(Bone.TrimStartAndEnd(), Pair.Key.TrimStartAndEnd());
        }
    }

    OutDefinition.GlbPath = FSafeFileIO::NormalizeFilePath(ExpectedGlbPath);
    OutDefinition.JsonPath = FSafeFileIO::NormalizeFilePath(JsonPath);
    return true;
}
