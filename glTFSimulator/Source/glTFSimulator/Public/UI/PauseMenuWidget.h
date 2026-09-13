// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file PauseMenuWidget.h
 * 역할: 일시정지 메뉴의 공통 UI 동작을 제공합니다.
 * 핵심 기능: 버튼 바인딩, 재개·설정·월드 선택 이동.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "PauseMenuWidget.generated.h"

class UButton;
class UTextBlock;

/**
 * Blueprint-editable pause menu base widget.
 *
 * Create the visual tree in a WBP child and pass widget references explicitly from the
 * WBP Construct event. This class does not use automatic widget-name binding or widget-tree lookup.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UPauseMenuWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    UFUNCTION(BlueprintCallable, Category="Pause|Widgets")
    void SetTitleText(UTextBlock* InTitleText);

    UFUNCTION(BlueprintCallable, Category="Pause|Widgets")
    void SetContinueButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Pause|Widgets")
    void SetSettingsButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Pause|Widgets")
    void SetExitButton(UButton* InButton);

    UFUNCTION(BlueprintCallable, Category="Pause")
    void ContinueFromUI();

    UFUNCTION(BlueprintCallable, Category="Pause")
    void OpenSettingsFromUI();

    UFUNCTION(BlueprintCallable, Category="Pause")
    void ExitFromUI();

    /** Re-enables Exit after a rejected/failed level-travel request restores this cached widget. */
    void ResetExitRequestState();

private:
    void BindButtonEvents();
    void UnbindButtonEvents();

    TWeakObjectPtr<UTextBlock> AssignedTitleText;
    TWeakObjectPtr<UButton> AssignedContinueButton;
    TWeakObjectPtr<UButton> AssignedSettingsButton;
    TWeakObjectPtr<UButton> AssignedExitButton;

    /** Prevents one click or duplicate Blueprint bindings from issuing more than one level travel. */
    bool bExitRequestInProgress = false;
};
