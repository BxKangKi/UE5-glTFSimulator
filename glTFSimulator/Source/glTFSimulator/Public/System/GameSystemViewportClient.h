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