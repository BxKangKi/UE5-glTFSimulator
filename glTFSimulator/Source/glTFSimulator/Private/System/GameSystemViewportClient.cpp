/**
 * @file GameSystemViewportClient.cpp
 * 역할: 프로젝트의 게임 뷰포트 클라이언트를 제공합니다.
 * 핵심 기능: 게임 뷰포트 동작 확장.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
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