// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/GameManagerActor.h"
#include "System/GameManagerSubSystem.h"
#include "Gameplay/EditableMeshActor.h"
#include "Gameplay/PrefabActor.h"
#include "Gameplay/VehiclePawn.h"
#include "Gameplay/WeaponActor.h"
#include "Model/glTFStreamActor.h"
#include "World/WorldManager.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

AGameManagerActor::AGameManagerActor()
{
    PrimaryActorTick.bCanEverTick = true;

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
}

void AGameManagerActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->StopGameManager(EndPlayReason);
    }

    Super::EndPlay(EndPlayReason);
}

void AGameManagerActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->TickGameManager(DeltaSeconds);
    }
}
