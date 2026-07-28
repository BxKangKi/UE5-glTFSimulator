// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "World/StartActor.h"

#include "Blueprint/WidgetLayoutLibrary.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
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

    // Widget classes and world assets are assigned directly in the owning Blueprint/class defaults.
}

void AStartActor::BeginPlay()
{
    // Arm before Super::BeginPlay so a legacy Blueprint ReceiveBeginPlay cannot consume the
    // carried-over Exit click and reopen gameplay before the native next-tick menu rebuild.
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        // Remember the loaded menu world as a soft object reference. Gameplay pause Exit can use
        // this when its PlayerController Blueprint has not repeated the same world assignment.
        GameManager->RegisterWorldSelectionWorld(TSoftObjectPtr<UWorld>(GetWorld()));
        bWorldSelectionReturnInputGuardActive = GameManager->ShouldOpenWorldSelectionMenuOnNextMainWorld();
    }

    Super::BeginPlay();

    // Rebuild the level list before any UI asks for it.
    BuildLevelFolderNameMap();

    // Legacy derived Blueprint graphs may still create widgets after calling the parent BeginPlay.
    // Running on the next tick lets this native flow own the final visible menu and clean up legacy widgets.
    GetWorldTimerManager().SetTimerForNextTick(
        FTimerDelegate::CreateWeakLambda(this, [this]()
        {
            InitializeStartScreenAfterBlueprintBeginPlay();
        }));
}

void AStartActor::Destroyed()
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(GameplayTravelWatchdogHandle);
        World->GetTimerManager().ClearTimer(WorldSelectionReturnInputGuardHandle);
    }
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

        // Keep the request alive until the return-input guard is released. This lets lower-level
        // world-travel code reject any stale callback that tries to reopen gameplay during arrival.
        bOpenWorldSelection = GameManager->ShouldOpenWorldSelectionMenuOnNextMainWorld();
    }

    if (bOpenWorldSelection)
    {
        // Arm before widget construction so legacy WBP Construct callbacks cannot immediately
        // reopen the previously selected gameplay world.
        bWorldSelectionReturnInputGuardActive = true;
        ShowWorldSelectionMenu();
    }
    else
    {
        CancelWorldSelectionReturnInputGuard();
        ShowStartMenu();
    }
}

void AStartActor::StartGame()
{
    ShowWorldSelectionMenu();
}

void AStartActor::ReturnToMainMenuFromWorldSelection()
{
    if (IsWorldSelectionReturnBlocked())
    {
        UE_LOG(LogTemp, Display,
            TEXT("[WorldSelection] Ignored a carried-over Back callback while the return input guard is active."));
        return;
    }

    if (bGameplayWorldTravelPending)
    {
        UE_LOG(LogTemp, Display,
            TEXT("[WorldSelection] Ignored a return-to-main-menu callback because travel to world '%s' is already pending."),
            *PendingGameplayWorldFolderName);
        return;
    }

    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->ClearWorldSelectionMenuRequest();
        GameManager->SetGamePaused(false);
    }

    ShowStartMenu();
}

void AStartActor::ShowStartMenu()
{
    if (IsWorldSelectionReturnBlocked())
    {
        UE_LOG(LogTemp, Display,
            TEXT("[WorldSelection] Ignored a carried-over start-menu callback while returning from gameplay."));
        return;
    }

    if (bGameplayWorldTravelPending)
    {
        UE_LOG(LogTemp, Display,
            TEXT("[WorldSelection] Ignored ShowStartMenu while travel to world '%s' is pending."),
            *PendingGameplayWorldFolderName);
        return;
    }

    UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this);
    CancelWorldSelectionReturnInputGuard();
    if (IsValid(GameManager))
    {
        GameManager->ClearWorldSelectionMenuRequest();
    }

    RemoveAllMenuWidgets();
    ResetEditorTransactionBufferForMenuTravel(TEXT("Show start menu"));

    StartMenuWidget = CreateAndAddMenuWidget(
        StartMenuWidgetClass.Get(),
        TEXT("StartMenuWidgetClass"));

    ApplyMenuInputMode(StartMenuWidget.Get());
}

void AStartActor::ShowWorldSelectionMenu()
{
    if (bGameplayWorldTravelPending)
    {
        UE_LOG(LogTemp, Display,
            TEXT("[WorldSelection] Ignored a duplicate world-selection request while travel to world '%s' is pending."),
            *PendingGameplayWorldFolderName);
        return;
    }

    BuildLevelFolderNameMap();
    RemoveAllMenuWidgets();
    ResetEditorTransactionBufferForMenuTravel(TEXT("Show single-player world-selection menu"));

    WorldSelectionWidget = Cast<UWorldSelectionWidget>(CreateAndAddMenuWidget(
        WorldSelectionWidgetClass.Get(),
        TEXT("WorldSelectionWidgetClass")));

    if (IsValid(WorldSelectionWidget))
    {
        WorldSelectionWidget->SetWorldSelectionData(FolderNameMap);
    }

    ApplyMenuInputMode(WorldSelectionWidget.Get());

    if (bWorldSelectionReturnInputGuardActive)
    {
        StartWorldSelectionReturnInputGuard();
    }
}

void AStartActor::ShowMultiplayerMenu()
{
    if (IsWorldSelectionReturnBlocked())
    {
        UE_LOG(LogTemp, Display,
            TEXT("[WorldSelection] Ignored a carried-over multiplayer-menu callback while returning from gameplay."));
        return;
    }

    CancelWorldSelectionReturnInputGuard();
    BuildLevelFolderNameMap();
    RemoveAllMenuWidgets();
    ResetEditorTransactionBufferForMenuTravel(TEXT("Show multiplayer menu"));

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
    if (IsWorldSelectionReturnBlocked())
    {
        UE_LOG(LogTemp, Display,
            TEXT("[WorldSelection] Ignored carried-over activation for '%s' while the return input guard is active."),
            *WorldFolderName);
        return;
    }

    if (bGameplayWorldTravelPending)
    {
        UE_LOG(LogTemp, Display,
            TEXT("[WorldSelection] Ignored duplicate world click '%s'; travel to '%s' is already pending."),
            *WorldFolderName,
            *PendingGameplayWorldFolderName);
        return;
    }

    FString ResolvedFolderName;
    if (!TryResolveWorldFolderFromDisplayName(WorldFolderName, ResolvedFolderName))
    {
        UE_LOG(LogTemp, Warning, TEXT("StartActor cannot open single-player world. Unknown world folder/display name: %s"), *WorldFolderName);
        return;
    }

    if (GameplayWorld.IsNull())
    {
        UE_LOG(LogTemp, Error,
            TEXT("[WorldSelection] Cannot open world '%s' because GameplayWorld is not assigned."),
            *ResolvedFolderName);
        return;
    }

    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->SetCurrentWorldName(ResolvedFolderName);
        GameManager->ClearWorldSelectionMenuRequest();
        GameManager->SetGamePaused(false);
    }

    bGameplayWorldTravelPending = true;
    PendingGameplayWorldFolderName = ResolvedFolderName;

    UE_LOG(LogTemp, Display,
        TEXT("[WorldSelection] Opening the directly assigned gameplay world. Folder=%s"),
        *ResolvedFolderName);

    PrepareMenuForWorldTravel();

    // Arm this before world travel. Successful travel destroys this actor and clears the timer; a failed
    // travel leaves the menu world alive and restores the world list instead of showing a blank screen.
    const float WatchdogDelay = FMath::Max(1.0f, GameplayTravelFailureTimeoutSeconds);
    GetWorldTimerManager().SetTimer(
        GameplayTravelWatchdogHandle,
        this,
        &AStartActor::HandleGameplayTravelWatchdogExpired,
        WatchdogDelay,
        false);

    bool bTravelRequested = false;
    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        bTravelRequested = Multiplayer->StartSinglePlayerWorld(this, ResolvedFolderName, GameplayWorld);
    }
    else
    {
        const FString Options = FString::Printf(TEXT("World=%s"), *ResolvedFolderName);
        UGameplayStatics::OpenLevelBySoftObjectPtr(this, GameplayWorld, true, Options);
        bTravelRequested = true;
    }

    if (!bTravelRequested)
    {
        UE_LOG(LogTemp, Error,
            TEXT("[WorldSelection] Failed to request travel to the assigned gameplay world. Folder=%s"),
            *ResolvedFolderName);
        GetWorldTimerManager().ClearTimer(GameplayTravelWatchdogHandle);
        bGameplayWorldTravelPending = false;
        PendingGameplayWorldFolderName.Reset();
        ShowWorldSelectionMenu();
        return;
    }
}

void AStartActor::HostMultiplayerWorldByFolderName(const FString& WorldFolderName)
{
    if (IsWorldSelectionReturnBlocked())
    {
        return;
    }

    FString ResolvedFolderName;
    if (!TryResolveWorldFolderFromDisplayName(WorldFolderName, ResolvedFolderName))
    {
        UE_LOG(LogTemp, Warning, TEXT("StartActor cannot host multiplayer world. Unknown world folder/display name: %s"), *WorldFolderName);
        return;
    }

    if (HostWorld.IsNull())
    {
        UE_LOG(LogTemp, Warning, TEXT("StartActor cannot host a multiplayer world because HostWorld is not assigned."));
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
        Multiplayer->HostMultiplayerWorld(this, ResolvedFolderName, HostWorld);
    }
}

void AStartActor::OpenClientConnectionWorld(const FString& InServerAddress)
{
    if (IsWorldSelectionReturnBlocked())
    {
        return;
    }

    if (ClientWorld.IsNull())
    {
        UE_LOG(LogTemp, Warning, TEXT("StartActor cannot open the client connection world because ClientWorld is not assigned."));
        return;
    }

    SetPendingServerAddress(InServerAddress);
    PrepareMenuForWorldTravel();
    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        Multiplayer->SetServerAddress(PendingServerAddress);
        Multiplayer->OpenClientConnectionWorld(this, ClientWorld);
    }
}

void AStartActor::JoinMultiplayerServer(const FString& InServerAddress, const FString& OptionalWorldFolderName)
{
    if (IsWorldSelectionReturnBlocked())
    {
        return;
    }

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

void AStartActor::HandleGameplayTravelWatchdogExpired()
{
    if (!bGameplayWorldTravelPending || IsActorBeingDestroyed())
    {
        return;
    }

    const FString FailedWorldFolder = PendingGameplayWorldFolderName;
    bGameplayWorldTravelPending = false;
    PendingGameplayWorldFolderName.Reset();

    UE_LOG(LogTemp, Error,
        TEXT("[WorldSelection] Travel to the assigned gameplay world did not complete. Restoring the world list. "
             "Confirm that the world reference is assigned and included in the packaged build. World=%s"),
        *FailedWorldFolder);

    ShowWorldSelectionMenu();
}

void AStartActor::StartWorldSelectionReturnInputGuard()
{
    bWorldSelectionReturnInputGuardActive = true;
    if (IsValid(WorldSelectionWidget))
    {
        WorldSelectionWidget->SetIsEnabled(false);
    }

    const float Delay = FMath::Clamp(WorldSelectionReturnInputGuardSeconds, 0.05f, 2.0f);
    GetWorldTimerManager().ClearTimer(WorldSelectionReturnInputGuardHandle);
    GetWorldTimerManager().SetTimer(
        WorldSelectionReturnInputGuardHandle,
        this,
        &AStartActor::TryReleaseWorldSelectionReturnInputGuard,
        Delay,
        false);

    UE_LOG(LogTemp, Display, TEXT("[WorldSelection] Return input guard armed for %.2f seconds."), Delay);
}

bool AStartActor::IsWorldSelectionActivationInputHeld() const
{
    APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
    if (!PlayerController)
    {
        return false;
    }

    return PlayerController->IsInputKeyDown(EKeys::LeftMouseButton)
        || PlayerController->IsInputKeyDown(EKeys::RightMouseButton)
        || PlayerController->IsInputKeyDown(EKeys::Enter)
        || PlayerController->IsInputKeyDown(EKeys::SpaceBar)
        || PlayerController->IsInputKeyDown(EKeys::Gamepad_FaceButton_Bottom);
}

bool AStartActor::IsWorldSelectionReturnBlocked() const
{
    if (bWorldSelectionReturnInputGuardActive)
    {
        return true;
    }

    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        return GameManager->ShouldOpenWorldSelectionMenuOnNextMainWorld();
    }

    return false;
}

void AStartActor::TryReleaseWorldSelectionReturnInputGuard()
{
    if (!bWorldSelectionReturnInputGuardActive || IsActorBeingDestroyed())
    {
        return;
    }

    if (IsWorldSelectionActivationInputHeld())
    {
        // Do not expose the world buttons until the exact input that initiated Exit has been released.
        GetWorldTimerManager().SetTimer(
            WorldSelectionReturnInputGuardHandle,
            this,
            &AStartActor::TryReleaseWorldSelectionReturnInputGuard,
            0.05f,
            false);
        return;
    }

    bWorldSelectionReturnInputGuardActive = false;
    GetWorldTimerManager().ClearTimer(WorldSelectionReturnInputGuardHandle);
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        // The arrival transaction is complete only now. World travel may be initiated again by a
        // deliberate button click after this point.
        GameManager->ClearWorldSelectionMenuRequest();
    }
    if (IsValid(WorldSelectionWidget))
    {
        WorldSelectionWidget->SetIsEnabled(true);
    }

    UE_LOG(LogTemp, Display, TEXT("[WorldSelection] Return input guard released; world selection is ready."));
}

void AStartActor::CancelWorldSelectionReturnInputGuard()
{
    bWorldSelectionReturnInputGuardActive = false;
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(WorldSelectionReturnInputGuardHandle);
    }
    if (IsValid(WorldSelectionWidget))
    {
        WorldSelectionWidget->SetIsEnabled(true);
    }
}

void AStartActor::PrepareMenuForWorldTravel()
{
    CancelWorldSelectionReturnInputGuard();
    RemoveAllMenuWidgets();
    ResetEditorTransactionBufferForMenuTravel(TEXT("Menu world travel"));
}

UClass* AStartActor::ResolveMenuWidgetClass(UClass* WidgetClass, const TCHAR* DebugWidgetName) const
{
    UClass* ResolvedClass = WidgetClass;
    if (!ResolvedClass || !ResolvedClass->IsChildOf(UStartWorldWidget::StaticClass()))
    {
        UE_LOG(LogTemp, Warning,
               TEXT("StartActor cannot create %s because no valid widget class is assigned."),
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
        Reason ? FString(Reason) : FString(TEXT("Menu world travel")));
}
