// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UI/WorldSelectionWidget.h"
#include "StartActor.generated.h"

/**
 * Owns the MainWorld menu flow.
 *
 * BP_StartWorld should inherit from this actor and explicitly assign each menu widget class.
 * WBP_StartWorld should inherit UStartWorldWidget, and WBP_LevelMenu should inherit
 * UWorldSelectionWidget so the native widget builds and handles the world buttons directly.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API AStartActor : public AActor
{
    GENERATED_BODY()

public:
    AStartActor();

    /** Opens the world-selection widget from the start screen. */
    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void StartGame();

    /** Shows the start screen widget. Bind this through UStartWorldWidget when the world-selection Back button is clicked. */
    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void ReturnToMainMenuFromWorldSelection();

    /** Rebuilds and shows the main start-screen widget. */
    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void ShowStartMenu();

    /** Rebuilds and shows the world-selection widget for single-player world travel. */
    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void ShowWorldSelectionMenu();

    /** Rebuilds and shows the multiplayer menu. Assign MultiplayerMenuWidgetClass in BP_StartWorld before using this. */
    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void ShowMultiplayerMenu();

    /** Re-scans available world folders and refreshes opened world-list widgets. */
    UFUNCTION(BlueprintCallable, Category="Start World|Data")
    void RefreshWorldFolderNameMap();

    /** Returns a map of world-folder names to display names for Blueprint world-list widgets. */
    UFUNCTION(BlueprintPure, Category="Start World|Data")
    TMap<FString, FString> GetFolderNameMap() const { return FolderNameMap; }

    /** Resolves a displayed level name back to its folder key. */
    UFUNCTION(BlueprintCallable, Category="Start World|Data")
    bool TryResolveWorldFolderFromDisplayName(const FString& DisplayName, FString& OutFolderName) const;

    /** Opens the single-player gameplay map after selecting a world folder from WBP_LevelMenu. */
    UFUNCTION(BlueprintCallable, Category="Start World|Navigation")
    void OpenSinglePlayerWorldByFolderName(const FString& WorldFolderName);

    UFUNCTION(BlueprintCallable, Category="Start World|Navigation")
    void HostMultiplayerWorldByFolderName(const FString& WorldFolderName);

    UFUNCTION(BlueprintCallable, Category="Start World|Navigation")
    void OpenClientConnectionWorld(const FString& InServerAddress);

    UFUNCTION(BlueprintCallable, Category="Start World|Navigation")
    void JoinMultiplayerServer(const FString& InServerAddress, const FString& OptionalWorldFolderName);

    UFUNCTION(BlueprintCallable, Category="Start World|Navigation")
    void SetPendingServerAddress(const FString& InServerAddress);

    UFUNCTION(BlueprintPure, Category="Start World|Navigation")
    FString GetPendingServerAddress() const { return PendingServerAddress; }

    /** Cleans StartWorld UI and editor-only transaction references before any menu-triggered map travel. */
    UFUNCTION(BlueprintCallable, Category="Start World|Navigation")
    void PrepareMenuForWorldTravel();

protected:
    virtual void BeginPlay() override;
    virtual void Destroyed() override;

    /** Start-screen widget class. Set this to a WBP_StartWorld child reparented to UStartWorldWidget. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|UI")
    TSubclassOf<UStartWorldWidget> StartMenuWidgetClass;

    /** World-selection widget class. Set this to WBP_LevelMenu reparented to UWorldSelectionWidget. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|UI")
    TSubclassOf<UWorldSelectionWidget> WorldSelectionWidgetClass;

    /** Optional multiplayer menu widget class. Assign explicitly when the project uses a multiplayer menu. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|UI")
    TSubclassOf<UStartWorldWidget> MultiplayerMenuWidgetClass;

    /** Single-player gameplay level opened after WBP_LevelMenu selects a world folder. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|Navigation")
    FName GameplayLevelName = TEXT("SingleWorld");

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|Navigation")
    FName HostWorldLevelName = TEXT("HostWorld");

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|Navigation")
    FName ClientWorldLevelName = TEXT("ClientWorld");

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|Navigation")
    FString DefaultServerAddress = TEXT("127.0.0.1:7777");

    /** Z order used when adding start/world-selection widgets to the viewport. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|UI")
    int32 MenuZOrder = 0;

    /** Applies UI-only input and shows the mouse cursor when menu widgets are opened. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|Input")
    bool bApplyMenuInputMode = true;

    /** Clears the editor undo buffer before StartWorld-triggered level travel to prevent REINST widget world leaks in PIE. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|Editor")
    bool bResetEditorTransactionsBeforeTravel = true;

private:
    void InitializeStartScreenAfterBlueprintBeginPlay();
    UClass* ResolveMenuWidgetClass(UClass* WidgetClass, const TCHAR* DebugWidgetName) const;
    UStartWorldWidget* CreateAndAddMenuWidget(UClass* WidgetClass, const TCHAR* DebugWidgetName);
    void RemoveTrackedMenuWidgets();
    void RemoveAllMenuWidgets();
    void ApplyMenuInputMode(UStartWorldWidget* FocusWidget) const;
    void BuildLevelFolderNameMap();
    void ResetEditorTransactionBufferForMenuTravel(const TCHAR* Reason) const;

private:
    UPROPERTY(Transient)
    TObjectPtr<UStartWorldWidget> StartMenuWidget;

    UPROPERTY(Transient)
    TObjectPtr<UWorldSelectionWidget> WorldSelectionWidget;

    UPROPERTY(Transient)
    TObjectPtr<UStartWorldWidget> MultiplayerMenuWidget;

    UPROPERTY(Transient)
    TMap<FString, FString> FolderNameMap;

    UPROPERTY(Transient)
    FString PendingServerAddress = TEXT("127.0.0.1:7777");
};
