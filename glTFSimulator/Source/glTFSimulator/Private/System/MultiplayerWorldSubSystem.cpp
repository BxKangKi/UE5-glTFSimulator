// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/MultiplayerWorldSubSystem.h"
#include "System/GameManagerSubSystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

namespace
{
    bool IsReturningToWorldSelection(const UObject* WorldContextObject)
    {
        if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(WorldContextObject))
        {
            if (Manager->ShouldOpenWorldSelectionMenuOnNextMainWorld())
            {
                UE_LOG(LogTemp, Display,
                    TEXT("[WorldSelection] Ignored stale gameplay-world travel while returning to world selection."));
                return true;
            }
        }
        return false;
    }
}

UMultiplayerWorldSubSystem* UMultiplayerWorldSubSystem::Get(const UObject* WorldContextObject)
{
    if (!IsValid(WorldContextObject))
    {
        return nullptr;
    }

    if (const UGameInstance* GameInstance = Cast<UGameInstance>(WorldContextObject))
    {
        return const_cast<UGameInstance*>(GameInstance)->GetSubsystem<UMultiplayerWorldSubSystem>();
    }

    if (const UGameInstanceSubsystem* GameInstanceSubsystem = Cast<UGameInstanceSubsystem>(WorldContextObject))
    {
        if (UGameInstance* GameInstance = GameInstanceSubsystem->GetGameInstance())
        {
            return GameInstance->GetSubsystem<UMultiplayerWorldSubSystem>();
        }
    }

    UWorld* World = WorldContextObject->GetWorld();
    if (!World)
    {
        return nullptr;
    }

    UGameInstance* GameInstance = World->GetGameInstance();
    return GameInstance ? GameInstance->GetSubsystem<UMultiplayerWorldSubSystem>() : nullptr;
}

bool UMultiplayerWorldSubSystem::OpenWorldByReference(
    const UObject* WorldContextObject,
    TSoftObjectPtr<UWorld> WorldAsset,
    const FString& Options) const
{
    if (!IsValid(WorldContextObject) || WorldAsset.IsNull())
    {
        return false;
    }

    if (IsReturningToWorldSelection(WorldContextObject))
    {
        return false;
    }

    UE_LOG(LogTemp, Display,
        TEXT("[WorldSelection] Opening a directly assigned world reference. Options=%s"),
        Options.IsEmpty() ? TEXT("<none>") : *Options);

    UGameplayStatics::OpenLevelBySoftObjectPtr(WorldContextObject, WorldAsset, true, Options);
    return true;
}

bool UMultiplayerWorldSubSystem::StartSinglePlayerWorld(
    const UObject* WorldContextObject,
    const FString& WorldFolderName,
    TSoftObjectPtr<UWorld> SinglePlayerWorld)
{
    if (IsReturningToWorldSelection(WorldContextObject))
    {
        return false;
    }

    WorldMode = EMultiplayerWorldMode::SinglePlayer;
    SelectedWorldFolderName = WorldFolderName;

    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(WorldContextObject))
    {
        Manager->SetCurrentWorldName(WorldFolderName);
    }

    FString Options;
    if (!WorldFolderName.IsEmpty())
    {
        // GameManagerSubSystem reads this option in the destination map. This is required because
        // the old world's shutdown can clear transient references during OpenLevel.
        Options = FString::Printf(TEXT("World=%s"), *WorldFolderName);
    }

    return OpenWorldByReference(WorldContextObject, SinglePlayerWorld, Options);
}

bool UMultiplayerWorldSubSystem::HostMultiplayerWorld(
    const UObject* WorldContextObject,
    const FString& WorldFolderName,
    TSoftObjectPtr<UWorld> HostWorld,
    int32 Port)
{
    if (IsReturningToWorldSelection(WorldContextObject))
    {
        return false;
    }

    WorldMode = EMultiplayerWorldMode::Host;
    SelectedWorldFolderName = WorldFolderName;

    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(WorldContextObject))
    {
        Manager->SetCurrentWorldName(WorldFolderName);
    }

    FString Options(TEXT("listen"));
    if (!WorldFolderName.IsEmpty())
    {
        Options += FString::Printf(TEXT("?World=%s"), *WorldFolderName);
    }
    if (Port > 0)
    {
        Options += FString::Printf(TEXT("?Port=%d"), Port);
    }

    return OpenWorldByReference(WorldContextObject, HostWorld, Options);
}

bool UMultiplayerWorldSubSystem::OpenClientConnectionWorld(
    const UObject* WorldContextObject,
    TSoftObjectPtr<UWorld> ClientWorld)
{
    if (IsReturningToWorldSelection(WorldContextObject))
    {
        return false;
    }

    WorldMode = EMultiplayerWorldMode::Client;
    return OpenWorldByReference(WorldContextObject, ClientWorld);
}

bool UMultiplayerWorldSubSystem::JoinMultiplayerWorld(const UObject* WorldContextObject, const FString& InServerAddress, const FString& WorldFolderName)
{
    if (!IsValid(WorldContextObject) || IsReturningToWorldSelection(WorldContextObject))
    {
        return false;
    }

    WorldMode = EMultiplayerWorldMode::Client;
    if (!WorldFolderName.IsEmpty())
    {
        SelectedWorldFolderName = WorldFolderName;
        if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(WorldContextObject))
        {
            Manager->SetCurrentWorldName(WorldFolderName);
        }
    }

    ServerAddress = InServerAddress.IsEmpty() ? ServerAddress : InServerAddress;
    if (ServerAddress.IsEmpty())
    {
        ServerAddress = TEXT("127.0.0.1:7777");
    }

    UWorld* World = WorldContextObject->GetWorld();
    APlayerController* PlayerController = World ? UGameplayStatics::GetPlayerController(WorldContextObject, 0) : nullptr;
    if (!PlayerController)
    {
        return false;
    }

    FString TravelAddress = ServerAddress;
    if (!SelectedWorldFolderName.IsEmpty() && !TravelAddress.Contains(TEXT("?World=")))
    {
        TravelAddress += FString::Printf(TEXT("?World=%s"), *SelectedWorldFolderName);
    }

    PlayerController->ClientTravel(TravelAddress, TRAVEL_Absolute);
    return true;
}

bool UMultiplayerWorldSubSystem::ShouldRunServerAuthority(const UObject* WorldContextObject) const
{
    const UWorld* World = IsValid(WorldContextObject) ? WorldContextObject->GetWorld() : nullptr;
    if (!World)
    {
        return WorldMode != EMultiplayerWorldMode::Client;
    }

    return World->GetNetMode() != NM_Client;
}

bool UMultiplayerWorldSubSystem::ShouldUseClientRenderOnlyStreaming(const UObject* WorldContextObject)
{
    UMultiplayerWorldSubSystem* Subsystem = Get(WorldContextObject);
    const UWorld* World = IsValid(WorldContextObject) ? WorldContextObject->GetWorld() : nullptr;
    const bool bIsNetworkClient = World && World->GetNetMode() == NM_Client;
    return bIsNetworkClient || (Subsystem && Subsystem->WorldMode == EMultiplayerWorldMode::Client);
}
