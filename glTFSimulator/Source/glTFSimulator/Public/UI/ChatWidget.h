#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ChatWidget.generated.h"

class UBorder;
class UEditableTextBox;
class UScrollBox;
class UTexture2D;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FChatTextSubmitted, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FChatCloseRequested);

UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UChatWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    UChatWidget(const FObjectInitializer& ObjectInitializer);
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativeConstruct() override;
    virtual FReply NativeOnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent) override;

    UPROPERTY(BlueprintAssignable, Category="Chat|Events") FChatTextSubmitted OnTextSubmitted;
    UPROPERTY(BlueprintAssignable, Category="Chat|Events") FChatCloseRequested OnCloseRequested;

    // A Blueprint child only needs to assign these appearance assets/defaults. No graph or Designer hierarchy is required.
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Chat|Appearance") TObjectPtr<UTexture2D> BackgroundTexture;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Chat|Appearance") FLinearColor BackgroundTint = FLinearColor(0.015f,0.02f,0.03f,0.88f);
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Chat|Appearance") FLinearColor TextColor = FLinearColor::White;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Chat|Appearance", meta=(ClampMin="300", ClampMax="1200")) float PanelWidth = 620.0f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Chat|Appearance", meta=(ClampMin="120", ClampMax="800")) float PanelHeight = 260.0f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Chat|Appearance") FText InputHint = FText::FromString(TEXT("Message or /time HH:MM"));

    UFUNCTION(BlueprintCallable, Category="Chat") void FocusInput();
    UFUNCTION(BlueprintCallable, Category="Chat") void AddMessage(const FString& Message);
private:
    UFUNCTION() void HandleCommitted(const FText& Text, ETextCommit::Type CommitMethod);
    UPROPERTY(Transient) TObjectPtr<UScrollBox> ChatLog;
    UPROPERTY(Transient) TObjectPtr<UEditableTextBox> ChatInput;
};
