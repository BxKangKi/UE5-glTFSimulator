// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Runtime/RuntimeGameplayManager.h"
#include "RuntimeCreatorHUDWidget.generated.h"

class UBorder;
class UScrollBox;
class UTextBlock;
class UUniformGridPanel;

/**
 * Runtime Creator HUD bridge class.
 *
 * This class no longer creates a native/UMG fallback layout. Create your own WBP
 * and bind its buttons to the BlueprintCallable functions below.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API URuntimeCreatorHUDWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    /** 현재 HUD가 연결할 RuntimeGameplayManager를 다시 찾고 모든 표시를 갱신합니다. */
    UFUNCTION(BlueprintCallable, Category="Runtime Creator HUD")
    void RefreshRuntimeManagerReference();

    /** 사용자 제작 WBP가 필요할 때 호출할 수 있는 툴바 갱신 이벤트 진입점입니다. 자동 버튼 생성은 하지 않습니다. */
    UFUNCTION(BlueprintCallable, Category="Runtime Creator HUD")
    void RefreshToolbar();

    /** 사용자 제작 WBP가 필요할 때 호출할 수 있는 아이템 목록 갱신 진입점입니다. 자동 버튼 생성은 하지 않습니다. */
    UFUNCTION(BlueprintCallable, Category="Runtime Creator HUD")
    void RefreshItemList();

    /** 상태/좌표/스냅 텍스트를 즉시 갱신합니다. */
    UFUNCTION(BlueprintCallable, Category="Runtime Creator HUD")
    void RefreshStatus();

    /** 외부 BP가 수동으로 특정 슬롯을 선택하고 싶을 때 호출합니다. */
    UFUNCTION(BlueprintCallable, Category="Runtime Creator HUD|Toolbar")
    void SelectToolbarSlotFromUI(int32 SlotIndex);

    /** 외부 BP가 수동으로 전체 아이템 목록의 항목을 선택하고 싶을 때 호출합니다. */
    UFUNCTION(BlueprintCallable, Category="Runtime Creator HUD|Inventory")
    void SelectAvailableItemFromUI(int32 AvailableItemIndex);

    /** UI 버튼에서 전체 아이템 창을 열고 닫을 때 호출합니다. */
    UFUNCTION(BlueprintCallable, Category="Runtime Creator HUD|Inventory")
    void ToggleItemListFromUI();

    /** UI 버튼에서 현재 편집 메시를 완료할 때 호출합니다. */
    UFUNCTION(BlueprintCallable, Category="Runtime Creator HUD|Editing")
    void FinishEditFromUI();

    /** UI 버튼에서 현재 편집 메시를 취소할 때 호출합니다. */
    UFUNCTION(BlueprintCallable, Category="Runtime Creator HUD|Editing")
    void CancelEditFromUI();

    /** UI 버튼에서 Runtime scene 저장을 실행할 때 호출합니다. */
    UFUNCTION(BlueprintCallable, Category="Runtime Creator HUD|Save")
    void SaveRuntimeSceneFromUI();

    /** UI 버튼에서 스냅을 토글할 때 호출합니다. */
    UFUNCTION(BlueprintCallable, Category="Runtime Creator HUD|Placement")
    void ToggleSnapFromUI();

protected:
    /** 위젯 생성 후 manager 이벤트를 바인딩합니다. */
    virtual void NativeConstruct() override;

    /** 위젯 제거 시 manager 이벤트 바인딩을 정리합니다. */
    virtual void NativeDestruct() override;

    /** 중앙 커서/상태처럼 매 프레임 바뀌는 값들을 가볍게 갱신합니다. */
    virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
    /** RuntimeGameplayManager의 상태 변경 이벤트를 HUD 갱신으로 연결합니다. */
    UFUNCTION()
    void HandleRuntimeStateChanged();

    /** RuntimeGameplayManager의 메시지 변경 이벤트를 HUD 갱신으로 연결합니다. */
    UFUNCTION()
    void HandleRuntimeMessageChanged(const FString& Message);

    /** RuntimeGameplayManager의 툴바 변경 이벤트를 HUD 갱신으로 연결합니다. */
    UFUNCTION()
    void HandleRuntimeToolbarChanged();

    /** RuntimeGameplayManager의 전체 아이템 창 이벤트를 HUD 갱신으로 연결합니다. */
    UFUNCTION()
    void HandleRuntimeItemListWindowChanged(bool bOpen);

    /** 상태 패널 텍스트를 반환합니다. */
    FText GetStatusText() const;

    /** 좌표/스냅/유효성 패널 텍스트를 반환합니다. */
    FText GetPlacementInfoText() const;

    /** 마지막 Runtime 메시지를 반환합니다. */
    FText GetMessageText() const;

    /** manager 포인터가 아직 없으면 찾아서 캐싱합니다. */
    ARuntimeGameplayManager* GetRuntimeManager() const;

    /** manager 이벤트를 중복 없이 바인딩합니다. */
    void BindRuntimeManagerEvents();

    /** manager 이벤트 바인딩을 안전하게 해제합니다. */
    void UnbindRuntimeManagerEvents();

    /** WBP에 같은 이름으로 배치된 텍스트/패널 참조만 찾아 캐싱합니다. 새 위젯은 만들지 않습니다. */
    void CacheUserWidgetReferences();

private:
    /** 사용자 제작 WBP에 같은 이름으로 존재할 때만 자동 텍스트 업데이트에 사용됩니다. */
    UPROPERTY(Transient)
    TObjectPtr<UUniformGridPanel> UserToolbarGrid;

    UPROPERTY(Transient)
    TObjectPtr<UScrollBox> UserItemListScrollBox;

    UPROPERTY(Transient)
    TObjectPtr<UBorder> UserItemListPanel;

    UPROPERTY(Transient)
    TObjectPtr<UTextBlock> UserStatusTextBlock;

    UPROPERTY(Transient)
    TObjectPtr<UTextBlock> UserPlacementInfoTextBlock;

    UPROPERTY(Transient)
    TObjectPtr<UTextBlock> UserMessageTextBlock;

    /** HUD가 현재 연결 중인 RuntimeGameplayManager입니다. */
    UPROPERTY(Transient)
    mutable TObjectPtr<ARuntimeGameplayManager> CachedRuntimeManager;

    /** 이벤트가 이미 바인딩되어 있는지 추적해 중복 바인딩을 막습니다. */
    bool bRuntimeEventsBound = false;
};
