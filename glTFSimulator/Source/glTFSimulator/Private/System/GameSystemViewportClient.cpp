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