// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/MultiplayerWorldStateActor.h"
#include "System/GameManagerSubSystem.h"
#include "System/MultiplayerWorldSubSystem.h"
#include "Engine/World.h"
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

    WorldFolderName = InWorldFolderName;
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
        return;
    }

    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        Multiplayer->SetSelectedWorldFolderName(WorldFolderName);
    }

    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        Manager->SetCurrentWorldName(WorldFolderName);
    }
}
