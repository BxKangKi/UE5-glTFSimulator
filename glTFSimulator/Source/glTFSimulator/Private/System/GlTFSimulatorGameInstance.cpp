// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/GlTFSimulatorGameInstance.h"
#include "System/GlTFSimulatorAssetRegistry.h"
#include "Engine/World.h"

void UGlTFSimulatorGameInstance::Init()
{
    Super::Init();
    EnsureAssetRegistry();
}

bool UGlTFSimulatorGameInstance::EnsureAssetRegistry()
{
    if (IsValid(RuntimeAssetRegistry))
    {
        return true;
    }

    RuntimeAssetRegistry = nullptr;
    if (AssetRegistryClass.IsNull())
    {
        UE_LOG(LogTemp, Error,
            TEXT("GlTFSimulatorGameInstance has no AssetRegistryClass. Assign a Blueprint subclass of UGlTFSimulatorAssetRegistry in the GameInstance defaults."));
        return false;
    }

    UClass* RegistryClass = AssetRegistryClass.LoadSynchronous();
    if (!IsValid(RegistryClass)
        || !RegistryClass->IsChildOf(UGlTFSimulatorAssetRegistry::StaticClass())
        || RegistryClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
    {
        UE_LOG(LogTemp, Error,
            TEXT("GlTFSimulatorGameInstance AssetRegistryClass is invalid or cannot be instantiated. Class=%s"),
            *GetNameSafe(RegistryClass));
        return false;
    }

    RuntimeAssetRegistry = NewObject<UGlTFSimulatorAssetRegistry>(
        this, RegistryClass, NAME_None, RF_Transient);
    if (!IsValid(RuntimeAssetRegistry))
    {
        UE_LOG(LogTemp, Error,
            TEXT("GlTFSimulatorGameInstance failed to create the private runtime asset registry from class %s."),
            *GetNameSafe(RegistryClass));
        return false;
    }

    UE_LOG(LogTemp, Display,
        TEXT("glTFSimulator asset registry initialized from class %s."),
        *GetNameSafe(RegistryClass));
    return true;
}

UGlTFSimulatorAssetRegistry* UGlTFSimulatorGameInstance::GetAssetRegistry() const
{
    return IsValid(RuntimeAssetRegistry) ? RuntimeAssetRegistry.Get() : nullptr;
}

UGlTFSimulatorAssetRegistry* UGlTFSimulatorGameInstance::GetAssetRegistryFromContext(const UObject* WorldContextObject)
{
    if (!IsValid(WorldContextObject))
    {
        return nullptr;
    }
    const UWorld* World = WorldContextObject->GetWorld();
    UGameInstance* GameInstance = World ? World->GetGameInstance() : Cast<UGameInstance>(const_cast<UObject*>(WorldContextObject));
    if (UGlTFSimulatorGameInstance* SimulatorGameInstance = Cast<UGlTFSimulatorGameInstance>(GameInstance))
    {
        // Init normally creates this before any world starts. The fallback makes direct/editor world
        // startup robust if a custom lifecycle calls into the registry unusually early.
        SimulatorGameInstance->EnsureAssetRegistry();
        return SimulatorGameInstance->GetAssetRegistry();
    }
    return nullptr;
}
