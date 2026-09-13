// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file MultiplayerWorldStateActor.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "System/MultiplayerWorldStateActor.h"
#include "System/GameManagerSubSystem.h"
#include "System/MultiplayerWorldSubSystem.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"

AMultiplayerWorldStateActor::AMultiplayerWorldStateActor()
{
    bReplicates = true;
    bAlwaysRelevant = true;
    SetReplicateMovement(false);
    SetNetUpdateFrequency(2.0f);
    SetMinNetUpdateFrequency(1.0f);
}

AMultiplayerWorldStateActor* AMultiplayerWorldStateActor::SpawnOrUpdateForWorld(UObject* WorldContextObject, const FString& InWorldFolderName)
{
    UWorld* World = IsValid(WorldContextObject) ? WorldContextObject->GetWorld() : nullptr;
    if (!World || World->GetNetMode() == NM_Client)
    {
        return nullptr;
    }

    for (TActorIterator<AMultiplayerWorldStateActor> It(World); It; ++It)
    {
        AMultiplayerWorldStateActor* Existing = *It;
        if (IsValid(Existing))
        {
            Existing->SetWorldFolderName(InWorldFolderName);
            return Existing;
        }
    }

    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AMultiplayerWorldStateActor* Spawned = World->SpawnActor<AMultiplayerWorldStateActor>(AMultiplayerWorldStateActor::StaticClass(), FTransform::Identity, Params);
    if (IsValid(Spawned))
    {
        Spawned->SetWorldFolderName(InWorldFolderName);
    }
    return Spawned;
}

void AMultiplayerWorldStateActor::SetWorldFolderName(const FString& InWorldFolderName)
{
    if (!HasAuthority())
    {
        return;
    }

    FString Normalized;
    if (!UGameManagerSubSystem::TryNormalizeWorldFolderName(
            InWorldFolderName, Normalized, false))
    {
        // Replicated state is a trust boundary. Keep the last valid authoritative value: publishing
        // an empty replacement is ambiguous with a newly spawned actor's pre-replication default
        // and would make an existing client retain a different value from the server.
        UE_LOG(LogTemp, Error,
            TEXT("Refused to replicate an invalid world folder key: %s"),
            *InWorldFolderName.Left(256));
        return;
    }

    WorldFolderName = MoveTemp(Normalized);
    ApplyWorldFolderName();
    ForceNetUpdate();
}

void AMultiplayerWorldStateActor::BeginPlay()
{
    Super::BeginPlay();
    ApplyWorldFolderName();
}

void AMultiplayerWorldStateActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AMultiplayerWorldStateActor, WorldFolderName);
}

void AMultiplayerWorldStateActor::OnRep_WorldFolderName()
{
    ApplyWorldFolderName();
}

void AMultiplayerWorldStateActor::ApplyWorldFolderName() const
{
    if (WorldFolderName.IsEmpty())
    {
        // A new replicated actor may briefly hold its class-default value before the first network
        // update. Preserve the URL/GameInstance hand-off during that window instead of clearing it.
        return;
    }

    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        Multiplayer->SetSelectedWorldFolderName(WorldFolderName);
    }

    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        Manager->SetCurrentWorldName(WorldFolderName);
        if (UWorld* World = GetWorld(); World && World->GetNetMode() == NM_Client)
        {
            if (APlayerController* PC = World->GetFirstPlayerController())
            {
                Manager->StartClientGameplaySession(PC);
            }
        }
    }
}
