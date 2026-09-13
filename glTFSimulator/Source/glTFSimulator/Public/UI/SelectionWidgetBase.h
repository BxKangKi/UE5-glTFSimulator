// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file SelectionWidgetBase.h
 * 역할: 선택형 메뉴가 공통으로 사용하는 동적 버튼 목록과 스타일을 관리합니다.
 * 핵심 기능: 목록 패널 수명, 생성 버튼 클릭 디스패치, 크기·정렬·폰트·버튼 스타일 커스터마이징.
 */

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Fonts/SlateFontInfo.h"
#include "Framework/Text/TextLayout.h"
#include "Layout/Children.h"
#include "Styling/SlateColor.h"
#include "Styling/SlateTypes.h"
#include "SelectionWidgetBase.generated.h"

class UButton;
class UPanelWidget;
class USelectionWidgetBase;
class USizeBox;
class UTextBlock;
class UWidget;

/** Lightweight dispatcher owned by USelectionWidgetBase for generated button clicks. */
UCLASS()
class GLTFSIMULATOR_API USelectionButtonClickHandler final : public UObject
{
    GENERATED_BODY()

public:
    void Setup(USelectionWidgetBase* InOwnerWidget, const FString& InSelectionKey);

    UFUNCTION()
    void HandleClicked();

private:
    TWeakObjectPtr<USelectionWidgetBase> OwnerWidget;
    FString SelectionKey;
};

/**
 * Common base for menus that display a generated list of selectable entries.
 *
 * Derived widgets own discovery/domain behavior only. This class owns the generated UMG entry
 * hierarchy and every shared button/layout/text customization option so ProjectSelectionWidget
 * and WorldSelectionWidget stay visually and behaviorally consistent.
 */
UCLASS(Abstract, Blueprintable, BlueprintType)
class GLTFSIMULATOR_API USelectionWidgetBase : public UUserWidget
{
    GENERATED_BODY()

public:
    virtual void NativeDestruct() override;

    /** Internal entry dispatcher. Generated click handlers call this after validating the owner. */
    void HandleGeneratedSelectionClicked(const FString& SelectionKey);

    /** Sets the generated button width/height. Values <= 0 keep desired size on that axis. */
    UFUNCTION(BlueprintCallable, Category="Selection|Generated Button Layout")
    void SetGeneratedButtonSize(const FVector2D& InButtonSize);

    /** Sets alignment of each generated entry inside supported panel slots. */
    UFUNCTION(BlueprintCallable, Category="Selection|Generated Button Layout")
    void SetGeneratedButtonPanelAlignment(EHorizontalAlignment InHorizontalAlignment, EVerticalAlignment InVerticalAlignment);

    /** Sets alignment of the generated label inside each button. */
    UFUNCTION(BlueprintCallable, Category="Selection|Generated Button Layout")
    void SetGeneratedButtonContentAlignment(EHorizontalAlignment InHorizontalAlignment, EVerticalAlignment InVerticalAlignment);

    /** Sets padding on each generated entry when the parent slot supports it. */
    UFUNCTION(BlueprintCallable, Category="Selection|Generated Button Layout")
    void SetGeneratedButtonEntryPadding(const FMargin& InPadding);

    /** Sets padding around each generated button label. */
    UFUNCTION(BlueprintCallable, Category="Selection|Generated Button Layout")
    void SetGeneratedButtonTextPadding(const FMargin& InPadding);

    /** Sets label justification. */
    UFUNCTION(BlueprintCallable, Category="Selection|Generated Button Layout")
    void SetGeneratedButtonTextJustification(ETextJustify::Type InJustification);

    /** Enables/disables overriding the generated UButton style. */
    UFUNCTION(BlueprintCallable, Category="Selection|Generated Button Style")
    void SetGeneratedButtonStyleOverrideEnabled(bool bInOverrideStyle);

    /** Sets the generated UButton style and enables style override. */
    UFUNCTION(BlueprintCallable, Category="Selection|Generated Button Style")
    void SetGeneratedButtonWidgetStyle(const FButtonStyle& InButtonStyle);

    /** Sets generated UButton color multipliers. */
    UFUNCTION(BlueprintCallable, Category="Selection|Generated Button Style")
    void SetGeneratedButtonColors(const FLinearColor& InColorAndOpacity, const FLinearColor& InBackgroundColor);

    /** Sets generated label foreground color. */
    UFUNCTION(BlueprintCallable, Category="Selection|Generated Button Text")
    void SetGeneratedButtonTextColor(const FSlateColor& InTextColor);

    /** Sets generated label font size. */
    UFUNCTION(BlueprintCallable, Category="Selection|Generated Button Text")
    void SetGeneratedButtonFontSize(int32 InFontSize);

    /** Sets generated label typeface. NAME_None keeps the current/default typeface. */
    UFUNCTION(BlueprintCallable, Category="Selection|Generated Button Text")
    void SetGeneratedButtonTypeface(FName InTypefaceFontName);

    /** Sets generated label outline options. */
    UFUNCTION(BlueprintCallable, Category="Selection|Generated Button Text")
    void SetGeneratedButtonOutlineSettings(const FFontOutlineSettings& InOutlineSettings);

    /** Sets generated label shadow options. */
    UFUNCTION(BlueprintCallable, Category="Selection|Generated Button Text")
    void SetGeneratedButtonTextShadow(const FVector2D& InShadowOffset, const FLinearColor& InShadowColor);

    /** Reapplies current layout/style values to already generated entries. */
    UFUNCTION(BlueprintCallable, Category="Selection|Generated Button Layout")
    void RefreshGeneratedButtonLayout();

protected:
    /** Assigns the panel that owns generated entries. Existing generated entries are removed. */
    void SetSelectionListPanel(UPanelWidget* InPanel);

    /** Returns the currently assigned generated-entry panel. */
    UPanelWidget* GetSelectionListPanel() const { return SelectionListPanel.Get(); }

    /** Creates, styles, binds, and adds one generated entry to the assigned panel. */
    UWidget* AddGeneratedSelectionEntry(const FString& SelectionKey, const FString& DisplayName);

    /** Removes generated entries and click bindings without touching Blueprint-authored children. */
    void ClearGeneratedSelectionEntries();

    /** Domain action implemented by derived selection widgets. */
    virtual void OnSelectionEntryActivated(const FString& SelectionKey);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Layout", meta=(ClampMin="0.0"))
    FVector2D GeneratedButtonSize = FVector2D(320.0f, 48.0f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Layout")
    TEnumAsByte<EHorizontalAlignment> GeneratedButtonPanelHorizontalAlignment = HAlign_Center;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Layout")
    TEnumAsByte<EVerticalAlignment> GeneratedButtonPanelVerticalAlignment = VAlign_Center;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Layout")
    FMargin GeneratedButtonEntryPadding = FMargin(0.0f, 0.0f, 0.0f, 8.0f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Layout")
    TEnumAsByte<EHorizontalAlignment> GeneratedButtonContentHorizontalAlignment = HAlign_Center;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Layout")
    TEnumAsByte<EVerticalAlignment> GeneratedButtonContentVerticalAlignment = VAlign_Center;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Layout")
    FMargin GeneratedButtonTextPadding = FMargin(12.0f, 6.0f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Layout")
    TEnumAsByte<ETextJustify::Type> GeneratedButtonTextJustification = ETextJustify::Center;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Style")
    bool bOverrideGeneratedButtonWidgetStyle = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Style")
    FButtonStyle GeneratedButtonWidgetStyle;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Style")
    FLinearColor GeneratedButtonColorAndOpacity = FLinearColor::White;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Style")
    FLinearColor GeneratedButtonBackgroundColor = FLinearColor::White;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Text")
    FSlateColor GeneratedButtonTextColor = FSlateColor::UseForeground();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Text", meta=(ClampMin="1"))
    int32 GeneratedButtonFontSize = 24;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Text")
    FName GeneratedButtonTypefaceFontName = NAME_None;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Text")
    FFontOutlineSettings GeneratedButtonOutlineSettings;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Text")
    FVector2D GeneratedButtonTextShadowOffset = FVector2D::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Selection|Generated Button Text")
    FLinearColor GeneratedButtonTextShadowColor = FLinearColor::Transparent;

private:
    void ApplyGeneratedEntrySize(USizeBox* EntrySizeBox) const;
    void ApplyGeneratedEntrySlotLayout(UWidget* EntryWidget) const;
    void ApplyGeneratedButtonStyle(UButton* Button) const;
    void ApplyGeneratedButtonContentLayout(UButton* Button) const;
    void ApplyGeneratedLabelLayout(UTextBlock* Label) const;

    TWeakObjectPtr<UPanelWidget> SelectionListPanel;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UWidget>> GeneratedSelectionEntries;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UButton>> GeneratedSelectionButtons;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UTextBlock>> GeneratedSelectionLabels;

    UPROPERTY(Transient)
    TArray<TObjectPtr<USizeBox>> GeneratedSelectionSizeBoxes;

    UPROPERTY(Transient)
    TArray<TObjectPtr<USelectionButtonClickHandler>> GeneratedSelectionClickHandlers;
};
