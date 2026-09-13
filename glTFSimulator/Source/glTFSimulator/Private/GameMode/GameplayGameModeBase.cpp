// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "GameMode/GameplayGameModeBase.h"

#include "System/GameManagerSubSystem.h"
#include "System/GameUpdateSubSystem.h"
#include "System/GlTFSimulatorGameInstance.h"
#include "Character/CharacterController.h"
#include "Character/PlayerCharacterController.h"

AGlTFSimulatorGameplayGameModeBase::AGlTFSimulatorGameplayGameModeBase()
{
    PrimaryActorTick.bCanEverTick = false;

    // Native defaults remove the need for a Blueprint GameMode just to wire the standard simulator
    // pawn/controller. Blueprint subclasses can still override these classes if a project needs to.
    PlayerControllerClass = APlayerCharacterController::StaticClass();
    DefaultPawnClass = ACharacterController::StaticClass();
}

void AGlTFSimulatorGameplayGameModeBase::BeginPlay()
{
    // Prepare the class-backed central registry before Blueprint ReceiveBeginPlay or any runtime
    // subsystem asks for assets. The GameInstance keeps the resulting instance private.
    if (UGlTFSimulatorGameInstance* SimulatorGameInstance = Cast<UGlTFSimulatorGameInstance>(GetGameInstance()))
    {
        SimulatorGameInstance->EnsureAssetRegistry();
    }

    Super::BeginPlay();

    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        Manager->StartGameplaySession(this);
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        TWeakObjectPtr<AGlTFSimulatorGameplayGameModeBase> WeakThis(this);
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [WeakThis](const float DeltaSeconds)
            {
                if (AGlTFSimulatorGameplayGameModeBase* StrongThis = WeakThis.Get())
                {
                    StrongThis->UpdateFromGameUpdate(DeltaSeconds);
                }
            },
            5);
    }
}

void AGlTFSimulatorGameplayGameModeBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
    }
    GameUpdateTickHandle = INDEX_NONE;

    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        Manager->StopGameplaySession(EndPlayReason, this);
    }

    Super::EndPlay(EndPlayReason);
}

void AGlTFSimulatorGameplayGameModeBase::UpdateFromGameUpdate(const float DeltaSeconds)
{
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        if (Manager->IsActiveGameMode(this))
        {
            Manager->UpdateGameManager(DeltaSeconds);
        }
    }
}
