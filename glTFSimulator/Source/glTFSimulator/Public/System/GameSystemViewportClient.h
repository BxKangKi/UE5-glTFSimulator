/**
 * @file GameSystemViewportClient.h
 * 역할: 프로젝트의 게임 뷰포트 클라이언트를 제공합니다.
 * 핵심 기능: 게임 뷰포트 동작 확장.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
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