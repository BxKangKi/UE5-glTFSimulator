// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "World/StartActor.h"

#include "Blueprint/WidgetLayoutLibrary.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "System/FileFunctionLibrary.h"
#include "System/GameManagerSubSystem.h"
#include "System/MacroLibrary.h"
#include "TimerManager.h"
#include "UI/StartWorldWidget.h"
#include "World/WorldData.h"

namespace
{
    static const TCHAR* const DefaultStartMenuWidgetClassPath = TEXT("/Game/Blueprints/StartWorld/WBP_StartWorld.WBP_StartWorld_C");
    static const TCHAR* const DefaultWorldSelectionWidgetClassPath = TEXT("/Game/Blueprints/StartWorld/WBP_LevelMenu.WBP_LevelMenu_C");

    FString NormalizeStartWorldString(FString Value)
    {
        Value.TrimStartAndEndInline();
        return Value;
    }
}

AStartActor::AStartActor()
{
    PrimaryActorTick.bCanEverTick = false;

    // Widget classes are intentionally not loaded in the constructor.
    // Assign them in BP_StartWorld, or let the fallback paths load only when a menu is opened.
}

void AStartActor::BeginPlay()
{
    Super::BeginPlay();

    // Rebuild the level list before any UI asks for it.
    BuildLevelFolderNameMap();

    // Legacy BP_StartWorld graphs may still create widgets after calling the parent BeginPlay.
    // Running on the next tick lets this native flow own the final visible menu and clean up legacy widgets.
    GetWorldTimerManager().SetTimerForNextTick(
        FTimerDelegate::CreateWeakLambda(this, [this]()
        {
            InitializeStartScreenAfterBlueprintBeginPlay();
        }));
}

void AStartActor::Destroyed()
{
    RemoveAllMenuWidgets();
    ResetEditorTransactionBufferForMenuTravel(TEXT("StartActor destroyed"));

    // Do not force garbage collection from Destroyed(). During level travel this can block asset loading
    // and make the editor/game appear stuck around the loading-progress phase.
    // Super::Destroyed() may still trigger legacy Blueprint ReceiveDestroyed graphs, so the editor
    // transaction buffer is reset before this call to avoid stale REINST widget world references.
    Super::Destroyed();
}

void AStartActor::InitializeStartScreenAfterBlueprintBeginPlay()
{
    bool bOpenWorldSelection = false;

    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        // Runtime world memory is released only after the destination menu level has finished loading.
        if (GameManager->HasPendingMainWorldRuntimePurge())
        {
            GameManager->ReleaseMainWorldRuntimeMemory(true);
        }

        bOpenWorldSelection = GameManager->ConsumeWorldSelectionMenuRequest();
    }

    if (bOpenWorldSelection)
    {
        ShowWorldSelectionMenu();
    }
    else
    {
        ShowStartMenu();
    }
}

void AStartActor::StartGame()
{
    ShowWorldSelectionMenu();
}

void AStartActor::ReturnToMainMenuFromWorldSelection()
{
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->ClearWorldSelectionMenuRequest();
        GameManager->SetGamePaused(false);
    }

    ShowStartMenu();
}

void AStartActor::ShowStartMenu()
{
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->ClearWorldSelectionMenuRequest();
    }

    RemoveAllMenuWidgets();
    ResetEditorTransactionBufferForMenuTravel(TEXT("Show StartWorld start menu"));

    StartMenuWidget = CreateAndAddMenuWidget(
        StartMenuWidgetClass,
        DefaultStartMenuWidgetClassPath,
        TEXT("StartMenuWidgetClass"),
        false);

    ApplyMenuInputMode(StartMenuWidget.Get());
}

void AStartActor::ShowWorldSelectionMenu()
{
    BuildLevelFolderNameMap();
    RemoveAllMenuWidgets();
    ResetEditorTransactionBufferForMenuTravel(TEXT("Show StartWorld world-selection menu"));

    WorldSelectionWidget = CreateAndAddMenuWidget(
        WorldSelectionWidgetClass,
        DefaultWorldSelectionWidgetClassPath,
        TEXT("WorldSelectionWidgetClass"),
        true);

    ApplyMenuInputMode(WorldSelectionWidget.Get());
}

void AStartActor::RefreshWorldFolderNameMap()
{
    BuildLevelFolderNameMap();

    if (IsValid(WorldSelectionWidget))
    {
        WorldSelectionWidget->Init(FolderNameMap);
    }
}

bool AStartActor::TryResolveWorldFolderFromDisplayName(const FString& DisplayName, FString& OutFolderName) const
{
    const FString NormalizedInput = NormalizeStartWorldString(DisplayName);
    if (NormalizedInput.IsEmpty())
    {
        OutFolderName.Reset();
        return false;
    }

    if (FolderNameMap.Contains(NormalizedInput))
    {
        OutFolderName = NormalizedInput;
        return true;
    }

    for (const TPair<FString, FString>& Pair : FolderNameMap)
    {
        if (NormalizeStartWorldString(Pair.Value).Equals(NormalizedInput, ESearchCase::IgnoreCase))
        {
            OutFolderName = Pair.Key;
            return true;
        }
    }

    OutFolderName.Reset();
    return false;
}

void AStartActor::OpenGameplayWorldByFolderName(const FString& WorldFolderName)
{
    FString ResolvedFolderName;
    if (!TryResolveWorldFolderFromDisplayName(WorldFolderName, ResolvedFolderName))
    {
        UE_LOG(LogTemp, Warning, TEXT("StartActor cannot open gameplay world. Unknown world folder/display name: %s"), *WorldFolderName);
        return;
    }

    if (GameplayLevelName == NAME_None)
    {
        UE_LOG(LogTemp, Warning, TEXT("StartActor cannot open gameplay world because GameplayLevelName is not assigned."));
        return;
    }

    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->SetCurrentWorldName(ResolvedFolderName);
        GameManager->ClearWorldSelectionMenuRequest();
        GameManager->SetGamePaused(false);
    }

    PrepareMenuForWorldTravel();
    UGameplayStatics::OpenLevel(this, GameplayLevelName);
}

void AStartActor::PrepareMenuForWorldTravel()
{
    RemoveAllMenuWidgets();
    ResetEditorTransactionBufferForMenuTravel(TEXT("StartWorld menu world travel"));
}

UClass* AStartActor::ResolveMenuWidgetClass(TSubclassOf<UStartWorldWidget> WidgetClass, const TCHAR* DefaultWidgetClassPath, const TCHAR* DebugWidgetName) const
{
    UClass* ResolvedClass = WidgetClass.Get();

    if (!ResolvedClass && DefaultWidgetClassPath && DefaultWidgetClassPath[0] != TEXT('\0'))
    {
        // Load fallback classes only when a menu is opened. This avoids constructor-time widget
        // Blueprint loads while BP_StartWorld or WBP_StartWorld is being compiled by the editor.
        ResolvedClass = LoadClass<UStartWorldWidget>(nullptr, DefaultWidgetClassPath);
    }

    if (!ResolvedClass || !ResolvedClass->IsChildOf(UStartWorldWidget::StaticClass()))
    {
        UE_LOG(LogTemp, Warning,
               TEXT("StartActor cannot create %s. Reparent the widget to StartWorldWidget and assign it in BP_StartWorld. Fallback path: %s"),
               DebugWidgetName ? DebugWidgetName : TEXT("MenuWidgetClass"),
               DefaultWidgetClassPath ? DefaultWidgetClassPath : TEXT("<none>"));
        return nullptr;
    }

    return ResolvedClass;
}

UStartWorldWidget* AStartActor::CreateAndAddMenuWidget(TSubclassOf<UStartWorldWidget> WidgetClass, const TCHAR* DefaultWidgetClassPath, const TCHAR* DebugWidgetName, bool bPassWorldSelectionData)
{
    UClass* ResolvedClass = ResolveMenuWidgetClass(WidgetClass, DefaultWidgetClassPath, DebugWidgetName);
    if (!ResolvedClass)
    {
        return nullptr;
    }

    const TSubclassOf<UStartWorldWidget> ResolvedWidgetClass(ResolvedClass);

    UStartWorldWidget* Widget = nullptr;
    if (UGameInstance* GameInstance = GetGameInstance())
    {
        // Use GameInstance as the widget outer in the native path. If editor transactions accidentally retain
        // a REINST widget during PIE, this avoids an immediate strong Outer chain back to the old world.
        Widget = CreateWidget<UStartWorldWidget>(GameInstance, ResolvedWidgetClass);
    }
    else if (APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0))
    {
        Widget = CreateWidget<UStartWorldWidget>(PlayerController, ResolvedWidgetClass);
    }
    else if (UWorld* World = GetWorld())
    {
        Widget = CreateWidget<UStartWorldWidget>(World, ResolvedWidgetClass);
    }

    if (!IsValid(Widget))
    {
        UE_LOG(LogTemp, Warning, TEXT("StartActor failed to create %s."), DebugWidgetName ? DebugWidgetName : TEXT("MenuWidget"));
        return nullptr;
    }

    // Runtime menu widgets should not participate in the editor transaction buffer.
    // A transaction-retained REINST widget was the source of the reported stale-world reference chain.
    Widget->ClearFlags(RF_Transactional);
    Widget->SetFlags(RF_Transient);

    // Set the typed owner before AddToViewport so Blueprint Construct/OnAssigned logic can use it safely.
    Widget->SetStartActor(this);

    if (bPassWorldSelectionData)
    {
        // Direct virtual call. No dynamic function lookup, property lookup, or parameter packing is used.
        Widget->Init(FolderNameMap);
    }

    Widget->AddToViewport(MenuZOrder);
    return Widget;
}

void AStartActor::RemoveTrackedMenuWidgets()
{
    if (IsValid(StartMenuWidget))
    {
        StartMenuWidget->RemoveFromParent();
    }
    if (IsValid(WorldSelectionWidget))
    {
        WorldSelectionWidget->RemoveFromParent();
    }

    StartMenuWidget = nullptr;
    WorldSelectionWidget = nullptr;
}

void AStartActor::RemoveAllMenuWidgets()
{
    UWidgetLayoutLibrary::RemoveAllWidgets(this);
    StartMenuWidget = nullptr;
    WorldSelectionWidget = nullptr;
}

void AStartActor::ApplyMenuInputMode(UStartWorldWidget* FocusWidget) const
{
    if (!bApplyMenuInputMode)
    {
        return;
    }

    APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
    if (!PlayerController)
    {
        return;
    }

    PlayerController->bShowMouseCursor = true;

    FInputModeUIOnly InputMode;
    InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
    if (IsValid(FocusWidget))
    {
        InputMode.SetWidgetToFocus(FocusWidget->TakeWidget());
    }
    PlayerController->SetInputMode(InputMode);
}

void AStartActor::BuildLevelFolderNameMap()
{
    FolderNameMap.Empty();

    // Build absolute paths relative to the project Content directory.
    const FString RootPath = PATH_ROOT;
    TArray<FString> SubFolders;
    if (!UFileFunctionLibrary::GetSubFolders(RootPath, SubFolders))
    {
        UE_LOG(LogTemp, Warning, TEXT("No subfolders found in %s"), *RootPath);
        return;
    }

    for (const FString& SubFolderName : SubFolders)
    {
        const FString FullSubFolderPath = RootPath / SubFolderName;
        if (!FPaths::DirectoryExists(FullSubFolderPath))
        {
            continue;
        }

        const FString LevelJsonPath = FullSubFolderPath / LEVEL_FILE_NAME;
        FString NameValue;
        if (UFileFunctionLibrary::LoadJsonStringValue(LevelJsonPath, LEVELNAME, NameValue))
        {
            FolderNameMap.Add(SubFolderName, NameValue);
            UE_LOG(LogTemp, Log, TEXT("Loaded Level: Folder=%s, Name=%s"), *SubFolderName, *NameValue);
        }
    }
}

void AStartActor::ResetEditorTransactionBufferForMenuTravel(const TCHAR* Reason) const
{
    if (!bResetEditorTransactionsBeforeTravel)
    {
        return;
    }

    UGameManagerSubSystem::ResetEditorTransactionBufferForWorldTravel(
        this,
        Reason ? FString(Reason) : FString(TEXT("StartWorld menu travel")));
}
