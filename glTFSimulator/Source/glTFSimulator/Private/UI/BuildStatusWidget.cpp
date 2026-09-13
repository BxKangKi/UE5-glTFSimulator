// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/BuildStatusWidget.h"

#include "Components/PanelWidget.h"
#include "Components/ProgressBar.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "System/GameManagerSubSystem.h"
#include "UI/MenuButtonWidget.h"

void UBuildStatusWidget::NativeDestruct()
{
    CancelAutoClose();
    UnbindManager();

    if (IsValid(ConfirmButtonWidget))
    {
        UnbindConfirmButtonWidget(ConfirmButtonWidget.Get());
    }
    ReleaseGeneratedConfirmButtonWidget();
    ConfirmButtonWidget = nullptr;

    ConfirmButtonHost.Reset();
    StatusTextBlock.Reset();
    OutputScrollBox.Reset();
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

void UBuildStatusWidget::SetOutputScrollBox(UScrollBox* InScrollBox)
{
    OutputScrollBox = InScrollBox;
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

void UBuildStatusWidget::ClearBuildOutput()
{
    OutputLines.Reset();
    BuildOutputText.Reset();
    UpdateAssignedWidgets();
}

void UBuildStatusWidget::SetConfirmButtonWidget(UMenuButtonWidget* InWidget)
{
    if (ConfirmButtonWidget.Get() == InWidget && !bGeneratedConfirmButtonWidget)
    {
        UpdateAssignedWidgets();
        return;
    }

    if (IsValid(ConfirmButtonWidget))
    {
        UnbindConfirmButtonWidget(ConfirmButtonWidget.Get());
    }
    ReleaseGeneratedConfirmButtonWidget();

    ConfirmButtonWidget = InWidget;
    bGeneratedConfirmButtonWidget = false;
    if (IsValid(InWidget))
    {
        BindConfirmButtonWidget(InWidget);
        InWidget->ConfigureButton(TEXT("Confirm"), TEXT("Confirm"));
    }

    UpdateAssignedWidgets();
}

void UBuildStatusWidget::SetConfirmButtonWidgetClass(TSubclassOf<UMenuButtonWidget> InWidgetClass)
{
    if (ConfirmButtonWidgetClass == InWidgetClass)
    {
        EnsureConfirmButtonWidget();
        UpdateAssignedWidgets();
        return;
    }

    ConfirmButtonWidgetClass = InWidgetClass;
    if (bGeneratedConfirmButtonWidget)
    {
        if (IsValid(ConfirmButtonWidget))
        {
            UnbindConfirmButtonWidget(ConfirmButtonWidget.Get());
        }
        ReleaseGeneratedConfirmButtonWidget();
    }

    EnsureConfirmButtonWidget();
    UpdateAssignedWidgets();
}

void UBuildStatusWidget::SetConfirmButtonHost(UPanelWidget* InHost)
{
    if (ConfirmButtonHost.Get() == InHost)
    {
        EnsureConfirmButtonWidget();
        UpdateAssignedWidgets();
        return;
    }

    if (bGeneratedConfirmButtonWidget)
    {
        if (IsValid(ConfirmButtonWidget))
        {
            UnbindConfirmButtonWidget(ConfirmButtonWidget.Get());
        }
        ReleaseGeneratedConfirmButtonWidget();
    }

    ConfirmButtonHost = InHost;
    EnsureConfirmButtonWidget();
    UpdateAssignedWidgets();
}

UMenuButtonWidget* UBuildStatusWidget::EnsureConfirmButtonWidget()
{
    if (IsValid(ConfirmButtonWidget))
    {
        return ConfirmButtonWidget.Get();
    }

    UPanelWidget* const Host = ConfirmButtonHost.Get();
    if (!IsValid(Host) || !ConfirmButtonWidgetClass)
    {
        return nullptr;
    }

    UMenuButtonWidget* const Widget = Cast<UMenuButtonWidget>(
        UUserWidget::CreateWidgetInstance(*this, ConfirmButtonWidgetClass, NAME_None));

    if (!IsValid(Widget))
    {
        UE_LOG(LogTemp, Warning, TEXT("BuildStatusWidget failed to create ConfirmButtonWidgetClass."));
        return nullptr;
    }

    Host->AddChild(Widget);
    ConfirmButtonWidget = Widget;
    bGeneratedConfirmButtonWidget = true;
    BindConfirmButtonWidget(Widget);
    Widget->ConfigureButton(TEXT("Confirm"), TEXT("Confirm"));
    return Widget;
}

void UBuildStatusWidget::ReleaseGeneratedConfirmButtonWidget()
{
    if (!bGeneratedConfirmButtonWidget)
    {
        return;
    }

    if (IsValid(ConfirmButtonWidget))
    {
        ConfirmButtonWidget->RemoveFromParent();
    }
    ConfirmButtonWidget = nullptr;
    bGeneratedConfirmButtonWidget = false;
}

void UBuildStatusWidget::BindConfirmButtonWidget(UMenuButtonWidget* InWidget)
{
    if (!IsValid(InWidget))
    {
        return;
    }

    InWidget->OnButtonClicked.RemoveDynamic(this, &UBuildStatusWidget::HandleConfirmButtonClicked);
    InWidget->OnButtonClicked.AddDynamic(this, &UBuildStatusWidget::HandleConfirmButtonClicked);
}

void UBuildStatusWidget::UnbindConfirmButtonWidget(UMenuButtonWidget* InWidget)
{
    if (IsValid(InWidget))
    {
        InWidget->OnButtonClicked.RemoveDynamic(this, &UBuildStatusWidget::HandleConfirmButtonClicked);
    }
}

void UBuildStatusWidget::BeginObservingBuild(const FString& ProjectName)
{
    CancelAutoClose();
    ObservedProjectName = ProjectName.TrimStartAndEnd();
    bHasTerminalResult = false;
    bLastBuildSucceeded = false;
    DisplayedProgress = 0.0f;
    DisplayedMessage = FString::Printf(TEXT("Preparing project build: %s"), *ObservedProjectName);
    BuildPhase = EProjectBuildPhase::Preparing;
    OutputLines.Reset();
    BuildOutputText.Reset();
    AppendOutputLine(DisplayedMessage);
    BindManager(UGameManagerSubSystem::GetSubSystem(this));
    SetVisibility(ESlateVisibility::Visible);
    RefreshBuildStatus();
}

void UBuildStatusWidget::StopObservingBuild()
{
    CancelAutoClose();
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
    CancelAutoClose();
    bHasTerminalResult = true;
    bLastBuildSucceeded = bSuccess;
    DisplayedProgress = 1.0f;
    DisplayedMessage = Message.IsEmpty()
        ? (bSuccess ? TEXT("Build completed successfully.") : TEXT("Build failed."))
        : Message;
    BuildPhase = bSuccess ? EProjectBuildPhase::Completed : EProjectBuildPhase::Failed;
    AppendOutputLine(DisplayedMessage);
    UpdateAssignedWidgets();
    OnBuildStatusUpdated.Broadcast(DisplayedProgress, DisplayedMessage, BuildPhase);
    OnBuildFinished.Broadcast(bSuccess, DisplayedMessage);

    if (bSuccess && bAutoCloseOnSuccess)
    {
        ScheduleAutoClose();
    }
}

void UBuildStatusWidget::HandleManagerStateChanged()
{
    RefreshBuildStatus();
}

void UBuildStatusWidget::HandleConfirmButtonClicked(const FString& ButtonKey)
{
    RequestConfirm();
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
        if (!Message.IsEmpty() && Message != DisplayedMessage)
        {
            DisplayedMessage = Message;
            AppendOutputLine(DisplayedMessage);
        }
    }
    BuildPhase = ResolvePhase();
    UpdateAssignedWidgets();
    OnBuildStatusUpdated.Broadcast(DisplayedProgress, DisplayedMessage, BuildPhase);
}

void UBuildStatusWidget::AppendOutputLine(const FString& Line)
{
    FString Normalized = Line;
    Normalized.TrimStartAndEndInline();
    if (Normalized.IsEmpty())
    {
        return;
    }

    // A state delegate may fire repeatedly while the same operation is active. Avoid duplicate lines.
    if (OutputLines.Num() > 0 && OutputLines.Last() == Normalized)
    {
        return;
    }

    // Preserve embedded newlines as separate terminal rows.
    TArray<FString> NewLines;
    Normalized.ParseIntoArrayLines(NewLines, false);
    if (NewLines.Num() == 0)
    {
        NewLines.Add(Normalized);
    }

    for (FString& NewLine : NewLines)
    {
        NewLine.TrimStartAndEndInline();
        if (!NewLine.IsEmpty() && (OutputLines.Num() == 0 || OutputLines.Last() != NewLine))
        {
            OutputLines.Add(MoveTemp(NewLine));
        }
    }

    const int32 SafeMaxLines = FMath::Clamp(MaxOutputLines, 1, 512);
    if (OutputLines.Num() > SafeMaxLines)
    {
        OutputLines.RemoveAt(0, OutputLines.Num() - SafeMaxLines, EAllowShrinking::No);
    }

    RebuildOutputText();
}

void UBuildStatusWidget::RebuildOutputText()
{
    BuildOutputText = FString::Join(OutputLines, TEXT("\n"));
}

void UBuildStatusWidget::ScheduleAutoClose()
{
    CancelAutoClose();

    if (!bHasTerminalResult || !bLastBuildSucceeded || !bAutoCloseOnSuccess)
    {
        return;
    }

    if (AutoCloseDelaySeconds <= KINDA_SMALL_NUMBER)
    {
        HandleAutoCloseTimer();
        return;
    }

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().SetTimer(
            AutoCloseTimerHandle,
            this,
            &UBuildStatusWidget::HandleAutoCloseTimer,
            AutoCloseDelaySeconds,
            false);
    }
}

void UBuildStatusWidget::CancelAutoClose()
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(AutoCloseTimerHandle);
    }
    AutoCloseTimerHandle.Invalidate();
}

void UBuildStatusWidget::HandleAutoCloseTimer()
{
    AutoCloseTimerHandle.Invalidate();
    if (bHasTerminalResult && bLastBuildSucceeded && !IsBuildActive())
    {
        RequestConfirm();
    }
}

void UBuildStatusWidget::UpdateAssignedWidgets()
{
    if (UTextBlock* Text = StatusTextBlock.Get())
    {
        Text->SetText(FText::FromString(BuildOutputText.IsEmpty() ? DisplayedMessage : BuildOutputText));
    }
    if (UScrollBox* ScrollBox = OutputScrollBox.Get())
    {
        ScrollBox->ScrollToEnd();
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

    if (UMenuButtonWidget* ConfirmWidget = EnsureConfirmButtonWidget())
    {
        const bool bShowConfirm = bHasTerminalResult && !IsBuildActive();
        ConfirmWidget->SetIsEnabled(bShowConfirm);
        ConfirmWidget->SetVisibility(bShowConfirm ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    }
}

void UBuildStatusWidget::RequestConfirm()
{
    if (IsBuildActive() || !bHasTerminalResult)
    {
        return;
    }

    CancelAutoClose();
    SetVisibility(ESlateVisibility::Collapsed);
    OnConfirmed.Broadcast();
    OnCloseRequested.Broadcast();

    // Keep the terminal result available during the callbacks, then detach from the manager.
    if (IsValid(Manager))
    {
        StopObservingBuild();
    }
}
