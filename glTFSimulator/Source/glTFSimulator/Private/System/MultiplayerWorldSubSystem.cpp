// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/MultiplayerWorldSubSystem.h"
#include "System/GameManagerSubSystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

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

bool UMultiplayerWorldSubSystem::OpenLevelByName(const UObject* WorldContextObject, FName LevelName, const FString& Options) const
{
    if (!IsValid(WorldContextObject) || LevelName == NAME_None)
    {
        return false;
    }

    UGameplayStatics::OpenLevel(WorldContextObject, LevelName, true, Options);
    return true;
}

bool UMultiplayerWorldSubSystem::StartSinglePlayerWorld(const UObject* WorldContextObject, const FString& WorldFolderName, FName SingleWorldLevelName)
{
    WorldMode = EMultiplayerWorldMode::SinglePlayer;
    SelectedWorldFolderName = WorldFolderName;

    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(WorldContextObject))
    {
        Manager->SetCurrentWorldName(WorldFolderName);
    }

    return OpenLevelByName(WorldContextObject, SingleWorldLevelName != NAME_None ? SingleWorldLevelName : FName(TEXT("SingleWorld")));
}

bool UMultiplayerWorldSubSystem::HostMultiplayerWorld(const UObject* WorldContextObject, const FString& WorldFolderName, FName HostWorldLevelName, int32 Port)
{
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

    return OpenLevelByName(WorldContextObject, HostWorldLevelName != NAME_None ? HostWorldLevelName : FName(TEXT("HostWorld")), Options);
}

bool UMultiplayerWorldSubSystem::OpenClientConnectionWorld(const UObject* WorldContextObject, FName ClientWorldLevelName)
{
    WorldMode = EMultiplayerWorldMode::Client;
    return OpenLevelByName(WorldContextObject, ClientWorldLevelName != NAME_None ? ClientWorldLevelName : FName(TEXT("ClientWorld")));
}

bool UMultiplayerWorldSubSystem::JoinMultiplayerWorld(const UObject* WorldContextObject, const FString& InServerAddress, const FString& WorldFolderName)
{
    if (!IsValid(WorldContextObject))
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
