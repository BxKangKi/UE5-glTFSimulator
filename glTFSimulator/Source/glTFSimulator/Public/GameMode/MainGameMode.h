// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

/**
 * @file MainGameMode.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "TimerManager.h"
#include "UI/WorldSelectionWidget.h"
#include "MainGameMode.generated.h"

class AGameModeBase;
struct FGlTFWorldLaunchProfile;
class UWorld;
class UStartWorldWidget;
class USettingsMenuWidget;
class UUserWidget;

/**
 * Owns the MainWorld menu flow.
 *
 * Top-level menu widget classes come from UGlTFSimulatorAssetRegistry and are created automatically.
 * Blueprint subclasses normally only provide layout/button wiring; Set*Widget remains an override path.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API AMainGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    AMainGameMode();

    /** Optional explicit override. Missing widgets are auto-created from the central registry. */
    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void SetStartMenuWidget(UStartWorldWidget* InWidget);

    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void SetWorldSelectionWidget(UWorldSelectionWidget* InWidget);

    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void SetMultiplayerMenuWidget(UStartWorldWidget* InWidget);

    /** Register the same USettingsMenuWidget-derived WBP type used by the pause menu. */
    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void SetSettingsWidget(USettingsMenuWidget* InWidget);

    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void ShowSettingsMenu();

    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void ReturnFromSettings();

    /** Opens the world-selection widget from the start screen. */
    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void StartGame();

    /** Shows the start screen widget. Bind this through UStartWorldWidget when the world-selection Back button is clicked. */
    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void ReturnToMainMenuFromWorldSelection();

    /** Shows the already-registered main start-screen widget. */
    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void ShowStartMenu();

    /** Shows the already-registered world-selection widget for single-player world travel. */
    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void ShowWorldSelectionMenu();

    /** Shows the already-registered multiplayer menu widget. */
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
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;


    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|Navigation")
    FString DefaultServerAddress = TEXT("127.0.0.1:7777");

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
    /** Creates any missing top-level menu widgets directly from the central registry classes. */
    void InitializeRegistryDrivenUI();
    void InitializeStartScreenAfterBlueprintBeginPlay();
    void HideAllMenuWidgets();
    void ApplyMenuInputMode(UUserWidget* FocusWidget) const;
    void BuildLevelFolderNameMap();
    void HandleGameplayTravelWatchdogExpired();
    void StartWorldSelectionReturnInputGuard();
    void TryReleaseWorldSelectionReturnInputGuard();
    void CancelWorldSelectionReturnInputGuard();
    bool IsWorldSelectionActivationInputHeld() const;
    bool IsWorldSelectionReturnBlocked() const;
    const FGlTFWorldLaunchProfile* FindWorldLaunchProfile(const FString& WorldFolderName) const;
    void ResolveWorldLaunch(
        const FString& WorldFolderName,
        bool bForHost,
        TSoftObjectPtr<UWorld>& OutWorld,
        TSoftClassPtr<AGameModeBase>& OutGameModeOverride,
        FString& OutResolutionSource) const;
    void ResetEditorTransactionBufferForMenuTravel(const TCHAR* Reason) const;

private:
    UPROPERTY(Transient)
    TObjectPtr<UStartWorldWidget> StartMenuWidget;

    UPROPERTY(Transient)
    TObjectPtr<UWorldSelectionWidget> WorldSelectionWidget;

    UPROPERTY(Transient)
    TObjectPtr<UStartWorldWidget> MultiplayerMenuWidget;

    UPROPERTY(Transient)
    TObjectPtr<USettingsMenuWidget> SettingsWidget;

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
