// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "GameMode/GameplayGameModeBase.h"
#include "SingleplayGameMode.generated.h"

/** Standalone gameplay GameMode. Runtime world data is read only from Worlds/<name>.gworld. */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API ASingleplayGameMode : public AGlTFSimulatorGameplayGameModeBase
{
    GENERATED_BODY()
public:
    ASingleplayGameMode();

protected:
    virtual void BeginPlay() override;
};
