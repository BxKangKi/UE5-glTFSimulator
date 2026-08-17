// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/GameManagerActor.h"
#include "System/GameManagerSubSystem.h"
#include "System/GameUpdateSubSystem.h"
#include "World/PrefabActor.h"
#include "Vehicle/VehiclePawn.h"
#include "Weapon/WeaponActor.h"
#include "Model/glTFStreamActor.h"
#include "World/WorldEnvManager.h"
#include "World/WeatherActor.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"

AGameManagerActor::AGameManagerActor()
{
    PrimaryActorTick.bCanEverTick = false;

    USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SetRootComponent(SceneRoot);

    // PlacementGridMaterial is assigned directly in the owning Blueprint/class defaults.

    PrefabActorClass = APrefabActor::StaticClass();
    VehiclePawnClass = AVehiclePawn::StaticClass();
    WeaponActorClass = AWeaponActor::StaticClass();
    WorldEnvManagerClass = AWorldEnvManager::StaticClass();
    SpawnActorClass = AglTFStreamActor::StaticClass();
    WeatherActorClass = AWeatherActor::StaticClass();
}


void AGameManagerActor::PostLoad()
{
    Super::PostLoad();
}

void AGameManagerActor::BeginPlay()
{
    Super::BeginPlay();

    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->StartGameManager(this);
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        TWeakObjectPtr<AGameManagerActor> WeakThis(this);
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [WeakThis](const float DeltaSeconds)
            {
                if (AGameManagerActor* StrongThis = WeakThis.Get())
                {
                    StrongThis->UpdateFromGameUpdate(DeltaSeconds);
                }
            },
            5);
    }
}

void AGameManagerActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateTickHandle);
    }
    GameUpdateTickHandle = INDEX_NONE;

    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->StopGameManager(EndPlayReason, this);
    }

    Super::EndPlay(EndPlayReason);
}


void AGameManagerActor::UpdateFromGameUpdate(float DeltaSeconds)
{
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->UpdateGameManager(DeltaSeconds);
    }
}
