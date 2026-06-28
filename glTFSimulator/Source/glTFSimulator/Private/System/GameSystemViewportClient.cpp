#include "System/GameSystemViewportClient.h"
#include "System/GameManagerSubSystem.h"

bool UGameSystemViewportClient::InputKey(const FInputKeyEventArgs &EventArgs)
{
	// F11키가 눌렸을 때 (Pressed)
	if (EventArgs.Key == EKeys::F11 && EventArgs.Event == IE_Pressed)
	{
		UGameManagerSubSystem::ToggleFullscreen();
	}

	return Super::InputKey(EventArgs);
}