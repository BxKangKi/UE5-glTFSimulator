// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "StartActor.generated.h"

class UUserWidget;

UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API AStartActor : public AActor
{
    GENERATED_BODY()

public:
    AStartActor();

    /** Opens the world-selection widget from the start screen. */
    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void StartGame();

    /** Shows the start screen widget. Bind this to the world-selection Back button when the widget has a StartWorld reference. */
    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void ReturnToMainMenuFromWorldSelection();

    /** Rebuilds and shows the main start screen widget. */
    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void ShowStartMenu();

    /** Rebuilds and shows the world-selection widget. */
    UFUNCTION(BlueprintCallable, Category="Start World|UI")
    void ShowWorldSelectionMenu();

    /** Re-scans available world folders and display names. */
    UFUNCTION(BlueprintCallable, Category="Start World|Data")
    void RefreshWorldFolderNameMap();

    /** Returns a map of world-folder names to display names for the level menu widget. */
    UFUNCTION(BlueprintPure, Category="Start World|Data")
    TMap<FString, FString> GetFolderNameMap() const { return FolderNameMap; }

protected:
    virtual void BeginPlay() override;
    virtual void Destroyed() override;

    /** Start-screen widget class. Set this in BP_StartWorld or another Blueprint child of StartActor. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|UI")
    TSubclassOf<UUserWidget> StartMenuWidgetClass;

    /** World-selection widget class. Set this in BP_StartWorld or another Blueprint child of StartActor. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|UI")
    TSubclassOf<UUserWidget> WorldSelectionWidgetClass;

    /** Z order used when adding start/world-selection widgets to the viewport. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|UI")
    int32 MenuZOrder = 0;

    /** Applies UI-only input and shows the mouse cursor when menu widgets are opened. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|Input")
    bool bApplyMenuInputMode = true;

    /** Name of the object property on widgets that should receive this StartActor reference. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|Reflection")
    FName StartActorWidgetPropertyName;

    /** Name of the Blueprint function called on the world-selection widget after creation. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Start World|Reflection")
    FName WorldSelectionInitFunctionName;

private:
    void InitializeStartScreenAfterBlueprintBeginPlay();
    UClass* ResolveMenuWidgetClass(TSubclassOf<UUserWidget> WidgetClass, const TCHAR* DefaultWidgetClassPath, const TCHAR* DebugWidgetName) const;
    UUserWidget* CreateAndAddMenuWidget(TSubclassOf<UUserWidget> WidgetClass, const TCHAR* DefaultWidgetClassPath, const TCHAR* DebugWidgetName);
    void RemoveTrackedMenuWidgets();
    void RemoveAllMenuWidgets();
    void ApplyMenuInputMode(UUserWidget* FocusWidget) const;
    void AssignStartActorReference(UUserWidget* Widget) const;
    void InvokeWorldSelectionInit(UUserWidget* Widget) const;
    bool HasCompatibleInitFunction(const UFunction* Function) const;
    void BuildLevelFolderNameMap();

private:
    UPROPERTY(Transient)
    TObjectPtr<UUserWidget> StartMenuWidget;

    UPROPERTY(Transient)
    TObjectPtr<UUserWidget> WorldSelectionWidget;

    UPROPERTY(Transient)
    TMap<FString, FString> FolderNameMap;
};
