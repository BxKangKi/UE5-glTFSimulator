// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "System/GameManagerSubSystem.h"
#include "GameManagerActor.generated.h"

class APrefabActor;
class AEditableMeshActor;
class AVehiclePawn;
class AWeaponActor;
class AWorldManager;
class AWeatherActor;
class AglTFStreamActor;
class UMaterialInterface;
class UUserWidget;
class UGameUpdateSubSystem;

/**
 * Editor-facing configuration actor for the game manager subsystem.
 * Runtime state and behavior live in UGameManagerSubSystem; this actor only exposes level/BP defaults.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API AGameManagerActor : public AActor
{
    GENERATED_BODY()

public:
    AGameManagerActor();

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Grid")
    TObjectPtr<UMaterialInterface> PlacementGridMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Grid", meta=(ClampMin="1.0", Units="cm"))
    float PlacementGridSpacing = 100.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Grid", meta=(ClampMin="0.25", Units="cm"))
    float PlacementGridLineThickness = 0.55f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Grid", meta=(ClampMin="100.0", Units="cm"))
    float PlacementGridMaxRadius = 300.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Grid", meta=(ClampMin="100.0", Units="cm"))
    float PlacementGridStrongRadius = 100.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Grid", meta=(ClampMin="100.0", Units="cm"))
    float PlacementGridFadeRadius = 300.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Classes")
    TSubclassOf<APrefabActor> PrefabActorClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Classes")
    TSubclassOf<AEditableMeshActor> EditableMeshActorClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Classes")
    TSubclassOf<AVehiclePawn> VehiclePawnClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Classes")
    TSubclassOf<AWeaponActor> WeaponActorClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|World")
    TSubclassOf<AWorldManager> WorldManagerClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|World")
    TSubclassOf<AglTFStreamActor> SpawnActorClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|World")
    TSubclassOf<AWeatherActor> WeatherActorClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|World")
    TSubclassOf<AActor> WaterClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|World")
    FTransform OceanTransform;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|World")
    TSubclassOf<UUserWidget> LoadingWidgetClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Placement", meta=(ClampMin="0.0", Units="cm"))
    float PlacementTraceDistance = 1000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Placement", meta=(ClampMin="0.0", Units="cm"))
    float CrosshairCollisionTraceDistance = 1000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Placement", meta=(ClampMin="1.0", Units="cm"))
    float FreeSpacePlacementDistance = 1000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Placement")
    bool bAllowFreeSpacePlacement = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Placement")
    float GridSize = 100.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Placement")
    float SurfacePlacementOffset = 2.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Placement")
    float VertexSelectionRayDistance = 28.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Placement")
    float VertexDragHoldSeconds = 0.18f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Placement")
    float VertexDragStartDistance = 18.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Vehicle")
    float VehicleEnterDistance = 450.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Object Creation")
    bool bEnableObjectVertexCreation = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Save")
    bool bAutoSaveScene = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Save", meta=(ClampMin="5.0", Units="s"))
    float SceneAutoSaveIntervalSeconds = 60.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Save")
    bool bSaveSceneOnEndPlay = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game|Mode")
    EPlayMode PlayMode = EPlayMode::Creator;

private:
    int32 GameUpdateTickHandle = INDEX_NONE;
    void UpdateFromGameUpdate(float DeltaSeconds);
};
