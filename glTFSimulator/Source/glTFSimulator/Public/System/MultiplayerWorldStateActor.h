// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MultiplayerWorldStateActor.generated.h"

/**
 * Small replicated authority state that tells clients which downloaded world folder
 * the server is running. Clients still stream their own render-only GLB data locally;
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

private:
    UPROPERTY(ReplicatedUsing=OnRep_WorldFolderName)
    FString WorldFolderName;

    UFUNCTION()
    void OnRep_WorldFolderName();

    void ApplyWorldFolderName() const;
};
