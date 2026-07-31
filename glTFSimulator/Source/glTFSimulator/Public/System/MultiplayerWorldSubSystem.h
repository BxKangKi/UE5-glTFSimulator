// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "MultiplayerWorldSubSystem.generated.h"

class UWorld;

UENUM(BlueprintType)
enum class EMultiplayerWorldMode : uint8
{
    SinglePlayer UMETA(DisplayName="Single Player"),
    Host UMETA(DisplayName="Host"),
    Client UMETA(DisplayName="Client")
};

/**
 * Stores the current play mode and centralizes travel to directly assigned world assets.
 * The subsystem lives for the GameInstance lifetime, so the selected world folder and server address
 * survive the MainWorld -> gameplay-world travel.
 */
UCLASS(BlueprintType)
class GLTFSIMULATOR_API UMultiplayerWorldSubSystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    static UMultiplayerWorldSubSystem* Get(const UObject* WorldContextObject);

    UFUNCTION(BlueprintPure, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    static UMultiplayerWorldSubSystem* Find(const UObject* WorldContextObject) { return Get(WorldContextObject); }

    UFUNCTION(BlueprintCallable, Category="Multiplayer")
    void SetSelectedWorldFolderName(const FString& InWorldFolderName) { SelectedWorldFolderName = InWorldFolderName; }

    UFUNCTION(BlueprintPure, Category="Multiplayer")
    FString GetSelectedWorldFolderName() const { return SelectedWorldFolderName; }

    UFUNCTION(BlueprintCallable, Category="Multiplayer")
    void SetServerAddress(const FString& InServerAddress) { ServerAddress = InServerAddress; }

    UFUNCTION(BlueprintPure, Category="Multiplayer")
    FString GetServerAddress() const { return ServerAddress; }

    UFUNCTION(BlueprintCallable, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    bool StartSinglePlayerWorld(const UObject* WorldContextObject, const FString& WorldFolderName, TSoftObjectPtr<UWorld> SinglePlayerWorld);

    /** Direct world/GameMode travel path used by StartActor and optional Blueprint callers. */
    UFUNCTION(BlueprintCallable, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    bool StartSinglePlayerWorldWithGameMode(
        const UObject* WorldContextObject,
        const FString& WorldFolderName,
        TSoftObjectPtr<UWorld> SinglePlayerWorld,
        TSoftClassPtr<AGameModeBase> GameModeOverride);

    UFUNCTION(BlueprintCallable, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    bool HostMultiplayerWorld(const UObject* WorldContextObject, const FString& WorldFolderName, TSoftObjectPtr<UWorld> HostWorld, int32 Port = 7777);

    /** Direct host-world/GameMode travel path used by StartActor and optional Blueprint callers. */
    UFUNCTION(BlueprintCallable, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    bool HostMultiplayerWorldWithGameMode(
        const UObject* WorldContextObject,
        const FString& WorldFolderName,
        TSoftObjectPtr<UWorld> HostWorld,
        TSoftClassPtr<AGameModeBase> GameModeOverride,
        int32 Port = 7777);

    UFUNCTION(BlueprintCallable, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    bool OpenClientConnectionWorld(const UObject* WorldContextObject, TSoftObjectPtr<UWorld> ClientWorld);

    UFUNCTION(BlueprintCallable, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    bool JoinMultiplayerWorld(const UObject* WorldContextObject, const FString& InServerAddress, const FString& WorldFolderName);

    UFUNCTION(BlueprintPure, Category="Multiplayer")
    EMultiplayerWorldMode GetWorldMode() const { return WorldMode; }

    UFUNCTION(BlueprintPure, Category="Multiplayer")
    bool IsMultiplayer() const { return WorldMode != EMultiplayerWorldMode::SinglePlayer; }

    UFUNCTION(BlueprintPure, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    bool ShouldRunServerAuthority(const UObject* WorldContextObject) const;

    UFUNCTION(BlueprintPure, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    static bool ShouldUseClientRenderOnlyStreaming(const UObject* WorldContextObject);

    /** Expected server GameMode for the most recent local map travel. Empty means use map World Settings. */
    TSoftClassPtr<AGameModeBase> GetRequestedGameModeOverride() const { return RequestedGameModeOverride; }

    /** External folder associated with the most recent explicit GameMode request. */
    FString GetRequestedGameModeWorldFolder() const { return RequestedGameModeWorldFolder; }

    /** Clears travel diagnostics after returning to the menu or starting a client-only connection flow. */
    void ClearRequestedGameModeOverride();

private:
    EMultiplayerWorldMode WorldMode = EMultiplayerWorldMode::SinglePlayer;
    FString SelectedWorldFolderName;
    FString ServerAddress = TEXT("127.0.0.1:7777");

    UPROPERTY(Transient)
    TSoftClassPtr<AGameModeBase> RequestedGameModeOverride;

    UPROPERTY(Transient)
    FString RequestedGameModeWorldFolder;

    bool OpenWorldByReference(
        const UObject* WorldContextObject,
        TSoftObjectPtr<UWorld> WorldAsset,
        const FString& Options,
        TSoftClassPtr<AGameModeBase> GameModeOverride) const;
};
