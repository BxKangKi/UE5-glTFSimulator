// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "StartWorldWidget.generated.h"

class AStartActor;
class UButton;

/**
 * Base class for StartWorld UI widgets.
 *
 * AStartActor creates each menu widget, assigns itself with SetStartActor(), and passes data
 * through explicit typed functions. Widget code that needs StartActor behavior calls the stored
 * actor only after a null check; no native-event path or widget-name
 * reflection is used by this class.
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

    /** Rebinds optional button variables. Blueprint can call this after assigning widgets dynamically. */
    UFUNCTION(BlueprintCallable, Category="Start World|Actions")
    void BindDefaultButtons();

    /** Removes C++ button delegates. NativeDestruct calls this automatically. */
    UFUNCTION(BlueprintCallable, Category="Start World|Actions")
    void UnbindDefaultButtons();

    /** Selects a world by folder key. */
    UFUNCTION(BlueprintCallable, Category="Start World|World Selection")
    void SetSelectedWorldFolderName(const FString& InWorldFolderName);

    /** Returns the currently selected world folder key. */
    UFUNCTION(BlueprintPure, Category="Start World|World Selection")
    FString GetSelectedWorldFolderName() const { return SelectedWorldFolderName; }

    /** Resolves either a folder key or a displayed name to the folder key used by the game manager. */
    UFUNCTION(BlueprintCallable, Category="Start World|World Selection")
    bool ResolveWorldFolderName(const FString& FolderOrDisplayName, FString& OutWorldFolderName) const;

    /** Opens the selected world folder. */
    UFUNCTION(BlueprintCallable, Category="Start World|World Selection")
    bool OpenSelectedWorld();

    /** Opens a world by folder key. Display names are also accepted for compatibility. */
    UFUNCTION(BlueprintCallable, Category="Start World|World Selection")
    bool OpenWorldByFolderName(const FString& WorldFolderName);

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

    /** Returns the last world-folder map delivered to this widget. */
    UFUNCTION(BlueprintPure, Category="Start World|Data")
    TMap<FString, FString> GetCachedWorldSelectionData() const { return CachedWorldSelectionData; }

    /** Returns the current world-folder map from StartActor when available, otherwise the cached copy. */
    UFUNCTION(BlueprintPure, Category="Start World|Data")
    TMap<FString, FString> GetFolderNameMap() const;

    /** Updates the cached world-selection data. Derived native widgets can override this directly. */
    UFUNCTION(BlueprintCallable, Category="Start World|Data")
    virtual void SetWorldSelectionData(const TMap<FString, FString>& Values);

    /** Called after SetStartActor assigns the owning actor. */
    UFUNCTION(BlueprintImplementableEvent, Category="Start World|Events")
    void OnStartActorAssigned(AStartActor* InStartActor);

    /** Called when StartActor passes the world-selection folder-name map to this widget. */
    UFUNCTION(BlueprintImplementableEvent, Category="Start World|Events")
    void OnWorldSelectionDataUpdated(const TMap<FString, FString>& Values);

protected:
    /** Optional start button. Assign with the StartButton variable or call BindDefaultButtons() after setting it. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Start World|Widgets")
    TObjectPtr<UButton> StartButton;

    /** Optional alias button for opening world selection. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Start World|Widgets")
    TObjectPtr<UButton> WorldSelectionButton;

    /** Optional back button for WBP_LevelMenu. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Start World|Widgets")
    TObjectPtr<UButton> BackButton;

    /** Optional refresh button for WBP_LevelMenu. */
    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Start World|Widgets")
    TObjectPtr<UButton> RefreshButton;

    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Start World|Widgets")
    TObjectPtr<UButton> MultiplayerButton;

    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Start World|Widgets")
    TObjectPtr<UButton> HostButton;

    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Start World|Widgets")
    TObjectPtr<UButton> ClientButton;

    UPROPERTY(BlueprintReadWrite, meta=(BindWidgetOptional), Category="Start World|Widgets")
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
    /** Weak on purpose: editor undo buffers can retain REINST widgets, and strong world refs cause stale world leaks. */
    TWeakObjectPtr<AStartActor> StartActor;
};
