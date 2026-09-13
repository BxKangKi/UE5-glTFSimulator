// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "UI/ProjectSelectionWidget.h"

#include "Components/Button.h"
#include "Components/PanelWidget.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "System/FileFunctionLibrary.h"
#include "System/GameManagerSubSystem.h"
#include "System/GlTFSimulatorAssetRegistry.h"
#include "System/GlTFSimulatorGameInstance.h"
#include "UI/BuildStatusWidget.h"
#include "System/MacroLibrary.h"
#include "System/ProjectConfig.h"
#include "UI/StartWorldWidget.h"
#include "World/WorldData.h"

void UProjectSelectionWidget::NativeConstruct()
{
    Super::NativeConstruct();
    ClearFlags(RF_Transactional);
    BindNavigationButtons();

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
    }

    if (UStartWorldWidget* Owner = OwnerStartWidget.Get())
    {
        Owner->HandleProjectSelectionWidgetRemoved(this);
    }
    OwnerStartWidget.Reset();
    BackButton.Reset();
    RefreshButton.Reset();
    BuildStatusWidget = nullptr;

    Super::NativeDestruct();
}

void UProjectSelectionWidget::SetOwnerStartWidget(UStartWorldWidget* InOwnerWidget)
{
    OwnerStartWidget = InOwnerWidget;
}

void UProjectSelectionWidget::SetProjectListPanel(UPanelWidget* InPanel)
{
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
    }
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
    ClearGeneratedSelectionEntries();

    if (!IsValid(GetSelectionListPanel()))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("ProjectSelectionWidget requires SetProjectListPanel() from the derived WBP."));
        return;
    }

    TArray<FString> ProjectFolders;
    if (!UFileFunctionLibrary::GetSubFolders(PATH_PROJECTS, ProjectFolders))
    {
        return;
    }

    ProjectFolders.Sort([](const FString& A, const FString& B)
    {
        return A.Compare(B, ESearchCase::IgnoreCase) < 0;
    });

    for (const FString& Folder : ProjectFolders)
    {
        FString SafeName;
        if (!UGameManagerSubSystem::TryNormalizeWorldFolderName(Folder, SafeName, false)
            || SafeName != Folder)
        {
            continue;
        }

        const FString Root = FPaths::Combine(PATH_PROJECTS, SafeName);
        const FString ConfigPath = FPaths::Combine(Root, LEVEL_FILE_NAME);
        const FString ResourcesPath = FPaths::Combine(Root, TEXT("resources"));
        if (!IFileManager::Get().FileExists(*ConfigPath)
            || !IFileManager::Get().DirectoryExists(*ResourcesPath))
        {
            continue;
        }

        FGlTFSimulatorProjectConfig ProjectConfig;
        FString ConfigError;
        if (!GlTFSimulatorProjectConfig::Load(
                ConfigPath, SafeName, ProjectConfig, nullptr, ConfigError))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("Project '%s' is hidden because config.json is invalid: %s"),
                *SafeName, *ConfigError);
            continue;
        }

        const FString DisplayName = FString::Printf(
            TEXT("%s [%s]"),
            *ProjectConfig.GetDisplayName(SafeName),
            *GlTFSimulatorProjectConfig::ToString(ProjectConfig.ProjectType));
        if (!IsValid(AddGeneratedSelectionEntry(SafeName, DisplayName)))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("ProjectSelectionWidget failed to create a generated button for %s."),
                *SafeName);
        }
    }
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
    if (UStartWorldWidget* Owner = OwnerStartWidget.Get())
    {
        Owner->CloseProjectSelectionWidget();
        return;
    }

    SetVisibility(ESlateVisibility::Collapsed);
}

void UProjectSelectionWidget::OnSelectionEntryActivated(const FString& SelectionKey)
{
    if (BuildProjectByName(SelectionKey))
    {
        ShowBuildStatus(SelectionKey);
    }
}
