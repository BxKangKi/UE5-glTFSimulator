// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/MultiplayerWorldSubSystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "System/GameManagerSubSystem.h"

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

    /** Adds one URL option without carrying options from the previous map. */
    void AppendTravelOption(FString& InOutOptions, const FString& Key, const FString& Value)
    {
        if (Key.IsEmpty() || Value.IsEmpty())
        {
            return;
        }

        if (!InOutOptions.IsEmpty())
        {
            InOutOptions += TEXT("?");
        }
        InOutOptions += Key;
        InOutOptions += TEXT("=");
        InOutOptions += Value;
    }

    /** Converts a directly assigned soft class into the URL value understood by Unreal travel. */
    bool AppendGameModeOption(FString& InOutOptions, TSoftClassPtr<AGameModeBase> GameModeOverride)
    {
        if (GameModeOverride.IsNull())
        {
            return true;
        }

        const FString GameModeClassPath = GameModeOverride.ToSoftObjectPath().ToString();
        if (GameModeClassPath.IsEmpty())
        {
            UE_LOG(LogTemp, Error,
                TEXT("[GameModeTravel] An explicit GameMode was assigned, but its soft class path is invalid."));
            return false;
        }

        AppendTravelOption(InOutOptions, TEXT("game"), GameModeClassPath);
        return true;
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

void UMultiplayerWorldSubSystem::ClearRequestedGameModeOverride()
{
    RequestedGameModeOverride = TSoftClassPtr<AGameModeBase>();
    RequestedGameModeWorldFolder.Reset();
}

bool UMultiplayerWorldSubSystem::OpenWorldByReference(
    const UObject* WorldContextObject,
    TSoftObjectPtr<UWorld> WorldAsset,
    const FString& Options,
    TSoftClassPtr<AGameModeBase> GameModeOverride) const
{
    if (!IsValid(WorldContextObject) || WorldAsset.IsNull())
    {
        return false;
    }

    if (IsReturningToWorldSelection(WorldContextObject))
    {
        return false;
    }

    FString FinalOptions = Options;
    if (!AppendGameModeOption(FinalOptions, GameModeOverride))
    {
        return false;
    }

    const FString WorldPath = WorldAsset.ToSoftObjectPath().ToString();
    const FString GameModePath = GameModeOverride.IsNull()
        ? FString(TEXT("<map World Settings>"))
        : GameModeOverride.ToSoftObjectPath().ToString();

    UE_LOG(LogTemp, Display,
        TEXT("[GameModeTravel] Open world. World=%s GameMode=%s Options=%s"),
        WorldPath.IsEmpty() ? TEXT("<invalid>") : *WorldPath,
        *GameModePath,
        FinalOptions.IsEmpty() ? TEXT("<none>") : *FinalOptions);

    // Absolute travel deliberately drops the previous map's URL options. This prevents an old
    // ?game= value from leaking into a later world that expects its own World Settings override.
    UGameplayStatics::OpenLevelBySoftObjectPtr(WorldContextObject, WorldAsset, true, FinalOptions);
    return true;
}

bool UMultiplayerWorldSubSystem::StartSinglePlayerWorld(
    const UObject* WorldContextObject,
    const FString& WorldFolderName,
    TSoftObjectPtr<UWorld> SinglePlayerWorld)
{
    return StartSinglePlayerWorldWithGameMode(
        WorldContextObject,
        WorldFolderName,
        SinglePlayerWorld,
        TSoftClassPtr<AGameModeBase>());
}

bool UMultiplayerWorldSubSystem::StartSinglePlayerWorldWithGameMode(
    const UObject* WorldContextObject,
    const FString& WorldFolderName,
    TSoftObjectPtr<UWorld> SinglePlayerWorld,
    TSoftClassPtr<AGameModeBase> GameModeOverride)
{
    if (IsReturningToWorldSelection(WorldContextObject))
    {
        return false;
    }

    WorldMode = EMultiplayerWorldMode::SinglePlayer;
    SelectedWorldFolderName = WorldFolderName;
    RequestedGameModeOverride = GameModeOverride;
    RequestedGameModeWorldFolder = WorldFolderName;

    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(WorldContextObject))
    {
        Manager->SetCurrentWorldName(WorldFolderName);
    }

    FString Options;
    if (!WorldFolderName.IsEmpty())
    {
        // GameManagerSubSystem reads this option in the destination map. This is required because
        // the old world's shutdown can clear transient references during OpenLevel.
        AppendTravelOption(Options, TEXT("World"), WorldFolderName);
    }

    const bool bOpened = OpenWorldByReference(
        WorldContextObject,
        SinglePlayerWorld,
        Options,
        GameModeOverride);
    if (!bOpened)
    {
        ClearRequestedGameModeOverride();
    }
    return bOpened;
}

bool UMultiplayerWorldSubSystem::HostMultiplayerWorld(
    const UObject* WorldContextObject,
    const FString& WorldFolderName,
    TSoftObjectPtr<UWorld> HostWorld,
    int32 Port)
{
    return HostMultiplayerWorldWithGameMode(
        WorldContextObject,
        WorldFolderName,
        HostWorld,
        TSoftClassPtr<AGameModeBase>(),
        Port);
}

bool UMultiplayerWorldSubSystem::HostMultiplayerWorldWithGameMode(
    const UObject* WorldContextObject,
    const FString& WorldFolderName,
    TSoftObjectPtr<UWorld> HostWorld,
    TSoftClassPtr<AGameModeBase> GameModeOverride,
    int32 Port)
{
    if (IsReturningToWorldSelection(WorldContextObject))
    {
        return false;
    }

    WorldMode = EMultiplayerWorldMode::Host;
    SelectedWorldFolderName = WorldFolderName;
    RequestedGameModeOverride = GameModeOverride;
    RequestedGameModeWorldFolder = WorldFolderName;

    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(WorldContextObject))
    {
        Manager->SetCurrentWorldName(WorldFolderName);
    }

    FString Options(TEXT("listen"));
    if (!WorldFolderName.IsEmpty())
    {
        AppendTravelOption(Options, TEXT("World"), WorldFolderName);
    }
    if (Port > 0)
    {
        AppendTravelOption(Options, TEXT("Port"), FString::FromInt(Port));
    }

    const bool bOpened = OpenWorldByReference(
        WorldContextObject,
        HostWorld,
        Options,
        GameModeOverride);
    if (!bOpened)
    {
        ClearRequestedGameModeOverride();
    }
    return bOpened;
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
    ClearRequestedGameModeOverride();
    return OpenWorldByReference(
        WorldContextObject,
        ClientWorld,
        FString(),
        TSoftClassPtr<AGameModeBase>());
}

bool UMultiplayerWorldSubSystem::JoinMultiplayerWorld(
    const UObject* WorldContextObject,
    const FString& InServerAddress,
    const FString& WorldFolderName)
{
    if (!IsValid(WorldContextObject) || IsReturningToWorldSelection(WorldContextObject))
    {
        return false;
    }

    // A client does not select the authoritative GameMode. The server's active map/travel URL does.
    WorldMode = EMultiplayerWorldMode::Client;
    ClearRequestedGameModeOverride();
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
