// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"
#include "UI/WorldSelectionWidget.h"
#include "StartActor.generated.h"

class UWorld;

/**
 * Owns the MainWorld menu flow.
 *
 * The owning Blueprint should inherit from this actor and explicitly assign each menu widget class.
 * The start-menu widget should inherit UStartWorldWidget, and the world-selection widget should inherit
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

    /** Rebuilds and shows the multiplayer menu. Assign MultiplayerMenuWidgetClass before using this. */
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

    /** Opens the directly assigned single-player gameplay world after selecting an external world-data folder. */
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

    /** Start-screen widget class assigned directly in the owning Blueprint/class defaults. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|UI")
    TSubclassOf<UStartWorldWidget> StartMenuWidgetClass;

    /** World-selection widget class assigned directly in the owning Blueprint/class defaults. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|UI")
    TSubclassOf<UWorldSelectionWidget> WorldSelectionWidgetClass;

    /** Optional multiplayer menu widget class. Assign explicitly when the project uses a multiplayer menu. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|UI")
    TSubclassOf<UStartWorldWidget> MultiplayerMenuWidgetClass;

    /** Single-player gameplay world opened after the world-selection widget chooses a folder. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|Navigation")
    TSoftObjectPtr<UWorld> GameplayWorld;

    /** Multiplayer host world. Assign the world asset directly. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|Navigation")
    TSoftObjectPtr<UWorld> HostWorld;

    /** Client connection/loading world. Assign the world asset directly. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|Navigation")
    TSoftObjectPtr<UWorld> ClientWorld;

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

    /** If MainWorld is still alive after this delay, restore the selection UI instead of leaving a blank/start screen. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|Navigation", meta=(ClampMin="1.0"))
    float GameplayTravelFailureTimeoutSeconds = 5.0f;

    /**
     * Minimum delay before accepting a world button after returning from gameplay. The guard also
     * remains active while the mouse/confirm key that initiated Exit is still held.
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|Input", meta=(ClampMin="0.05", ClampMax="2.0"))
    float WorldSelectionReturnInputGuardSeconds = 0.20f;

private:
    void InitializeStartScreenAfterBlueprintBeginPlay();
    UClass* ResolveMenuWidgetClass(UClass* WidgetClass, const TCHAR* DebugWidgetName) const;
    UStartWorldWidget* CreateAndAddMenuWidget(UClass* WidgetClass, const TCHAR* DebugWidgetName);
    void RemoveTrackedMenuWidgets();
    void RemoveAllMenuWidgets();
    void ApplyMenuInputMode(UStartWorldWidget* FocusWidget) const;
    void BuildLevelFolderNameMap();
    void HandleGameplayTravelWatchdogExpired();
    void StartWorldSelectionReturnInputGuard();
    void TryReleaseWorldSelectionReturnInputGuard();
    void CancelWorldSelectionReturnInputGuard();
    bool IsWorldSelectionActivationInputHeld() const;
    bool IsWorldSelectionReturnBlocked() const;
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

    /** Blocks stale Blueprint/back-button callbacks after a valid world button has already started travel. */
    UPROPERTY(Transient)
    bool bGameplayWorldTravelPending = false;

    UPROPERTY(Transient)
    FString PendingGameplayWorldFolderName;

    /** Blocks Construct-time callbacks and carried-over click/key releases after gameplay Exit. */
    UPROPERTY(Transient)
    bool bWorldSelectionReturnInputGuardActive = false;

    FTimerHandle GameplayTravelWatchdogHandle;
    FTimerHandle WorldSelectionReturnInputGuardHandle;
};
