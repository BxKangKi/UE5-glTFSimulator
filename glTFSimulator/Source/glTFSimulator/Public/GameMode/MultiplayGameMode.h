// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "GameMode/GameplayGameModeBase.h"
#include "MultiplayGameMode.generated.h"

/** Listen/dedicated-server gameplay GameMode. Clients bootstrap render-only streaming from replicated world state. */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API AMultiplayGameMode : public AGlTFSimulatorGameplayGameModeBase
{
    GENERATED_BODY()
public:
    AMultiplayGameMode();

protected:
    virtual void BeginPlay() override;
};
