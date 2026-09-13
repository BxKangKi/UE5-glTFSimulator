// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file MultiplayerWorldStateActor.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MultiplayerWorldStateActor.generated.h"

/**
 * Small replicated authority state that tells clients which downloaded world folder
 * the server is running. Clients still stream their own render-only .gwd data locally;
 * the server stays authoritative for gameplay, collision and simulation.
 */
UCLASS(BlueprintType)
class GLTFSIMULATOR_API AMultiplayerWorldStateActor : public AActor
{
    GENERATED_BODY()

public:
    AMultiplayerWorldStateActor();

    static AMultiplayerWorldStateActor* SpawnOrUpdateForWorld(UObject* WorldContextObject, const FString& InWorldFolderName);

    UFUNCTION(BlueprintCallable, Category="Multiplayer")
    void SetWorldFolderName(const FString& InWorldFolderName);

    UFUNCTION(BlueprintPure, Category="Multiplayer")
    const FString& GetWorldFolderName() const { return WorldFolderName; }

protected:
    virtual void BeginPlay() override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
    UPROPERTY(ReplicatedUsing=OnRep_WorldFolderName)
    FString WorldFolderName;

private:
    UFUNCTION()
    void OnRep_WorldFolderName();

    void ApplyWorldFolderName() const;
};
