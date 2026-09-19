// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "System/ProjectConfig.h"
#include "UI/SelectionWidgetBase.h"
#include "ProjectSelectionWidget.generated.h"

class UButton;
class UEditableTextBox;
class UPanelWidget;
class UMenuButtonWidget;
class UStartWorldWidget;
class UBuildStatusWidget;

/**
 * Project browser shown directly over the current start menu.
 *
 * Generated-entry creation and lifecycle are provided by USelectionWidgetBase. Each entry is a
 * Blueprint-authored UMenuButtonWidget class; native code discovers valid Projects/<Project> folders,
 * populates the assigned panel, and starts the project-specific .gworld/.gasset build pipeline.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UProjectSelectionWidget : public USelectionWidgetBase
{
    GENERATED_BODY()

public:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    /** Internal owner assigned when this widget is registered with UStartWorldWidget. */
    void SetOwnerStartWidget(UStartWorldWidget* InOwnerWidget);

    /** Assign the panel that receives generated project buttons. */
    UFUNCTION(BlueprintCallable, Category="Projects|Widgets")
    void SetProjectListPanel(UPanelWidget* InPanel);

    /** Assign the Back button. */
    UFUNCTION(BlueprintCallable, Category="Projects|Widgets")
    void SetBackButton(UButton* InButton);

    /** Optional Refresh button. */
    UFUNCTION(BlueprintCallable, Category="Projects|Widgets")
    void SetRefreshButton(UButton* InButton);

    /** Text box used by the first generated "Create Project" entry. */
    UFUNCTION(BlueprintCallable, Category="Projects|Widgets")
    void SetNewProjectNameTextBox(UEditableTextBox* InTextBox);

    /** Optional explicit BuildStatus widget. If unset, BuildStatusWidgetClass is loaded from the central registry. */
    UFUNCTION(BlueprintCallable, Category="Projects|Widgets")
    void SetBuildStatusWidget(UBuildStatusWidget* InWidget);

    UFUNCTION(BlueprintPure, Category="Projects|Widgets")
    UBuildStatusWidget* GetBuildStatusWidget() const { return BuildStatusWidget.Get(); }

    /** Re-scan glTFSimulator/Projects and rebuild the generated project list. */
    UFUNCTION(BlueprintCallable, Category="Projects")
    void RefreshProjects();

    /** Creates Projects/<Name>/config.json and Projects/<Name>/resources, then refreshes the list. */
    UFUNCTION(BlueprintCallable, Category="Projects")
    bool CreateNewProject(const FString& ProjectName);

    /** Starts the selected project build. World projects emit .gworld; Character/Dynamic projects emit .gasset. */
    UFUNCTION(BlueprintCallable, Category="Projects")
    bool BuildProjectByName(const FString& ProjectName);

    UFUNCTION(BlueprintCallable, Category="Projects|Configuration")
    bool GetProjectConfiguration(
        const FString& ProjectName,
        EGlTFSimulatorProjectType& OutProjectType,
        bool& bOutAllowExternalAssets,
        FString& OutDisplayName) const;

    UFUNCTION(BlueprintCallable, Category="Projects|Configuration")
    bool SetProjectType(const FString& ProjectName, EGlTFSimulatorProjectType ProjectType);

    UFUNCTION(BlueprintCallable, Category="Projects|Configuration")
    bool SetWorldExternalAssetsAllowed(const FString& ProjectName, bool bAllowed);

    /** Hide this registered widget and restore the start widget. */
    UFUNCTION(BlueprintCallable, Category="Projects")
    void CloseProjectSelection();

protected:
    virtual void OnSelectionEntryActivated(const FString& SelectionKey) override;

private:
    void BindNavigationButtons();
    void UnbindNavigationButtons();
    UEditableTextBox* EnsureNewProjectNameTextBox();
    UBuildStatusWidget* EnsureBuildStatusWidget();
    void ShowBuildStatus(const FString& ProjectName);
    void SetCreateProjectMode(bool bEnabled, bool bClearText = false);
    void UpdateCreateProjectEntryLabel();

    UFUNCTION()
    void HandleBuildStatusConfirmed();

    TWeakObjectPtr<UStartWorldWidget> OwnerStartWidget;
    TWeakObjectPtr<UButton> BackButton;
    TWeakObjectPtr<UButton> RefreshButton;
    TWeakObjectPtr<UEditableTextBox> NewProjectNameTextBox;
    TWeakObjectPtr<UMenuButtonWidget> CreateProjectEntryWidget;
    bool bOwnsGeneratedProjectNameTextBox = false;
    bool bCreateProjectMode = false;

    UPROPERTY(Transient)
    TObjectPtr<UBuildStatusWidget> BuildStatusWidget;

    bool bGeneratedBuildStatusWidget = false;
};
