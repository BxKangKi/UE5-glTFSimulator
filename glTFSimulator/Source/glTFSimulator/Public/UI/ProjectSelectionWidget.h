// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UI/SelectionWidgetBase.h"
#include "ProjectSelectionWidget.generated.h"

class UButton;
class UPanelWidget;
class UStartWorldWidget;
class UBuildStatusWidget;

/**
 * Project browser shown directly over the current start menu.
 *
 * Shared generated-entry styling and lifecycle are provided by USelectionWidgetBase. Blueprint
 * owns this top-level widget instance; native code discovers valid Projects/<Project> folders,
 * populates the assigned panel, and starts the normal project -> .gwd build pipeline.
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

    /** Optional explicit BuildStatus widget. If unset, BuildStatusWidgetClass is loaded from the central registry. */
    UFUNCTION(BlueprintCallable, Category="Projects|Widgets")
    void SetBuildStatusWidget(UBuildStatusWidget* InWidget);

    UFUNCTION(BlueprintPure, Category="Projects|Widgets")
    UBuildStatusWidget* GetBuildStatusWidget() const { return BuildStatusWidget.Get(); }

    /** Re-scan glTFSimulator/Projects and rebuild the generated project list. */
    UFUNCTION(BlueprintCallable, Category="Projects")
    void RefreshProjects();

    /** Start building one project into Worlds/<Project>.gwd. */
    UFUNCTION(BlueprintCallable, Category="Projects")
    bool BuildProjectByName(const FString& ProjectName);

    /** Hide this registered widget and restore the start widget. */
    UFUNCTION(BlueprintCallable, Category="Projects")
    void CloseProjectSelection();

protected:
    virtual void OnSelectionEntryActivated(const FString& SelectionKey) override;

private:
    void BindNavigationButtons();
    void UnbindNavigationButtons();
    UBuildStatusWidget* EnsureBuildStatusWidget();
    void ShowBuildStatus(const FString& ProjectName);

    UFUNCTION()
    void HandleBuildStatusCloseRequested();

    TWeakObjectPtr<UStartWorldWidget> OwnerStartWidget;
    TWeakObjectPtr<UButton> BackButton;
    TWeakObjectPtr<UButton> RefreshButton;

    UPROPERTY(Transient)
    TObjectPtr<UBuildStatusWidget> BuildStatusWidget;
};
