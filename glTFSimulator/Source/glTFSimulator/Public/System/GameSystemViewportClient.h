/**
 * @file GameSystemViewportClient.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameViewportClient.h"
#include "GameSystemViewportClient.generated.h"

UCLASS()
class GLTFSIMULATOR_API UGameSystemViewportClient : public UGameViewportClient
{
	GENERATED_BODY()

public:
	virtual bool InputKey(const FInputKeyEventArgs& EventArgs) override;
};