#include "UI/ChatWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Texture2D.h"

UChatWidget::UChatWidget(const FObjectInitializer& O):Super(O){SetIsFocusable(true);}
TSharedRef<SWidget> UChatWidget::RebuildWidget()
{
    if (WidgetTree && !WidgetTree->RootWidget)
    {
        USizeBox* Size=WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(),TEXT("ChatSize")); Size->SetWidthOverride(PanelWidth); Size->SetHeightOverride(PanelHeight);
        UBorder* Border=WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(),TEXT("ChatBorder")); Border->SetPadding(FMargin(10)); Border->SetBrushColor(BackgroundTint); if(BackgroundTexture) Border->SetBrushFromTexture(BackgroundTexture);
        UVerticalBox* Column=WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(),TEXT("ChatColumn"));
        ChatLog=WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(),TEXT("ChatLog"));
        ChatInput=WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(),TEXT("ChatInput")); ChatInput->SetHintText(InputHint);
        Column->AddChildToVerticalBox(ChatLog)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        UVerticalBoxSlot* InputSlot=Column->AddChildToVerticalBox(ChatInput); InputSlot->SetPadding(FMargin(0,8,0,0)); InputSlot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
        Border->SetContent(Column); Size->AddChild(Border); WidgetTree->RootWidget=Size;
    }
    return Super::RebuildWidget();
}
void UChatWidget::NativeConstruct(){Super::NativeConstruct(); if(ChatInput){ChatInput->OnTextCommitted.RemoveAll(this);ChatInput->OnTextCommitted.AddDynamic(this,&UChatWidget::HandleCommitted);}}
void UChatWidget::HandleCommitted(const FText& Text,ETextCommit::Type Method){if(Method!=ETextCommit::OnEnter)return;const FString V=Text.ToString().TrimStartAndEnd();if(!V.IsEmpty()){OnTextSubmitted.Broadcast(V);ChatInput->SetText(FText::GetEmpty());}}
void UChatWidget::FocusInput(){if(ChatInput)ChatInput->SetKeyboardFocus();}
void UChatWidget::AddMessage(const FString& Message){if(!ChatLog)return;UTextBlock* Line=WidgetTree->ConstructWidget<UTextBlock>();Line->SetText(FText::FromString(Message));Line->SetColorAndOpacity(FSlateColor(TextColor));Line->SetAutoWrapText(true);ChatLog->AddChild(Line);ChatLog->ScrollToEnd();}
FReply UChatWidget::NativeOnKeyDown(const FGeometry& G,const FKeyEvent& E){if(E.GetKey()==EKeys::Escape){OnCloseRequested.Broadcast();return FReply::Handled();}return Super::NativeOnKeyDown(G,E);}
