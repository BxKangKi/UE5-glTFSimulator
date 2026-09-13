// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

/**
 * @file MainGameMode.cpp
 * 역할: 메뉴 월드의 UI와 목적지 맵 선택을 관리합니다.
 * 핵심 기능: 폴더 목록, 맵·GameMode profile, 선택값 전달, travel watchdog.
 * UObject/Actor 접근은 게임 스레드에서 수행하고, worker에는 독립된 native 데이터를 전달하십시오.
 */

#include "GameMode/MainGameMode.h"
#include "GameMode/SingleplayGameMode.h"
#include "GameMode/MultiplayGameMode.h"

#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
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
#include "UI/SettingsMenuWidget.h"
#include "UI/ProjectSelectionWidget.h"
#include "Blueprint/UserWidget.h"
#include "System/GlTFSimulatorGameInstance.h"
#include "System/GlTFSimulatorAssetRegistry.h"
#include "System/WorldArchive.h"
#include "System/SafeFileIO.h"
#include "World/WorldData.h"

namespace
{
    FString NormalizeStartWorldString(FString Value)
    {
        Value.TrimStartAndEndInline();
        return Value;
    }

    /** Adds one travel option without inheriting any option from the menu map. */
    void AppendStartWorldTravelOption(FString& InOutOptions, const FString& Key, const FString& Value)
    {
        if (Key.IsEmpty() || Value.IsEmpty())
        {
            return;
        }

        if (!InOutOptions.IsEmpty())
        {
            InOutOptions += TEXT("?");
        }
        InOutOptions += Key;
        InOutOptions += TEXT("=");
        InOutOptions += Value;
    }
}

AMainGameMode::AMainGameMode()
{
    PrimaryActorTick.bCanEverTick = false;

    PendingServerAddress = DefaultServerAddress;

    // Registry widget classes are auto-created in BeginPlay. Set*Widget remains an optional
    // compatibility/override path for projects that explicitly construct a custom instance.
}

void AMainGameMode::SetStartMenuWidget(UStartWorldWidget* InWidget)
{
    StartMenuWidget = InWidget;
    if (IsValid(InWidget))
    {
        InWidget->SetMainGameMode(this);
    }
}

void AMainGameMode::SetWorldSelectionWidget(UWorldSelectionWidget* InWidget)
{
    WorldSelectionWidget = InWidget;
    if (IsValid(InWidget))
    {
        InWidget->SetMainGameMode(this);
        InWidget->SetWorldSelectionData(FolderNameMap);
    }
}

void AMainGameMode::SetMultiplayerMenuWidget(UStartWorldWidget* InWidget)
{
    MultiplayerMenuWidget = InWidget;
    if (IsValid(InWidget))
    {
        InWidget->SetMainGameMode(this);
        InWidget->SetWorldSelectionData(FolderNameMap);
    }
}

void AMainGameMode::SetSettingsWidget(USettingsMenuWidget* InWidget)
{
    if (IsValid(SettingsWidget))
    {
        SettingsWidget->OnCloseRequested.RemoveDynamic(this, &AMainGameMode::ReturnFromSettings);
    }
    SettingsWidget = InWidget;
    if (IsValid(SettingsWidget))
    {
        SettingsWidget->OnCloseRequested.RemoveDynamic(this, &AMainGameMode::ReturnFromSettings);
        SettingsWidget->OnCloseRequested.AddDynamic(this, &AMainGameMode::ReturnFromSettings);
        SettingsWidget->SetVisibility(ESlateVisibility::Collapsed);
    }
}

void AMainGameMode::ShowSettingsMenu()
{
    HideAllMenuWidgets();
    if (!IsValid(SettingsWidget))
    {
        UE_LOG(LogTemp, Warning, TEXT("MainGameMode has no registered Settings widget. Blueprint must create it and call SetSettingsWidget."));
        ShowStartMenu();
        return;
    }
    SettingsWidget->InitializeSettingsFromSavedData();
    SettingsWidget->SetVisibility(ESlateVisibility::Visible);
    ApplyMenuInputMode(SettingsWidget.Get());
}

void AMainGameMode::ReturnFromSettings()
{
    if (IsValid(SettingsWidget))
    {
        SettingsWidget->SetVisibility(ESlateVisibility::Collapsed);
    }
    ShowStartMenu();
}

void AMainGameMode::BeginPlay()
{
    // The registry is class-backed and privately instantiated by the GameInstance. Prepare it before
    // Blueprint ReceiveBeginPlay so even legacy menu Blueprint code can resolve central assets.
    if (UGlTFSimulatorGameInstance* SimulatorGameInstance = Cast<UGlTFSimulatorGameInstance>(GetGameInstance()))
    {
        SimulatorGameInstance->EnsureAssetRegistry();
    }

    // Arm before Super::BeginPlay so a legacy Blueprint ReceiveBeginPlay cannot consume the
    // carried-over Exit click before the native menu is rebuilt later in this same BeginPlay call.
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        bWorldSelectionReturnInputGuardActive = GameManager->ShouldOpenWorldSelectionMenuOnNextMainWorld();
    }

    // The menu world must not retain an explicit gameplay GameMode request from the previous travel.
    // A later world with no override must be free to use its own World Settings.
    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        Multiplayer->ClearRequestedGameModeOverride();
    }

    Super::BeginPlay();

    // Rebuild the level list before any UI asks for it.
    BuildLevelFolderNameMap();

    // Blueprint ReceiveBeginPlay has completed when Super::BeginPlay() returns. Keep any instances
    // explicitly registered by Blueprint, then fill only the missing slots from AssetRegistryClass.
    InitializeRegistryDrivenUI();
    InitializeStartScreenAfterBlueprintBeginPlay();
}

void AMainGameMode::InitializeRegistryDrivenUI()
{
    APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
    if (!IsValid(PlayerController) || !PlayerController->IsLocalController())
    {
        return;
    }

    UGlTFSimulatorAssetRegistry* Registry = UGlTFSimulatorGameInstance::GetAssetRegistryFromContext(this);
    if (!IsValid(Registry))
    {
        UE_LOG(LogTemp, Error, TEXT("MainGameMode cannot initialize registry-driven UI because the central AssetRegistry is unavailable."));
        return;
    }

    const auto AddTopLevelWidget = [](UUserWidget* Widget, const int32 ZOrder)
    {
        if (IsValid(Widget) && !Widget->IsInViewport())
        {
            Widget->AddToPlayerScreen(ZOrder);
        }
    };

    if (!IsValid(StartMenuWidget) && !Registry->StartMenuWidgetClass.IsNull())
    {
        if (UClass* WidgetClass = Registry->StartMenuWidgetClass.LoadSynchronous())
        {
            UStartWorldWidget* Widget = CreateWidget<UStartWorldWidget>(PlayerController, WidgetClass);
            SetStartMenuWidget(Widget);
            AddTopLevelWidget(Widget, 10);
        }
    }

    if (!IsValid(WorldSelectionWidget) && !Registry->WorldSelectionWidgetClass.IsNull())
    {
        if (UClass* WidgetClass = Registry->WorldSelectionWidgetClass.LoadSynchronous())
        {
            UWorldSelectionWidget* Widget = CreateWidget<UWorldSelectionWidget>(PlayerController, WidgetClass);
            SetWorldSelectionWidget(Widget);
            AddTopLevelWidget(Widget, 11);
        }
    }

    if (!IsValid(MultiplayerMenuWidget) && !Registry->MultiplayerMenuWidgetClass.IsNull())
    {
        if (UClass* WidgetClass = Registry->MultiplayerMenuWidgetClass.LoadSynchronous())
        {
            UStartWorldWidget* Widget = CreateWidget<UStartWorldWidget>(PlayerController, WidgetClass);
            SetMultiplayerMenuWidget(Widget);
            AddTopLevelWidget(Widget, 12);
        }
    }

    if (!IsValid(SettingsWidget) && !Registry->SettingsMenuWidgetClass.IsNull())
    {
        if (UClass* WidgetClass = Registry->SettingsMenuWidgetClass.LoadSynchronous())
        {
            USettingsMenuWidget* Widget = CreateWidget<USettingsMenuWidget>(PlayerController, WidgetClass);
            SetSettingsWidget(Widget);
            AddTopLevelWidget(Widget, 20);
        }
    }

    // Project selection is owned by the start widget rather than MainGameMode. Create and register it
    // automatically so the WBP only needs to expose/bind its internal buttons/panel.
    if (IsValid(StartMenuWidget)
        && !IsValid(StartMenuWidget->GetProjectSelectionWidget())
        && !Registry->ProjectSelectionWidgetClass.IsNull())
    {
        if (UClass* WidgetClass = Registry->ProjectSelectionWidgetClass.LoadSynchronous())
        {
            UProjectSelectionWidget* ProjectWidget = CreateWidget<UProjectSelectionWidget>(PlayerController, WidgetClass);
            if (IsValid(ProjectWidget))
            {
                ProjectWidget->SetVisibility(ESlateVisibility::Collapsed);
                AddTopLevelWidget(ProjectWidget, 30);
                StartMenuWidget->SetProjectSelectionWidget(ProjectWidget);
            }
        }
    }

    HideAllMenuWidgets();
}

void AMainGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (IsValid(SettingsWidget))
    {
        SettingsWidget->OnCloseRequested.RemoveDynamic(this, &AMainGameMode::ReturnFromSettings);
    }

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(GameplayTravelWatchdogHandle);
        World->GetTimerManager().ClearTimer(WorldSelectionReturnInputGuardHandle);
    }
    HideAllMenuWidgets();
    ResetEditorTransactionBufferForMenuTravel(TEXT("MainGameMode destroyed"));

    // Do not force garbage collection from EndPlay. During level travel this can block asset loading
    // and make the editor/game appear stuck around the loading-progress phase. Resetting editor
    // transactions before Super::EndPlay avoids stale REINST widget world references in PIE.
    Super::EndPlay(EndPlayReason);
}

void AMainGameMode::InitializeStartScreenAfterBlueprintBeginPlay()
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

void AMainGameMode::StartGame()
{
    ShowWorldSelectionMenu();
}

void AMainGameMode::ReturnToMainMenuFromWorldSelection()
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

void AMainGameMode::ShowStartMenu()
{
    if (IsWorldSelectionReturnBlocked() || bGameplayWorldTravelPending)
    {
        return;
    }
    CancelWorldSelectionReturnInputGuard();
    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->ClearWorldSelectionMenuRequest();
    }
    HideAllMenuWidgets();
    if (IsValid(StartMenuWidget))
    {
        StartMenuWidget->SetVisibility(ESlateVisibility::Visible);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("MainGameMode has no registered StartMenu widget."));
    }
    ApplyMenuInputMode(StartMenuWidget.Get());
}

void AMainGameMode::ShowWorldSelectionMenu()
{
    if (bGameplayWorldTravelPending)
    {
        return;
    }
    BuildLevelFolderNameMap();
    HideAllMenuWidgets();
    if (IsValid(WorldSelectionWidget))
    {
        WorldSelectionWidget->SetWorldSelectionData(FolderNameMap);
        WorldSelectionWidget->SetVisibility(ESlateVisibility::Visible);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("MainGameMode has no registered WorldSelection widget."));
    }
    ApplyMenuInputMode(WorldSelectionWidget.Get());
    if (bWorldSelectionReturnInputGuardActive)
    {
        StartWorldSelectionReturnInputGuard();
    }
}

void AMainGameMode::ShowMultiplayerMenu()
{
    if (IsWorldSelectionReturnBlocked())
    {
        return;
    }
    CancelWorldSelectionReturnInputGuard();
    BuildLevelFolderNameMap();
    HideAllMenuWidgets();
    if (IsValid(MultiplayerMenuWidget))
    {
        MultiplayerMenuWidget->SetWorldSelectionData(FolderNameMap);
        MultiplayerMenuWidget->SetVisibility(ESlateVisibility::Visible);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("MainGameMode has no registered Multiplayer widget."));
    }
    ApplyMenuInputMode(MultiplayerMenuWidget.Get());
}

void AMainGameMode::RefreshWorldFolderNameMap()
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

bool AMainGameMode::TryResolveWorldFolderFromDisplayName(const FString& DisplayName, FString& OutFolderName) const
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

    FString MatchingFolder;
    for (const TPair<FString, FString>& Pair : FolderNameMap)
    {
        if (NormalizeStartWorldString(Pair.Value).Equals(NormalizedInput, ESearchCase::IgnoreCase))
        {
            if (!MatchingFolder.IsEmpty() && MatchingFolder != Pair.Key)
            {
                // TMap iteration order is intentionally unspecified. Reject an ambiguous display
                // name instead of opening a different physical world on another build/platform.
                UE_LOG(LogTemp, Error,
                    TEXT("[WorldSelection] Display name '%s' is ambiguous between folders '%s' and '%s'. Pass the folder key or make WorldName values unique."),
                    *NormalizedInput, *MatchingFolder, *Pair.Key);
                OutFolderName.Reset();
                return false;
            }
            MatchingFolder = Pair.Key;
        }
    }

    OutFolderName = MoveTemp(MatchingFolder);
    return !OutFolderName.IsEmpty();
}

const FGlTFWorldLaunchProfile* AMainGameMode::FindWorldLaunchProfile(const FString& WorldFolderName) const
{
    const FString NormalizedFolderName = NormalizeStartWorldString(WorldFolderName);
    if (NormalizedFolderName.IsEmpty())
    {
        return nullptr;
    }

    const UGlTFSimulatorAssetRegistry* Registry =
        UGlTFSimulatorGameInstance::GetAssetRegistryFromContext(this);
    if (!IsValid(Registry))
    {
        return nullptr;
    }

    const FGlTFWorldLaunchProfile* Match = nullptr;
    for (const FGlTFWorldLaunchProfile& Profile : Registry->WorldLaunchProfiles)
    {
        const FString ProfileFolderName = NormalizeStartWorldString(Profile.WorldFolderName);
        if (!ProfileFolderName.Equals(NormalizedFolderName, ESearchCase::IgnoreCase))
        {
            continue;
        }

        if (Match)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[GameModeTravel] Duplicate launch profile for folder '%s'. The first profile is used."),
                *NormalizedFolderName);
            continue;
        }
        Match = &Profile;
    }

    return Match;
}

void AMainGameMode::ResolveWorldLaunch(
    const FString& WorldFolderName,
    bool bForHost,
    TSoftObjectPtr<UWorld>& OutWorld,
    TSoftClassPtr<AGameModeBase>& OutGameModeOverride,
    FString& OutResolutionSource) const
{
    const UGlTFSimulatorAssetRegistry* Registry =
        UGlTFSimulatorGameInstance::GetAssetRegistryFromContext(this);
    OutWorld = Registry
        ? (bForHost ? Registry->HostWorld : Registry->GameplayWorld)
        : TSoftObjectPtr<UWorld>();

    UClass* RequestedGameMode = nullptr;
    if (Registry)
    {
        RequestedGameMode = bForHost
            ? Registry->MultiplayGameModeClass.LoadSynchronous()
            : Registry->SingleplayGameModeClass.LoadSynchronous();
    }
    if (!IsValid(RequestedGameMode))
    {
        RequestedGameMode = bForHost
            ? AMultiplayGameMode::StaticClass()
            : ASingleplayGameMode::StaticClass();
    }
    OutGameModeOverride = RequestedGameMode;
    OutResolutionSource = bForHost
        ? FString(TEXT("Common HostWorld + MultiplayGameMode"))
        : FString(TEXT("Common GameplayWorld + SingleplayGameMode"));

    const FGlTFWorldLaunchProfile* Profile = FindWorldLaunchProfile(WorldFolderName);
    if (!Profile)
    {
        return;
    }

    TSoftObjectPtr<UWorld> ProfileWorld = bForHost
        ? Profile->HostWorld
        : Profile->SinglePlayerWorld;
    if (bForHost && ProfileWorld.IsNull() && !Profile->SinglePlayerWorld.IsNull())
    {
        ProfileWorld = Profile->SinglePlayerWorld;
    }

    if (!ProfileWorld.IsNull())
    {
        OutWorld = ProfileWorld;
        OutResolutionSource = bForHost
            ? FString(TEXT("Profile world + MultiplayGameMode"))
            : FString(TEXT("Profile SinglePlayerWorld + SingleplayGameMode"));
    }
}

void AMainGameMode::OpenSinglePlayerWorldByFolderName(const FString& WorldFolderName)
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
        UE_LOG(LogTemp, Warning, TEXT("MainGameMode cannot open single-player world. Unknown world folder/display name: %s"), *WorldFolderName);
        return;
    }

    TSoftObjectPtr<UWorld> SelectedGameplayWorld;
    TSoftClassPtr<AGameModeBase> SelectedGameModeOverride;
    FString LaunchResolutionSource;
    ResolveWorldLaunch(
        ResolvedFolderName,
        false,
        SelectedGameplayWorld,
        SelectedGameModeOverride,
        LaunchResolutionSource);

    if (SelectedGameplayWorld.IsNull())
    {
        UE_LOG(LogTemp, Error,
            TEXT("[WorldSelection] Cannot open world '%s': neither the folder profile nor GameplayWorld has a map assigned."),
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
        TEXT("[GameModeTravel] Single-player selection. Folder=%s World=%s GameMode=%s Source=%s"),
        *ResolvedFolderName,
        *SelectedGameplayWorld.ToSoftObjectPath().ToString(),
        SelectedGameModeOverride.IsNull()
            ? TEXT("<map World Settings>")
            : *SelectedGameModeOverride.ToSoftObjectPath().ToString(),
        *LaunchResolutionSource);

    PrepareMenuForWorldTravel();

    // Arm this before world travel. Successful travel destroys this actor and clears the timer; a failed
    // travel leaves the menu world alive and restores the world list instead of showing a blank screen.
    const float WatchdogDelay = FMath::Max(1.0f, GameplayTravelFailureTimeoutSeconds);
    GetWorldTimerManager().SetTimer(
        GameplayTravelWatchdogHandle,
        this,
        &AMainGameMode::HandleGameplayTravelWatchdogExpired,
        WatchdogDelay,
        false);

    bool bTravelRequested = false;
    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        bTravelRequested = Multiplayer->StartSinglePlayerWorldWithGameMode(
            this,
            ResolvedFolderName,
            SelectedGameplayWorld,
            SelectedGameModeOverride);
    }
    else
    {
        FString Options;
        AppendStartWorldTravelOption(Options, TEXT("World"), ResolvedFolderName);
        if (!SelectedGameModeOverride.IsNull())
        {
            AppendStartWorldTravelOption(
                Options,
                TEXT("game"),
                SelectedGameModeOverride.ToSoftObjectPath().ToString());
        }
        UGameplayStatics::OpenLevelBySoftObjectPtr(this, SelectedGameplayWorld, true, Options);
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
        if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
        {
            Multiplayer->ClearRequestedGameModeOverride();
        }
        ShowWorldSelectionMenu();
        return;
    }
}

void AMainGameMode::HostMultiplayerWorldByFolderName(const FString& WorldFolderName)
{
    if (IsWorldSelectionReturnBlocked())
    {
        return;
    }

    FString ResolvedFolderName;
    if (!TryResolveWorldFolderFromDisplayName(WorldFolderName, ResolvedFolderName))
    {
        UE_LOG(LogTemp, Warning, TEXT("MainGameMode cannot host multiplayer world. Unknown world folder/display name: %s"), *WorldFolderName);
        return;
    }

    TSoftObjectPtr<UWorld> SelectedHostWorld;
    TSoftClassPtr<AGameModeBase> SelectedGameModeOverride;
    FString LaunchResolutionSource;
    ResolveWorldLaunch(
        ResolvedFolderName,
        true,
        SelectedHostWorld,
        SelectedGameModeOverride,
        LaunchResolutionSource);

    if (SelectedHostWorld.IsNull())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("MainGameMode cannot host world '%s': neither the folder profile nor HostWorld has a map assigned."),
            *ResolvedFolderName);
        return;
    }

    if (UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(this))
    {
        GameManager->SetCurrentWorldName(ResolvedFolderName);
        GameManager->ClearWorldSelectionMenuRequest();
        GameManager->SetGamePaused(false);
    }

    UE_LOG(LogTemp, Display,
        TEXT("[GameModeTravel] Host selection. Folder=%s World=%s GameMode=%s Source=%s"),
        *ResolvedFolderName,
        *SelectedHostWorld.ToSoftObjectPath().ToString(),
        SelectedGameModeOverride.IsNull()
            ? TEXT("<map World Settings>")
            : *SelectedGameModeOverride.ToSoftObjectPath().ToString(),
        *LaunchResolutionSource);

    PrepareMenuForWorldTravel();
    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        Multiplayer->HostMultiplayerWorldWithGameMode(
            this,
            ResolvedFolderName,
            SelectedHostWorld,
            SelectedGameModeOverride);
    }
    else
    {
        FString Options(TEXT("listen"));
        AppendStartWorldTravelOption(Options, TEXT("World"), ResolvedFolderName);
        if (!SelectedGameModeOverride.IsNull())
        {
            AppendStartWorldTravelOption(
                Options,
                TEXT("game"),
                SelectedGameModeOverride.ToSoftObjectPath().ToString());
        }
        UGameplayStatics::OpenLevelBySoftObjectPtr(this, SelectedHostWorld, true, Options);
    }
}

void AMainGameMode::OpenClientConnectionWorld(const FString& InServerAddress)
{
    if (IsWorldSelectionReturnBlocked())
    {
        return;
    }

    const UGlTFSimulatorAssetRegistry* Registry =
        UGlTFSimulatorGameInstance::GetAssetRegistryFromContext(this);
    const TSoftObjectPtr<UWorld> ClientWorld = Registry
        ? Registry->ClientWorld : TSoftObjectPtr<UWorld>();
    if (ClientWorld.IsNull())
    {
        UE_LOG(LogTemp, Warning, TEXT("MainGameMode cannot open the client connection world because AssetRegistry.ClientWorld is not assigned."));
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

void AMainGameMode::JoinMultiplayerServer(const FString& InServerAddress, const FString& OptionalWorldFolderName)
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

void AMainGameMode::SetPendingServerAddress(const FString& InServerAddress)
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

void AMainGameMode::HandleGameplayTravelWatchdogExpired()
{
    if (!bGameplayWorldTravelPending || IsActorBeingDestroyed())
    {
        return;
    }

    const FString FailedWorldFolder = PendingGameplayWorldFolderName;
    bGameplayWorldTravelPending = false;
    PendingGameplayWorldFolderName.Reset();
    if (UMultiplayerWorldSubSystem* Multiplayer = UMultiplayerWorldSubSystem::Get(this))
    {
        Multiplayer->ClearRequestedGameModeOverride();
    }

    UE_LOG(LogTemp, Error,
        TEXT("[WorldSelection] Travel to the assigned gameplay world did not complete. Restoring the world list. "
             "Confirm that the world reference is assigned and included in the packaged build. World=%s"),
        *FailedWorldFolder);

    ShowWorldSelectionMenu();
}

void AMainGameMode::StartWorldSelectionReturnInputGuard()
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
        &AMainGameMode::TryReleaseWorldSelectionReturnInputGuard,
        Delay,
        false);

    UE_LOG(LogTemp, Display, TEXT("[WorldSelection] Return input guard armed for %.2f seconds."), Delay);
}

bool AMainGameMode::IsWorldSelectionActivationInputHeld() const
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

bool AMainGameMode::IsWorldSelectionReturnBlocked() const
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

void AMainGameMode::TryReleaseWorldSelectionReturnInputGuard()
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
            &AMainGameMode::TryReleaseWorldSelectionReturnInputGuard,
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

void AMainGameMode::CancelWorldSelectionReturnInputGuard()
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

void AMainGameMode::PrepareMenuForWorldTravel()
{
    CancelWorldSelectionReturnInputGuard();
    HideAllMenuWidgets();
    ResetEditorTransactionBufferForMenuTravel(TEXT("Menu world travel"));
}

void AMainGameMode::HideAllMenuWidgets()
{
    if (IsValid(StartMenuWidget)) StartMenuWidget->SetVisibility(ESlateVisibility::Collapsed);
    if (IsValid(WorldSelectionWidget)) WorldSelectionWidget->SetVisibility(ESlateVisibility::Collapsed);
    if (IsValid(MultiplayerMenuWidget)) MultiplayerMenuWidget->SetVisibility(ESlateVisibility::Collapsed);
    if (IsValid(SettingsWidget)) SettingsWidget->SetVisibility(ESlateVisibility::Collapsed);
}

void AMainGameMode::ApplyMenuInputMode(UUserWidget* FocusWidget) const
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
    // Supplying a non-focusable SObjectWidget makes PlayerController emit an error every time a
    // menu opens. Mouse-only menu roots do not need explicit keyboard focus.
    if (IsValid(FocusWidget) && FocusWidget->IsFocusable())
    {
        InputMode.SetWidgetToFocus(FocusWidget->TakeWidget());
    }
    PlayerController->SetInputMode(InputMode);
}

void AMainGameMode::BuildLevelFolderNameMap()
{
    FolderNameMap.Empty();
    TArray<FString> WorldFiles;
    IFileManager::Get().FindFiles(WorldFiles,
        *FPaths::Combine(PATH_WORLDS, TEXT("*.gwd")), true, false);
    WorldFiles.Sort([](const FString& A, const FString& B)
    {
        return A.Compare(B, ESearchCase::IgnoreCase) < 0;
    });

    for (const FString& FileName : WorldFiles)
    {
        const FString WorldKey = FPaths::GetBaseFilename(FileName);
        FString SafeKey;
        if (!UGameManagerSubSystem::TryNormalizeWorldFolderName(WorldKey, SafeKey, false)
            || SafeKey != WorldKey) continue;

        FString Error;
        const FString ArchivePath = FPaths::Combine(PATH_WORLDS, FileName);
        const TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> Reader =
            FGWorldArchiveReader::Open(ArchivePath, Error);
        FString ConfigText;
        if (!Reader.IsValid() || !Reader->ReadWorldConfig(ConfigText, Error))
        {
            UE_LOG(LogTemp, Error,
                TEXT("[WorldSelection] Ignoring invalid .gwd '%s': %s"), *ArchivePath, *Error);
            continue;
        }
        const FSafeJsonLoadResult Config = FSafeFileIO::ParseJsonText(
            ConfigText, ArchivePath + TEXT("#config.json"));
        FString DisplayName;
        if (!Config.IsSuccess()
            || !Config.JsonObject->TryGetStringField(CONFIG_WORLD_NAME_FIELD, DisplayName))
        {
            UE_LOG(LogTemp, Error,
                TEXT("[WorldSelection] Ignoring .gwd without a valid config WorldName: %s"), *ArchivePath);
            continue;
        }
        DisplayName.TrimStartAndEndInline();
        if (DisplayName.IsEmpty()) continue;
        FolderNameMap.Add(SafeKey, MoveTemp(DisplayName));
    }
}

void AMainGameMode::ResetEditorTransactionBufferForMenuTravel(const TCHAR* Reason) const
{
    if (!bResetEditorTransactionsBeforeTravel)
    {
        return;
    }

    UGameManagerSubSystem::ResetEditorTransactionBufferForWorldTravel(
        this,
        Reason ? FString(Reason) : FString(TEXT("Menu world travel")));
}
