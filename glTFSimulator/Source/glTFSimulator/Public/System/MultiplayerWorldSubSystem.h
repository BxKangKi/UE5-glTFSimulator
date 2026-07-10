// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "MultiplayerWorldSubSystem.generated.h"

UENUM(BlueprintType)
enum class EMultiplayerWorldMode : uint8
{
    SinglePlayer UMETA(DisplayName="Single Player"),
    Host UMETA(DisplayName="Host"),
    Client UMETA(DisplayName="Client")
};

/**
 * Stores the current play mode and centralizes level travel for SingleWorld / HostWorld / ClientWorld.
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
    bool StartSinglePlayerWorld(const UObject* WorldContextObject, const FString& WorldFolderName, FName SingleWorldLevelName);

    UFUNCTION(BlueprintCallable, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    bool HostMultiplayerWorld(const UObject* WorldContextObject, const FString& WorldFolderName, FName HostWorldLevelName, int32 Port = 7777);

    UFUNCTION(BlueprintCallable, Category="Multiplayer", meta=(WorldContext="WorldContextObject"))
    bool OpenClientConnectionWorld(const UObject* WorldContextObject, FName ClientWorldLevelName);

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

private:
    EMultiplayerWorldMode WorldMode = EMultiplayerWorldMode::SinglePlayer;
    FString SelectedWorldFolderName;
    FString ServerAddress = TEXT("127.0.0.1:7777");

    bool OpenLevelByName(const UObject* WorldContextObject, FName LevelName, const FString& Options = FString()) const;
};
