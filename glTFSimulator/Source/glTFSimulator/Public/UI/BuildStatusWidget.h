// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "BuildStatusWidget.generated.h"

class UButton;
class UGameManagerSubSystem;
class UProgressBar;
class UTextBlock;

UENUM(BlueprintType)
enum class EProjectBuildPhase : uint8
{
    Idle UMETA(DisplayName="Idle"),
    Preparing UMETA(DisplayName="Preparing"),
    BuildingModels UMETA(DisplayName="Building Models"),
    WritingArchive UMETA(DisplayName="Writing Archive"),
    Completed UMETA(DisplayName="Completed"),
    Failed UMETA(DisplayName="Failed")
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
    FBuildStatusUpdated, float, Progress, const FString&, Message, EProjectBuildPhase, Phase);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FBuildStatusFinished, bool, bSuccess, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FBuildStatusCloseRequested);

/**
 * Blueprint-facing project build progress surface.
 *
 * A WBP subclass owns layout only. From Construct, pass the TextBlocks/ProgressBar/Button through
 * the Set* functions below. The native widget subscribes to GameManager build delegates and keeps
 * those ordinary UMG widgets synchronized without requiring tick graphs.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UBuildStatusWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    virtual void NativeDestruct() override;

    UPROPERTY(BlueprintAssignable, Category="Build Status")
    FBuildStatusUpdated OnBuildStatusUpdated;

    UPROPERTY(BlueprintAssignable, Category="Build Status")
    FBuildStatusFinished OnBuildFinished;

    UPROPERTY(BlueprintAssignable, Category="Build Status")
    FBuildStatusCloseRequested OnCloseRequested;

    UFUNCTION(BlueprintCallable, Category="Build Status|Widgets")
    void SetStatusTextBlock(UTextBlock* InTextBlock);

    UFUNCTION(BlueprintCallable, Category="Build Status|Widgets")
    void SetProgressBar(UProgressBar* InProgressBar);

    UFUNCTION(BlueprintCallable, Category="Build Status|Widgets")
    void SetPercentTextBlock(UTextBlock* InTextBlock);

    UFUNCTION(BlueprintCallable, Category="Build Status|Widgets")
    void SetProjectNameTextBlock(UTextBlock* InTextBlock);

    UFUNCTION(BlueprintCallable, Category="Build Status|Widgets")
    void SetCloseButton(UButton* InButton);

    /** Starts observing the already-started Projects build. Safe to call after BuildProjectByName. */
    UFUNCTION(BlueprintCallable, Category="Build Status")
    void BeginObservingBuild(const FString& ProjectName);

    UFUNCTION(BlueprintCallable, Category="Build Status")
    void StopObservingBuild();

    UFUNCTION(BlueprintCallable, Category="Build Status")
    void RefreshBuildStatus();

    UFUNCTION(BlueprintCallable, Category="Build Status")
    void RequestClose();

    UFUNCTION(BlueprintPure, Category="Build Status")
    float GetDisplayedProgress() const { return DisplayedProgress; }

    UFUNCTION(BlueprintPure, Category="Build Status")
    FString GetDisplayedMessage() const { return DisplayedMessage; }

    UFUNCTION(BlueprintPure, Category="Build Status")
    EProjectBuildPhase GetBuildPhase() const { return BuildPhase; }

    UFUNCTION(BlueprintPure, Category="Build Status")
    bool IsBuildActive() const;

    UFUNCTION(BlueprintPure, Category="Build Status")
    FString GetObservedProjectName() const { return ObservedProjectName; }

private:
    UFUNCTION()
    void HandleBuildProgress(float Progress);

    UFUNCTION()
    void HandleBuildCompleted(bool bSuccess, const FString& Message);

    UFUNCTION()
    void HandleManagerStateChanged();

    void BindManager(UGameManagerSubSystem* InManager);
    void UnbindManager();
    void UpdateAssignedWidgets();
    EProjectBuildPhase ResolvePhase() const;

    UPROPERTY(Transient)
    TObjectPtr<UGameManagerSubSystem> Manager;

    TWeakObjectPtr<UTextBlock> StatusTextBlock;
    TWeakObjectPtr<UProgressBar> AssignedProgressBar;
    TWeakObjectPtr<UTextBlock> PercentTextBlock;
    TWeakObjectPtr<UTextBlock> ProjectNameTextBlock;
    TWeakObjectPtr<UButton> CloseButton;

    FString ObservedProjectName;
    FString DisplayedMessage;
    float DisplayedProgress = 0.0f;
    EProjectBuildPhase BuildPhase = EProjectBuildPhase::Idle;
    bool bLastBuildSucceeded = false;
    bool bHasTerminalResult = false;
};
