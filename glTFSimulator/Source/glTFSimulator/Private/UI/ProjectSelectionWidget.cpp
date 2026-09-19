// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/ProjectSelectionWidget.h"

#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/PanelWidget.h"
#include "Components/ScrollBox.h"
#include "Blueprint/WidgetTree.h"
#include "GameFramework/PlayerController.h"
#include "System/GameManagerSubSystem.h"
#include "System/GlTFSimulatorAssetRegistry.h"
#include "System/GlTFSimulatorGameInstance.h"
#include "UI/BuildStatusWidget.h"
#include "UI/MenuButtonWidget.h"
#include "System/MacroLibrary.h"
#include "System/ProjectConfig.h"
#include "UI/StartWorldWidget.h"
#include "World/WorldData.h"

namespace
{
    const FString CreateProjectSelectionKey = TEXT("__create_project__");
}

void UProjectSelectionWidget::NativeConstruct()
{
    Super::NativeConstruct();
    ClearFlags(RF_Transactional);
    BindNavigationButtons();
    SetCreateProjectMode(false, false);

    if (IsValid(GetSelectionListPanel()))
    {
        RefreshProjects();
    }
}

void UProjectSelectionWidget::NativeDestruct()
{
    UnbindNavigationButtons();
    if (IsValid(BuildStatusWidget))
    {
        BuildStatusWidget->OnConfirmed.RemoveDynamic(
            this, &UProjectSelectionWidget::HandleBuildStatusConfirmed);
        BuildStatusWidget->StopObservingBuild();
        if (bGeneratedBuildStatusWidget)
        {
            BuildStatusWidget->RemoveFromParent();
        }
    }
    bGeneratedBuildStatusWidget = false;

    if (UStartWorldWidget* Owner = OwnerStartWidget.Get())
    {
        Owner->HandleProjectSelectionWidgetRemoved(this);
    }
    SetCreateProjectMode(false, true);
    CreateProjectEntryWidget.Reset();
    OwnerStartWidget.Reset();
    BackButton.Reset();
    RefreshButton.Reset();
    if (bOwnsGeneratedProjectNameTextBox)
    {
        if (UEditableTextBox* Generated = NewProjectNameTextBox.Get())
        {
            Generated->RemoveFromParent();
        }
    }
    NewProjectNameTextBox.Reset();
    bOwnsGeneratedProjectNameTextBox = false;
    BuildStatusWidget = nullptr;

    Super::NativeDestruct();
}

void UProjectSelectionWidget::SetOwnerStartWidget(UStartWorldWidget* InOwnerWidget)
{
    OwnerStartWidget = InOwnerWidget;
}

void UProjectSelectionWidget::SetProjectListPanel(UPanelWidget* InPanel)
{
    SetCreateProjectMode(false, true);
    CreateProjectEntryWidget.Reset();
    if (bOwnsGeneratedProjectNameTextBox)
    {
        if (UEditableTextBox* Generated = NewProjectNameTextBox.Get())
        {
            Generated->RemoveFromParent();
        }
        NewProjectNameTextBox.Reset();
        bOwnsGeneratedProjectNameTextBox = false;
    }
    SetSelectionListPanel(InPanel);
    RefreshProjects();
}

void UProjectSelectionWidget::SetBackButton(UButton* InButton)
{
    if (UButton* Existing = BackButton.Get())
    {
        Existing->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::CloseProjectSelection);
    }

    BackButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::CloseProjectSelection);
        InButton->OnClicked.AddDynamic(this, &UProjectSelectionWidget::CloseProjectSelection);
    }
}

void UProjectSelectionWidget::SetRefreshButton(UButton* InButton)
{
    if (UButton* Existing = RefreshButton.Get())
    {
        Existing->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::RefreshProjects);
    }

    RefreshButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::RefreshProjects);
        InButton->OnClicked.AddDynamic(this, &UProjectSelectionWidget::RefreshProjects);
    }
}

void UProjectSelectionWidget::SetNewProjectNameTextBox(UEditableTextBox* InTextBox)
{
    if (bOwnsGeneratedProjectNameTextBox)
    {
        if (UEditableTextBox* Generated = NewProjectNameTextBox.Get(); Generated && Generated != InTextBox)
        {
            Generated->RemoveFromParent();
        }
    }
    NewProjectNameTextBox = InTextBox;
    bOwnsGeneratedProjectNameTextBox = false;
    if (IsValid(InTextBox))
    {
        if (UPanelWidget* Panel = GetSelectionListPanel(); IsValid(Panel) && Panel->HasChild(InTextBox))
        {
            // Keep even a Blueprint-provided text box at child index 0. This avoids the first-open
            // bottom placement that occurred when the input was appended after generated buttons.
            Panel->ShiftChild(0, InTextBox);
        }
        InTextBox->SetIsEnabled(bCreateProjectMode);
        InTextBox->SetVisibility(bCreateProjectMode ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    }
}

UEditableTextBox* UProjectSelectionWidget::EnsureNewProjectNameTextBox()
{
    if (UEditableTextBox* Existing = NewProjectNameTextBox.Get())
    {
        if (UPanelWidget* Panel = GetSelectionListPanel(); IsValid(Panel) && Panel->HasChild(Existing))
        {
            Panel->ShiftChild(0, Existing);
        }
        return Existing;
    }

    UPanelWidget* Panel = GetSelectionListPanel();
    if (!IsValid(Panel) || !IsValid(WidgetTree))
    {
        return nullptr;
    }

    UEditableTextBox* Generated = WidgetTree->ConstructWidget<UEditableTextBox>(
        UEditableTextBox::StaticClass(), TEXT("GeneratedNewProjectNameTextBox"));
    if (!IsValid(Generated))
    {
        return nullptr;
    }
    Generated->SetHintText(FText::FromString(TEXT("Project name")));
    Generated->SetIsEnabled(false);
    Generated->SetVisibility(ESlateVisibility::Collapsed);
    Panel->AddChild(Generated);
    NewProjectNameTextBox = Generated;
    bOwnsGeneratedProjectNameTextBox = true;
    return Generated;
}

void UProjectSelectionWidget::SetCreateProjectMode(const bool bEnabled, const bool bClearText)
{
    bCreateProjectMode = bEnabled;

    if (UEditableTextBox* TextBox = NewProjectNameTextBox.Get())
    {
        if (bClearText)
        {
            TextBox->SetText(FText::GetEmpty());
        }
        TextBox->SetIsEnabled(bCreateProjectMode);
        TextBox->SetVisibility(bCreateProjectMode ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    }

    UpdateCreateProjectEntryLabel();
}

void UProjectSelectionWidget::UpdateCreateProjectEntryLabel()
{
    if (UMenuButtonWidget* Entry = CreateProjectEntryWidget.Get())
    {
        Entry->ConfigureButton(
            CreateProjectSelectionKey,
            bCreateProjectMode ? TEXT("Done") : TEXT("Create Project"));
    }
}

void UProjectSelectionWidget::SetBuildStatusWidget(UBuildStatusWidget* InWidget)
{
    if (BuildStatusWidget == InWidget)
    {
        return;
    }
    if (IsValid(BuildStatusWidget))
    {
        BuildStatusWidget->OnConfirmed.RemoveDynamic(
            this, &UProjectSelectionWidget::HandleBuildStatusConfirmed);
        if (bGeneratedBuildStatusWidget)
        {
            BuildStatusWidget->RemoveFromParent();
        }
    }
    bGeneratedBuildStatusWidget = false;
    BuildStatusWidget = InWidget;
    if (IsValid(BuildStatusWidget))
    {
        BuildStatusWidget->OnConfirmed.RemoveDynamic(
            this, &UProjectSelectionWidget::HandleBuildStatusConfirmed);
        BuildStatusWidget->OnConfirmed.AddDynamic(
            this, &UProjectSelectionWidget::HandleBuildStatusConfirmed);
        BuildStatusWidget->SetVisibility(ESlateVisibility::Collapsed);
    }
}

UBuildStatusWidget* UProjectSelectionWidget::EnsureBuildStatusWidget()
{
    if (IsValid(BuildStatusWidget))
    {
        return BuildStatusWidget.Get();
    }

    APlayerController* PlayerController = GetOwningPlayer();
    UGlTFSimulatorAssetRegistry* Registry =
        UGlTFSimulatorGameInstance::GetAssetRegistryFromContext(this);
    if (!IsValid(PlayerController) || !IsValid(Registry)
        || Registry->BuildStatusWidgetClass.IsNull())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Project build progress UI requires BuildStatusWidgetClass in the central AssetRegistry ")
            TEXT("or SetBuildStatusWidget() from Blueprint."));
        return nullptr;
    }

    UClass* WidgetClass = Registry->BuildStatusWidgetClass.LoadSynchronous();
    if (!IsValid(WidgetClass) || !WidgetClass->IsChildOf(UBuildStatusWidget::StaticClass()))
    {
        UE_LOG(LogTemp, Error, TEXT("Configured BuildStatusWidgetClass is invalid."));
        return nullptr;
    }

    UBuildStatusWidget* Widget = CreateWidget<UBuildStatusWidget>(PlayerController, WidgetClass);
    if (!IsValid(Widget))
    {
        return nullptr;
    }
    Widget->AddToPlayerScreen(40);
    SetBuildStatusWidget(Widget);
    bGeneratedBuildStatusWidget = true;
    return BuildStatusWidget.Get();
}

void UProjectSelectionWidget::ShowBuildStatus(const FString& ProjectName)
{
    if (UBuildStatusWidget* Widget = EnsureBuildStatusWidget())
    {
        SetVisibility(ESlateVisibility::Collapsed);
        Widget->BeginObservingBuild(ProjectName);
        Widget->SetVisibility(ESlateVisibility::Visible);
    }
}

void UProjectSelectionWidget::HandleBuildStatusConfirmed()
{
    if (IsValid(BuildStatusWidget))
    {
        BuildStatusWidget->SetVisibility(ESlateVisibility::Collapsed);
        BuildStatusWidget->StopObservingBuild();
    }
    SetVisibility(ESlateVisibility::Visible);
    RefreshProjects();
}

void UProjectSelectionWidget::BindNavigationButtons()
{
    if (UButton* Button = BackButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::CloseProjectSelection);
        Button->OnClicked.AddDynamic(this, &UProjectSelectionWidget::CloseProjectSelection);
    }

    if (UButton* Button = RefreshButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::RefreshProjects);
        Button->OnClicked.AddDynamic(this, &UProjectSelectionWidget::RefreshProjects);
    }
}

void UProjectSelectionWidget::UnbindNavigationButtons()
{
    if (UButton* Button = BackButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::CloseProjectSelection);
    }

    if (UButton* Button = RefreshButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UProjectSelectionWidget::RefreshProjects);
    }
}

void UProjectSelectionWidget::RefreshProjects()
{
    SetCreateProjectMode(false, true);
    CreateProjectEntryWidget.Reset();
    ClearGeneratedSelectionEntries();

    if (!IsValid(GetSelectionListPanel()))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("ProjectSelectionWidget requires SetProjectListPanel() from the derived WBP."));
        return;
    }

    // Allocate the generated input while the panel is still empty. Keeping this collapsed child at
    // index 0 from the first refresh means its first Visible transition is already above every
    // generated project button. Previously it was first created only after clicking Create Project,
    // so AddChild() appended it at the bottom; the next refresh happened to move it to the top.
    // Pre-creating it here makes the layout deterministic from the very first use.
    EnsureNewProjectNameTextBox();

    // The create action is generated after the input, so the visible creation layout is always:
    // [Project Name TextBox] -> [Done/Create Project] -> [Project entries].
    UWidget* CreateEntry = AddGeneratedSelectionEntry(CreateProjectSelectionKey, TEXT("Create Project"));
    CreateProjectEntryWidget = Cast<UMenuButtonWidget>(CreateEntry);
    if (!IsValid(CreateEntry))
    {
        UE_LOG(LogTemp, Warning, TEXT("ProjectSelectionWidget could not create the project-creation entry."));
    }
    UpdateCreateProjectEntryLabel();

    UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this);
    if (!IsValid(Manager))
    {
        UE_LOG(LogTemp, Error, TEXT("ProjectSelectionWidget cannot enumerate projects because GameManagerSubSystem is unavailable."));
        return;
    }

    TArray<FGlTFSimulatorProjectSummary> Projects;
    Manager->GetProjectSummaries(Projects);
    for (const FGlTFSimulatorProjectSummary& Project : Projects)
    {
        const FString Label = FString::Printf(
            TEXT("%s [%s]"),
            *Project.DisplayName,
            *GlTFSimulatorProjectConfig::ToString(Project.ProjectType));
        if (!IsValid(AddGeneratedSelectionEntry(Project.FolderName, Label)))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("ProjectSelectionWidget failed to create a generated button for %s."),
                *Project.FolderName);
        }
    }
}

bool UProjectSelectionWidget::CreateNewProject(const FString& ProjectName)
{
    UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this);
    if (!IsValid(Manager) || !Manager->CreateProjectByName(ProjectName))
    {
        return false;
    }

    // Completing creation keeps ProjectSelection open. Only leave inline creation mode, clear and
    // collapse the TextBox, restore the Create Project label, then refresh so the new project appears
    // immediately in the same screen.
    SetCreateProjectMode(false, true);
    RefreshProjects();
    SetVisibility(ESlateVisibility::Visible);
    return true;
}

bool UProjectSelectionWidget::BuildProjectByName(const FString& ProjectName)
{
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        return Manager->BuildProjectByName(ProjectName);
    }

    UE_LOG(LogTemp, Error, TEXT("Project build failed because GameManagerSubSystem is unavailable."));
    return false;
}


bool UProjectSelectionWidget::GetProjectConfiguration(
    const FString& ProjectName,
    EGlTFSimulatorProjectType& OutProjectType,
    bool& bOutAllowExternalAssets,
    FString& OutDisplayName) const
{
    if (const UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        return Manager->GetProjectConfigurationByName(
            ProjectName, OutProjectType, bOutAllowExternalAssets, OutDisplayName);
    }
    return false;
}

bool UProjectSelectionWidget::SetProjectType(
    const FString& ProjectName,
    const EGlTFSimulatorProjectType ProjectType)
{
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        const bool bChanged = Manager->SetProjectTypeByName(ProjectName, ProjectType);
        if (bChanged) RefreshProjects();
        return bChanged;
    }
    return false;
}

bool UProjectSelectionWidget::SetWorldExternalAssetsAllowed(
    const FString& ProjectName,
    const bool bAllowed)
{
    if (UGameManagerSubSystem* Manager = UGameManagerSubSystem::GetSubSystem(this))
    {
        const bool bChanged = Manager->SetWorldExternalAssetsAllowedByName(ProjectName, bAllowed);
        if (bChanged) RefreshProjects();
        return bChanged;
    }
    return false;
}

void UProjectSelectionWidget::CloseProjectSelection()
{
    SetCreateProjectMode(false, true);

    if (UStartWorldWidget* Owner = OwnerStartWidget.Get())
    {
        Owner->CloseProjectSelectionWidget();
        return;
    }

    SetVisibility(ESlateVisibility::Collapsed);
}

void UProjectSelectionWidget::OnSelectionEntryActivated(const FString& SelectionKey)
{
    if (SelectionKey == CreateProjectSelectionKey)
    {
        if (!bCreateProjectMode)
        {
            // Rebuild generated entries AFTER the persistent input. Do this before
            // opening so runtime Slate child order never depends on ShiftChild.
            RefreshProjects();
            UEditableTextBox* TextBox = EnsureNewProjectNameTextBox();
            if (!IsValid(TextBox))
            {
                UE_LOG(LogTemp, Warning, TEXT("Create Project could not allocate its project-name input."));
                return;
            }

            SetCreateProjectMode(true, false);
            if (UScrollBox* Scroll = Cast<UScrollBox>(GetSelectionListPanel()))
            {
                Scroll->ScrollToStart();
            }
            TextBox->SetKeyboardFocus();
            return;
        }

        UEditableTextBox* TextBox = NewProjectNameTextBox.Get();
        const FString ProjectName = IsValid(TextBox)
            ? TextBox->GetText().ToString().TrimStartAndEnd()
            : FString();

        // Done always exits the inline creation UI. An empty name therefore acts like cancel; a
        // valid name is created before the list is refreshed.
        SetCreateProjectMode(false, false);
        if (!ProjectName.IsEmpty())
        {
            if (CreateNewProject(ProjectName))
            {
                return; // The list was refreshed in-place; ProjectSelection remains visible.
            }

            UE_LOG(LogTemp, Warning, TEXT("Project creation failed for '%s'."), *ProjectName);
        }

        SetCreateProjectMode(false, true);
        return;
    }

    // Selecting an existing project also leaves inline creation mode before the build/status UI
    // takes over, so the text box never remains visible behind another screen.
    SetCreateProjectMode(false, true);
    if (BuildProjectByName(SelectionKey))
    {
        ShowBuildStatus(SelectionKey);
    }
}
