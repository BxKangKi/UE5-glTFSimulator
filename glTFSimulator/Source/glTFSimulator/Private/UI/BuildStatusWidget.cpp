// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/BuildStatusWidget.h"

#include "Components/Button.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "System/GameManagerSubSystem.h"

void UBuildStatusWidget::NativeDestruct()
{
    UnbindManager();
    if (UButton* Button = CloseButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UBuildStatusWidget::RequestClose);
    }
    CloseButton.Reset();
    StatusTextBlock.Reset();
    AssignedProgressBar.Reset();
    PercentTextBlock.Reset();
    ProjectNameTextBlock.Reset();
    Super::NativeDestruct();
}

void UBuildStatusWidget::SetStatusTextBlock(UTextBlock* InTextBlock)
{
    StatusTextBlock = InTextBlock;
    UpdateAssignedWidgets();
}

void UBuildStatusWidget::SetProgressBar(UProgressBar* InProgressBar)
{
    AssignedProgressBar = InProgressBar;
    UpdateAssignedWidgets();
}

void UBuildStatusWidget::SetPercentTextBlock(UTextBlock* InTextBlock)
{
    PercentTextBlock = InTextBlock;
    UpdateAssignedWidgets();
}

void UBuildStatusWidget::SetProjectNameTextBlock(UTextBlock* InTextBlock)
{
    ProjectNameTextBlock = InTextBlock;
    UpdateAssignedWidgets();
}

void UBuildStatusWidget::SetCloseButton(UButton* InButton)
{
    if (UButton* Existing = CloseButton.Get())
    {
        Existing->OnClicked.RemoveDynamic(this, &UBuildStatusWidget::RequestClose);
    }
    CloseButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UBuildStatusWidget::RequestClose);
        InButton->OnClicked.AddDynamic(this, &UBuildStatusWidget::RequestClose);
    }
    UpdateAssignedWidgets();
}

void UBuildStatusWidget::BeginObservingBuild(const FString& ProjectName)
{
    ObservedProjectName = ProjectName.TrimStartAndEnd();
    bHasTerminalResult = false;
    bLastBuildSucceeded = false;
    DisplayedProgress = 0.0f;
    BindManager(UGameManagerSubSystem::GetSubSystem(this));
    SetVisibility(ESlateVisibility::Visible);
    RefreshBuildStatus();
}

void UBuildStatusWidget::StopObservingBuild()
{
    UnbindManager();
    ObservedProjectName.Reset();
    BuildPhase = EProjectBuildPhase::Idle;
}

bool UBuildStatusWidget::IsBuildActive() const
{
    return IsValid(Manager) && Manager->IsProjectBuildInProgress();
}

void UBuildStatusWidget::BindManager(UGameManagerSubSystem* InManager)
{
    if (Manager == InManager)
    {
        return;
    }
    UnbindManager();
    Manager = InManager;
    if (IsValid(Manager))
    {
        Manager->OnWorldBakeProgress.RemoveDynamic(this, &UBuildStatusWidget::HandleBuildProgress);
        Manager->OnWorldBakeProgress.AddDynamic(this, &UBuildStatusWidget::HandleBuildProgress);
        Manager->OnWorldBakeCompleted.RemoveDynamic(this, &UBuildStatusWidget::HandleBuildCompleted);
        Manager->OnWorldBakeCompleted.AddDynamic(this, &UBuildStatusWidget::HandleBuildCompleted);
        Manager->OnStateChanged.RemoveDynamic(this, &UBuildStatusWidget::HandleManagerStateChanged);
        Manager->OnStateChanged.AddDynamic(this, &UBuildStatusWidget::HandleManagerStateChanged);
    }
}

void UBuildStatusWidget::UnbindManager()
{
    if (IsValid(Manager))
    {
        Manager->OnWorldBakeProgress.RemoveDynamic(this, &UBuildStatusWidget::HandleBuildProgress);
        Manager->OnWorldBakeCompleted.RemoveDynamic(this, &UBuildStatusWidget::HandleBuildCompleted);
        Manager->OnStateChanged.RemoveDynamic(this, &UBuildStatusWidget::HandleManagerStateChanged);
    }
    Manager = nullptr;
}

void UBuildStatusWidget::HandleBuildProgress(const float Progress)
{
    DisplayedProgress = FMath::Max(DisplayedProgress, FMath::Clamp(Progress, 0.0f, 1.0f));
    bHasTerminalResult = false;
    RefreshBuildStatus();
}

void UBuildStatusWidget::HandleBuildCompleted(const bool bSuccess, const FString& Message)
{
    bHasTerminalResult = true;
    bLastBuildSucceeded = bSuccess;
    DisplayedProgress = 1.0f;
    DisplayedMessage = Message;
    BuildPhase = bSuccess ? EProjectBuildPhase::Completed : EProjectBuildPhase::Failed;
    UpdateAssignedWidgets();
    OnBuildStatusUpdated.Broadcast(DisplayedProgress, DisplayedMessage, BuildPhase);
    OnBuildFinished.Broadcast(bSuccess, DisplayedMessage);
}

void UBuildStatusWidget::HandleManagerStateChanged()
{
    RefreshBuildStatus();
}

EProjectBuildPhase UBuildStatusWidget::ResolvePhase() const
{
    if (bHasTerminalResult)
    {
        return bLastBuildSucceeded ? EProjectBuildPhase::Completed : EProjectBuildPhase::Failed;
    }
    if (!IsValid(Manager) || !Manager->IsProjectBuildInProgress())
    {
        return EProjectBuildPhase::Idle;
    }
    if (!Manager->IsWorldBakeInProgress() || DisplayedProgress < 0.05f)
    {
        return EProjectBuildPhase::Preparing;
    }
    if (DisplayedProgress >= 0.95f)
    {
        return EProjectBuildPhase::WritingArchive;
    }
    return EProjectBuildPhase::BuildingModels;
}

void UBuildStatusWidget::RefreshBuildStatus()
{
    if (!IsValid(Manager))
    {
        BindManager(UGameManagerSubSystem::GetSubSystem(this));
    }
    if (IsValid(Manager))
    {
        DisplayedProgress = FMath::Max(
            DisplayedProgress,
            FMath::Clamp(Manager->GetWorldBakeProgress(), 0.0f, 1.0f));
        const FString Message = Manager->GetLastMessage();
        if (!Message.IsEmpty())
        {
            DisplayedMessage = Message;
        }
    }
    BuildPhase = ResolvePhase();
    UpdateAssignedWidgets();
    OnBuildStatusUpdated.Broadcast(DisplayedProgress, DisplayedMessage, BuildPhase);
}

void UBuildStatusWidget::UpdateAssignedWidgets()
{
    if (UTextBlock* Text = StatusTextBlock.Get())
    {
        Text->SetText(FText::FromString(DisplayedMessage));
    }
    if (UProgressBar* Progress = AssignedProgressBar.Get())
    {
        Progress->SetPercent(FMath::Clamp(DisplayedProgress, 0.0f, 1.0f));
    }
    if (UTextBlock* Text = PercentTextBlock.Get())
    {
        Text->SetText(FText::FromString(FString::Printf(
            TEXT("%d%%"), FMath::RoundToInt(FMath::Clamp(DisplayedProgress, 0.0f, 1.0f) * 100.0f))));
    }
    if (UTextBlock* Text = ProjectNameTextBlock.Get())
    {
        Text->SetText(FText::FromString(ObservedProjectName));
    }
    if (UButton* Button = CloseButton.Get())
    {
        // Keep the build visible by default. Blueprint may still hide the whole widget itself.
        Button->SetIsEnabled(!IsBuildActive());
    }
}

void UBuildStatusWidget::RequestClose()
{
    if (IsBuildActive())
    {
        return;
    }
    OnCloseRequested.Broadcast();
}
