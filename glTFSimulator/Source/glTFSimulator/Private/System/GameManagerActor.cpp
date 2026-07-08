// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/GameManagerActor.h"
#include "System/GameManagerSubSystem.h"
#include "System/GameUpdateSubSystem.h"
#include "Model/EditableMeshActor.h"
#include "World/PrefabActor.h"
#include "Vehicle/VehiclePawn.h"
#include "Weapon/WeaponActor.h"
#include "Model/glTFStreamActor.h"
#include "World/WorldManager.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

AGameManagerActor::AGameManagerActor()
{
    PrimaryActorTick.bCanEverTick = false;

    USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SetRootComponent(SceneRoot);

    static ConstructorHelpers::FObjectFinder<UMaterialInterface> GridMaterialFinder(TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial"));
    if (GridMaterialFinder.Succeeded())
    {
        PlacementGridMaterial = GridMaterialFinder.Object;
    }

    PrefabActorClass = APrefabActor::StaticClass();
    EditableMeshActorClass = AEditableMeshActor::StaticClass();
    VehiclePawnClass = AVehiclePawn::StaticClass();
    WeaponActorClass = AWeaponActor::StaticClass();
    WorldManagerClass = AWorldManager::StaticClass();
    SpawnActorClass = AglTFStreamActor::StaticClass();
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
        GameUpdateTickHandle = GameUpdate->RegisterUpdate(
            this,
            [this](const float DeltaSeconds)
            {
                TickFromGameUpdate(DeltaSeconds);
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
        GameManager->StopGameManager(EndPlayReason);
    }

    Super::EndPlay(EndPlayReason);
}

void AGameManagerActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    TickFromGameUpdate(DeltaSeconds);
}

void AGameManagerActor::TickFromGameUpdate(float DeltaSeconds)
{
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->TickGameManager(DeltaSeconds);
    }
}
