// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "StartWorldWidget.generated.h"

class AStartActor;
class UButton;
class UTextBlock;

/**
 * Base class for StartWorld UI widgets.
 *
 * Reparent WBP_StartWorld to this class and bind its Start button to ExecuteStartGame() or StartGame().
 * Reparent WBP_LevelMenu to this class too, then use Init() / OnWorldSelectionDataUpdated() to rebuild
 * the world list and ExecuteReturnToMainMenuFromWorldSelection() for its Back button.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UStartWorldWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    /** Stores the owning StartActor. AStartActor calls this immediately after creating the widget. */
    UFUNCTION(BlueprintCallable, Category="Start World|Actor")
    void SetStartActor(AStartActor* InStartActor);

    /** Returns the owning StartActor assigned by AStartActor. */
    UFUNCTION(BlueprintPure, Category="Start World|Actor")
    AStartActor* GetStartActor() const { return StartActor.Get(); }

    /** Opens the world-selection screen through the owning StartActor. Bind this to the start button. */
    UFUNCTION(BlueprintCallable, Category="Start World|Actions")
    void ExecuteStartGame();

    /** Returns from the world-selection screen to the main start screen. Bind this to the Back button. */
    UFUNCTION(BlueprintCallable, Category="Start World|Actions")
    void ExecuteReturnToMainMenuFromWorldSelection();

    /** Short alias for Blueprint button bindings. */
    UFUNCTION(BlueprintCallable, Category="Start World|Actions")
    void ReturnToMainMenuFromWorldSelection();

    /** Opens the main start screen through the owning StartActor. */
    UFUNCTION(BlueprintCallable, Category="Start World|Actions")
    void ExecuteShowStartMenu();

    /** Opens the world-selection screen through the owning StartActor. */
    UFUNCTION(BlueprintCallable, Category="Start World|Actions")
    void ExecuteShowWorldSelectionMenu();

    /** Opens the multiplayer menu through the owning StartActor. */
    UFUNCTION(BlueprintCallable, Category="Start World|Actions")
    void ExecuteShowMultiplayerMenu();

    /** Re-scans world folders and refreshes the world-selection widget. */
    UFUNCTION(BlueprintCallable, Category="Start World|Actions")
    void ExecuteRefreshWorldSelectionData();

    /** Rebinds optional named buttons. Blueprint can call this after dynamically rebuilding its layout. */
    UFUNCTION(BlueprintCallable, Category="Start World|Actions")
    void BindDefaultButtons();

    /** Removes C++ button delegates. NativeDestruct calls this automatically. */
    UFUNCTION(BlueprintCallable, Category="Start World|Actions")
    void UnbindDefaultButtons();

    /** Selects a world by folder key. WBP_LevelMenu can call this before OpenSelectedWorld(). */
    UFUNCTION(BlueprintCallable, Category="Start World|World Selection")
    void SetSelectedWorldFolderName(const FString& InWorldFolderName);

    /** Returns the currently selected world folder key. */
    UFUNCTION(BlueprintPure, Category="Start World|World Selection")
    FString GetSelectedWorldFolderName() const { return SelectedWorldFolderName; }

    /** Resolves either a folder key or a displayed name to the folder key used by the game manager. */
    UFUNCTION(BlueprintCallable, Category="Start World|World Selection")
    bool ResolveWorldFolderName(const FString& FolderOrDisplayName, FString& OutWorldFolderName) const;

    /** Opens the selected world folder. Bind this to dynamically generated world buttons when the selection is already set. */
    UFUNCTION(BlueprintCallable, Category="Start World|World Selection")
    bool OpenSelectedWorld();

    /** Opens a world by folder key. */
    UFUNCTION(BlueprintCallable, Category="Start World|World Selection")
    bool OpenWorldByFolderName(const FString& WorldFolderName);

    /** Opens a world by the display name shown in WBP_LevelMenu. */
    UFUNCTION(BlueprintCallable, Category="Start World|World Selection")
    bool OpenWorldByDisplayName(const FString& DisplayName);

    /** Opens a world by reading the first TextBlock found under a Button. */
    UFUNCTION(BlueprintCallable, Category="Start World|World Selection")
    bool OpenWorldFromButtonText(UButton* Button);

    UFUNCTION(BlueprintCallable, Category="Start World|Multiplayer")
    void ExecuteHostSelectedWorld();

    UFUNCTION(BlueprintCallable, Category="Start World|Multiplayer")
    bool HostSelectedWorld();

    UFUNCTION(BlueprintCallable, Category="Start World|Multiplayer")
    bool HostWorldByFolderName(const FString& WorldFolderName);

    UFUNCTION(BlueprintCallable, Category="Start World|Multiplayer")
    void ExecuteJoinSelectedWorld();

    UFUNCTION(BlueprintCallable, Category="Start World|Multiplayer")
    bool JoinSelectedWorld();

    UFUNCTION(BlueprintCallable, Category="Start World|Multiplayer")
    bool JoinServer(const FString& InServerAddress);

    UFUNCTION(BlueprintCallable, Category="Start World|Multiplayer")
    void ExecuteOpenClientConnectionWorld();

    UFUNCTION(BlueprintCallable, Category="Start World|Multiplayer")
    bool OpenClientConnectionWorld();

    UFUNCTION(BlueprintCallable, Category="Start World|Multiplayer")
    void SetServerAddress(const FString& InServerAddress);

    UFUNCTION(BlueprintPure, Category="Start World|Multiplayer")
    FString GetServerAddress() const { return ServerAddress; }

    /** Reads the first TextBlock text under a Button. Useful for WBP_LevelMenu dynamic buttons. */
    UFUNCTION(BlueprintPure, Category="Start World|World Selection")
    FString GetButtonTextFromUI(UButton* Button) const;

    /** Clears UI references and editor-only transaction state before a widget-triggered OpenLevel call. */
    UFUNCTION(BlueprintCallable, Category="Start World|World Selection")
    void PrepareForWorldTravelFromUI();

    /** Returns the last world-folder map delivered to this widget. */
    UFUNCTION(BlueprintPure, Category="Start World|Data")
    TMap<FString, FString> GetCachedWorldSelectionData() const { return CachedWorldSelectionData; }

    /** Returns the current world-folder map from StartActor when available, otherwise the cached copy. */
    UFUNCTION(BlueprintPure, Category="Start World|Data")
    TMap<FString, FString> GetFolderNameMap() const;

    /** Updates the cached world-selection data and notifies Blueprint subclasses. */
    UFUNCTION(BlueprintCallable, Category="Start World|Data")
    void SetWorldSelectionData(const TMap<FString, FString>& Values);

    /** Compatibility entry point for WBP_LevelMenu. Override or handle OnWorldSelectionDataUpdated in Blueprint. */
    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Start World|Data")
    void Init(const TMap<FString, FString>& Values);
    virtual void Init_Implementation(const TMap<FString, FString>& Values);

    /** Called after SetStartActor assigns the owning actor. */
    UFUNCTION(BlueprintImplementableEvent, Category="Start World|Events")
    void OnStartActorAssigned(AStartActor* InStartActor);

    /** Called when StartActor passes the world-selection folder-name map to this widget. */
    UFUNCTION(BlueprintImplementableEvent, Category="Start World|Events")
    void OnWorldSelectionDataUpdated(const TMap<FString, FString>& Values);

protected:
    /** Optional start button. Bind this variable or name a button StartButton / StartGameButton / WorldSelectionButton. */
    UPROPERTY(BlueprintReadOnly, meta=(BindWidgetOptional), Category="Start World|Widgets")
    TObjectPtr<UButton> StartButton;

    /** Optional alias button for opening world selection. */
    UPROPERTY(BlueprintReadOnly, meta=(BindWidgetOptional), Category="Start World|Widgets")
    TObjectPtr<UButton> WorldSelectionButton;

    /** Optional back button for WBP_LevelMenu. */
    UPROPERTY(BlueprintReadOnly, meta=(BindWidgetOptional), Category="Start World|Widgets")
    TObjectPtr<UButton> BackButton;

    /** Optional refresh button for WBP_LevelMenu. */
    UPROPERTY(BlueprintReadOnly, meta=(BindWidgetOptional), Category="Start World|Widgets")
    TObjectPtr<UButton> RefreshButton;

    UPROPERTY(BlueprintReadOnly, meta=(BindWidgetOptional), Category="Start World|Widgets")
    TObjectPtr<UButton> MultiplayerButton;

    UPROPERTY(BlueprintReadOnly, meta=(BindWidgetOptional), Category="Start World|Widgets")
    TObjectPtr<UButton> HostButton;

    UPROPERTY(BlueprintReadOnly, meta=(BindWidgetOptional), Category="Start World|Widgets")
    TObjectPtr<UButton> ClientButton;

    UPROPERTY(BlueprintReadOnly, meta=(BindWidgetOptional), Category="Start World|Widgets")
    TObjectPtr<UButton> JoinButton;

    /** Last world-folder map passed from StartActor. */
    UPROPERTY(BlueprintReadOnly, Category="Start World|Data")
    TMap<FString, FString> CachedWorldSelectionData;

    /** Last selected world folder key. */
    UPROPERTY(BlueprintReadOnly, Category="Start World|World Selection")
    FString SelectedWorldFolderName;

    UPROPERTY(BlueprintReadOnly, Category="Start World|Multiplayer")
    FString ServerAddress = TEXT("127.0.0.1:7777");

private:
    void CacheDefaultButtons();

    /** Weak on purpose: editor undo buffers can retain REINST widgets, and strong world refs cause stale world leaks. */
    TWeakObjectPtr<AStartActor> StartActor;
};
