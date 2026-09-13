// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "System/GameManagerSubSystem.h"
#include "GameplayGameModeBase.generated.h"

/**
 * Common gameplay bootstrap owned by Unreal's GameMode lifecycle.
 *
 * This replaces the old level-placed/spawned GameManagerActor bootstrap. The GameMode owns only
 * light numeric/session tuning; heavyweight asset/class references stay in UGlTFSimulatorAssetRegistry.
 * Single-player and multiplayer GameModes derive from this class and share the same validated .gwd
 * startup path through UGameManagerSubSystem.
 */
UCLASS(Abstract, Blueprintable, BlueprintType)
class GLTFSIMULATOR_API AGlTFSimulatorGameplayGameModeBase : public AGameModeBase
{
    GENERATED_BODY()

public:
    AGlTFSimulatorGameplayGameModeBase();

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|Grid", meta=(ClampMin="1.0", Units="cm"))
    float PlacementGridSpacing = 100.0f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|Grid", meta=(ClampMin="0.25", Units="cm"))
    float PlacementGridLineThickness = 0.55f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|Grid", meta=(ClampMin="100.0", Units="cm"))
    float PlacementGridMaxRadius = 300.0f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|Grid", meta=(ClampMin="100.0", Units="cm"))
    float PlacementGridStrongRadius = 100.0f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|Grid", meta=(ClampMin="100.0", Units="cm"))
    float PlacementGridFadeRadius = 300.0f;

    /** Optional direct-PIE world key. Normal menu travel carries ?World=<key>. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|World")
    FString WorldFolderName;

    /** Legacy Blueprint field retained for asset compatibility. Global ocean transform is native-hardcoded. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|World", meta=(DeprecatedProperty, DeprecationMessage="Global ocean transform is hardcoded in GameManagerSubSystem"))
    FTransform OceanTransform;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|Placement", meta=(ClampMin="0.0", Units="cm"))
    float PlacementTraceDistance = 1000.0f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|Placement", meta=(ClampMin="0.0", Units="cm"))
    float CrosshairCollisionTraceDistance = 1000.0f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|Placement", meta=(ClampMin="1.0", Units="cm"))
    float FreeSpacePlacementDistance = 1000.0f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|Placement")
    bool bAllowFreeSpacePlacement = true;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|Placement", meta=(ClampMin="1.0"))
    float GridSize = 100.0f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|Placement")
    float SurfacePlacementOffset = 2.0f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|Vehicle", meta=(ClampMin="0.0"))
    float VehicleEnterDistance = 450.0f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|Save")
    bool bAutoSaveScene = true;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|Save", meta=(ClampMin="5.0", Units="s"))
    float SceneAutoSaveIntervalSeconds = 60.0f;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|Save")
    bool bSaveSceneOnEndPlay = true;

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Game|Mode")
    EPlayMode PlayMode = EPlayMode::Creator;

private:
    int32 GameUpdateTickHandle = INDEX_NONE;
    void UpdateFromGameUpdate(float DeltaSeconds);
};
