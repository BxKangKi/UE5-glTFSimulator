// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/WorldSelectionButtonWidget.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "UI/WorldSelectionWidget.h"

namespace
{
    FString NormalizeWorldButtonText(FString Value)
    {
        Value.TrimStartAndEndInline();
        return Value;
    }
}

void UWorldSelectionButtonWidget::NativeConstruct()
{
    Super::NativeConstruct();
    ClearFlags(RF_Transactional);
    RefreshDisplayName();
    BindClickDelegate();
}

void UWorldSelectionButtonWidget::NativeDestruct()
{
    if (Level)
    {
        Level->OnClicked.RemoveDynamic(this, &UWorldSelectionButtonWidget::OpenAssignedWorld);
    }

    Super::NativeDestruct();
}

void UWorldSelectionButtonWidget::SetLevelButton(UButton* InLevelButton)
{
    if (Level == InLevelButton)
    {
        return;
    }

    if (Level)
    {
        Level->OnClicked.RemoveDynamic(this, &UWorldSelectionButtonWidget::OpenAssignedWorld);
    }

    Level = InLevelButton;
    BindClickDelegate();
}

void UWorldSelectionButtonWidget::SetNameTextBlock(UTextBlock* InNameTextBlock)
{
    Name = InNameTextBlock;
    RefreshDisplayName();
}

void UWorldSelectionButtonWidget::SetupWorldButton(UWorldSelectionWidget* InOwner, const FString& InWorldFolderName, const FString& InDisplayName)
{
    OwningWorldSelectionWidget = InOwner;
    WorldFolderName = NormalizeWorldButtonText(InWorldFolderName);
    WorldDisplayName = NormalizeWorldButtonText(InDisplayName);
    if (WorldDisplayName.IsEmpty())
    {
        WorldDisplayName = WorldFolderName;
    }

    RefreshDisplayName();
    BindClickDelegate();
}

void UWorldSelectionButtonWidget::OpenAssignedWorld()
{
    if (UWorldSelectionWidget* Owner = OwningWorldSelectionWidget.Get())
    {
        Owner->OpenWorldByFolderName(WorldFolderName);
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("WorldSelectionButtonWidget cannot open %s because the owning selection widget is not assigned."), *WorldFolderName);
}

void UWorldSelectionButtonWidget::BindClickDelegate()
{
    if (!Level)
    {
        return;
    }

    Level->OnClicked.RemoveDynamic(this, &UWorldSelectionButtonWidget::OpenAssignedWorld);
    Level->OnClicked.AddDynamic(this, &UWorldSelectionButtonWidget::OpenAssignedWorld);
}

void UWorldSelectionButtonWidget::RefreshDisplayName()
{
    if (!Name)
    {
        return;
    }

    const FString TextToShow = WorldDisplayName.IsEmpty() ? WorldFolderName : WorldDisplayName;
    Name->SetText(FText::FromString(TextToShow));
}
