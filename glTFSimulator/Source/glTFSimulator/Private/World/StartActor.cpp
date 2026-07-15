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
#include "System/MultiplayerWorldSubSystem.h"
#include "TimerManager.h"
#include "UI/StartWorldWidget.h"
#include "UI/WorldSelectionWidget.h"
#include "World/WorldData.h"

namespace
{
    FString NormalizeStartWorldString(FString Value)
    {
        Value.TrimStartAndEndInline();
        return Value;
    }
}

AStartActor::AStartActor()
{
    PrimaryActorTick.bCanEverTick = false;

    PendingServerAddress = DefaultServerAddress;

    // Widget classes are intentionally assigned in BP_StartWorld. No fallback Blueprint paths are loaded here.
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
    ResetEditorTransactionBufferForMenuTravel(TEXT("Show MainWorld start menu"));

    StartMenuWidget = CreateAndAddMenuWidget(
        StartMenuWidgetClass.Get(),
        TEXT("StartMenuWidgetClass"));

    ApplyMenuInputMode(StartMenuWidget.Get());
}

void AStartActor::ShowWorldSelectionMenu()
{
    BuildLevelFolderNameMap();
    RemoveAllMenuWidgets();
    ResetEditorTransactionBufferForMenuTravel(TEXT("Show MainWorld single-player world-selection menu"));

    WorldSelectionWidget = Cast<UWorldSelectionWidget>(CreateAndAddMenuWidget(
        WorldSelectionWidgetClass.Get(),
        TEXT("WorldSelectionWidgetClass")));

    if (IsValid(WorldSelectionWidget))
    {
        WorldSelectionWidget->SetWorldSelectionData(FolderNameMap);
    }

    ApplyMenuInputMode(WorldSelectionWidget.Get());
}

void AStartActor::ShowMultiplayerMenu()
{
    BuildLevelFolderNameMap();
    RemoveAllMenuWidgets();
    ResetEditorTransactionBufferForMenuTravel(TEXT("Show MainWorld multiplayer menu"));

    MultiplayerMenuWidget = CreateAndAddMenuWidget(
        MultiplayerMenuWidgetClass.Get(),
        TEXT("MultiplayerMenuWidgetClass"));

    if (IsValid(MultiplayerMenuWidget))
    {
        MultiplayerMenuWidget->SetWorldSelectionData(FolderNameMap);
    }

    ApplyMenuInputMode(MultiplayerMenuWidget.Get());
}

void AStartActor::RefreshWorldFolderNameMap()
{
    BuildLevelFolderNameMap();

    if (IsValid(WorldSelectionWidget))
    {
        WorldSelectionWidget->SetWorldSelectionData(FolderNameMap);
    }

    if (IsValid(MultiplayerMenuWidget))
    {
        MultiplayerMenuWidget->SetWorldSelectionData(FolderNameMap);
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

void AStartActor::OpenSinglePlayerWorldByFolderName(const FString& WorldFolderName)
{
    FString ResolvedFolderName;
    if (!TryResolveWorldFolderFromDisplayName(WorldFolderName, ResolvedFolderName))
    {
        UE_LOG(LogTemp, Warning, TEXT("StartActor cannot open single-player world. Unknown world folder/display name: %s"), *WorldFolderName);
        return;
    }

    if (GameplayLevelName == NAME_None)
    {
        UE_LOG(LogTemp, Warning, TEXT("StartActor cannot open single-player world because GameplayLevelName is not assigned."));
        return;
    }

    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->SetCurrentWorldName(ResolvedFolderName);
        GameManager->ClearWorldSelectionMenuRequest();
        GameManager->SetGamePaused(false);
    }

    PrepareMenuForWorldTravel();
    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        Multiplayer->StartSinglePlayerWorld(this, ResolvedFolderName, GameplayLevelName);
    }
    else
    {
        UGameplayStatics::OpenLevel(this, GameplayLevelName);
    }
}

void AStartActor::HostMultiplayerWorldByFolderName(const FString& WorldFolderName)
{
    FString ResolvedFolderName;
    if (!TryResolveWorldFolderFromDisplayName(WorldFolderName, ResolvedFolderName))
    {
        UE_LOG(LogTemp, Warning, TEXT("StartActor cannot host multiplayer world. Unknown world folder/display name: %s"), *WorldFolderName);
        return;
    }

    if (HostWorldLevelName == NAME_None)
    {
        UE_LOG(LogTemp, Warning, TEXT("StartActor cannot host multiplayer world because HostWorldLevelName is not assigned."));
        return;
    }

    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->SetCurrentWorldName(ResolvedFolderName);
        GameManager->ClearWorldSelectionMenuRequest();
        GameManager->SetGamePaused(false);
    }

    PrepareMenuForWorldTravel();
    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        Multiplayer->HostMultiplayerWorld(this, ResolvedFolderName, HostWorldLevelName);
    }
}

void AStartActor::OpenClientConnectionWorld(const FString& InServerAddress)
{
    SetPendingServerAddress(InServerAddress);
    PrepareMenuForWorldTravel();
    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        Multiplayer->SetServerAddress(PendingServerAddress);
        Multiplayer->OpenClientConnectionWorld(this, ClientWorldLevelName);
    }
}

void AStartActor::JoinMultiplayerServer(const FString& InServerAddress, const FString& OptionalWorldFolderName)
{
    SetPendingServerAddress(InServerAddress);
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->ClearWorldSelectionMenuRequest();
        GameManager->SetGamePaused(false);
    }

    PrepareMenuForWorldTravel();
    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        Multiplayer->JoinMultiplayerWorld(this, PendingServerAddress, OptionalWorldFolderName);
    }
}

void AStartActor::SetPendingServerAddress(const FString& InServerAddress)
{
    PendingServerAddress = InServerAddress;
    PendingServerAddress.TrimStartAndEndInline();
    if (PendingServerAddress.IsEmpty())
    {
        PendingServerAddress = DefaultServerAddress.IsEmpty() ? FString(TEXT("127.0.0.1:7777")) : DefaultServerAddress;
    }

    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        Multiplayer->SetServerAddress(PendingServerAddress);
    }
}

void AStartActor::PrepareMenuForWorldTravel()
{
    RemoveAllMenuWidgets();
    ResetEditorTransactionBufferForMenuTravel(TEXT("MainWorld menu world travel"));
}

UClass* AStartActor::ResolveMenuWidgetClass(UClass* WidgetClass, const TCHAR* DebugWidgetName) const
{
    UClass* ResolvedClass = WidgetClass;
    if (!ResolvedClass || !ResolvedClass->IsChildOf(UStartWorldWidget::StaticClass()))
    {
        UE_LOG(LogTemp, Warning,
               TEXT("StartActor cannot create %s because no valid widget class is assigned in BP_StartWorld."),
               DebugWidgetName ? DebugWidgetName : TEXT("MenuWidgetClass"));
        return nullptr;
    }

    return ResolvedClass;
}

UStartWorldWidget* AStartActor::CreateAndAddMenuWidget(UClass* WidgetClass, const TCHAR* DebugWidgetName)
{
    UClass* ResolvedClass = ResolveMenuWidgetClass(WidgetClass, DebugWidgetName);
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
    if (IsValid(MultiplayerMenuWidget))
    {
        MultiplayerMenuWidget->RemoveFromParent();
    }

    StartMenuWidget = nullptr;
    WorldSelectionWidget = nullptr;
    MultiplayerMenuWidget = nullptr;
}

void AStartActor::RemoveAllMenuWidgets()
{
    UWidgetLayoutLibrary::RemoveAllWidgets(this);
    StartMenuWidget = nullptr;
    WorldSelectionWidget = nullptr;
    MultiplayerMenuWidget = nullptr;
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
        Reason ? FString(Reason) : FString(TEXT("MainWorld menu travel")));
}
