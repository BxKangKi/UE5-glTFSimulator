/**
 * @file GameSystemViewportClient.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "System/GameSystemViewportClient.h"
#include "System/GameManagerSubSystem.h"

bool UGameSystemViewportClient::InputKey(const FInputKeyEventArgs &EventArgs)
{
	// Handles the F11 key press.
	if (EventArgs.Key == EKeys::F11 && EventArgs.Event == IE_Pressed)
	{
		UGameManagerSubSystem::ToggleFullscreen();
	}

	return Super::InputKey(EventArgs);
}