#include "RuntimeFramework/SimulatorGlTFRuntimeCacheLibrary.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "JsonObjectConverter.h"
#include "Misc/Paths.h"

namespace SimulatorGlTFRuntimeCache
{
    static FString ResolveFingerprintPath(const FString& Filename, const bool bPathRelativeToContent)
    {
        const FString InputPath = bPathRelativeToContent
            ? FPaths::Combine(FPaths::ProjectContentDir(), Filename)
            : Filename;
        return FPaths::ConvertRelativePathToFull(InputPath);
    }
}

UglTFRuntimeAsset* USimulatorGlTFRuntimeCacheLibrary::LoadSharedAssetFromFilename(UObject* WorldContextObject,
    const FString& Filename,
        const bool bPathRelativeToContent,
        const FglTFRuntimeConfig& LoaderConfig)
{
    if (!IsValid(WorldContextObject)) return nullptr;
    UWorld* World = WorldContextObject->GetWorld();
    UGameInstance* GameInstance = IsValid(World) ? World->GetGameInstance() : nullptr;
    USimulatorSharedResourceSubsystem* Cache = IsValid(GameInstance) ? GameInstance->GetSubsystem<USimulatorSharedResourceSubsystem>() : nullptr;
    if (!IsValid(Cache)) return nullptr;
    FString LoaderVariant;
    FJsonObjectConverter::UStructToJsonObjectString(FglTFRuntimeConfig::StaticStruct(), &LoaderConfig, LoaderVariant, 0, 0);
    LoaderVariant += bPathRelativeToContent ? TEXT("|1") : TEXT("|0");
    FString Error;
    UObject* SharedObject = Cache->AcquireSourceAssetSyncForOwner(SimulatorGlTFRuntimeCache::ResolveFingerprintPath(Filename, bPathRelativeToContent), WorldContextObject,
        [Filename, bPathRelativeToContent, LoaderConfig]() -> UObject* { return UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(Filename, bPathRelativeToContent, LoaderConfig); }, Error, LoaderVariant);
    return Cast<UglTFRuntimeAsset>(SharedObject);
}

UglTFRuntimeAsset* USimulatorGlTFRuntimeCacheLibrary::LoadSharedAssetFromFilenameWithLease(UObject* WorldContextObject,
    const FString& Filename,
        const bool bPathRelativeToContent,
        const FglTFRuntimeConfig& LoaderConfig, FSimulatorResourceLease& OutLease, FString& OutError)
{
    OutLease = {};
    if (!IsValid(WorldContextObject)) { OutError = TEXT("World context is invalid"); return nullptr; }
    UWorld* World = WorldContextObject->GetWorld(); UGameInstance* GameInstance = IsValid(World) ? World->GetGameInstance() : nullptr;
    USimulatorSharedResourceSubsystem* Cache = IsValid(GameInstance) ? GameInstance->GetSubsystem<USimulatorSharedResourceSubsystem>() : nullptr;
    if (!IsValid(Cache)) { OutError = TEXT("Shared resource subsystem is unavailable"); return nullptr; }
    FString LoaderVariant;
    FJsonObjectConverter::UStructToJsonObjectString(FglTFRuntimeConfig::StaticStruct(), &LoaderConfig, LoaderVariant, 0, 0);
    LoaderVariant += bPathRelativeToContent ? TEXT("|1") : TEXT("|0");
    UObject* SharedObject = nullptr;
    OutLease = Cache->AcquireSourceAssetSync(SimulatorGlTFRuntimeCache::ResolveFingerprintPath(Filename, bPathRelativeToContent),
        [Filename, bPathRelativeToContent, LoaderConfig]() -> UObject* { return UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(Filename, bPathRelativeToContent, LoaderConfig); }, SharedObject, OutError, LoaderVariant);
    return Cast<UglTFRuntimeAsset>(SharedObject);
}
