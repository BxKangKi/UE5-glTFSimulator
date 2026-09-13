// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

/**
 * @file StartWorldWidget.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "UI/StartWorldWidget.h"

#include "Components/Button.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "GameMode/MainGameMode.h"
#include "UI/ProjectSelectionWidget.h"
#include "System/GameManagerSubSystem.h"
#include "System/MacroLibrary.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"

namespace
{
    FString NormalizeStartWorldText(FString Value)
    {
        Value.TrimStartAndEndInline();
        return Value;
    }
}

void UStartWorldWidget::NativeConstruct()
{
    Super::NativeConstruct();
    ClearFlags(RF_Transactional);
    BindDefaultButtons();
}

void UStartWorldWidget::NativeDestruct()
{
    UnbindDefaultButtons();
    if (IsValid(ProjectSelectionWidget))
    {
        ProjectSelectionWidget->SetOwnerStartWidget(nullptr);
        ProjectSelectionWidget = nullptr;
    }
    StartButton.Reset();
    WorldSelectionButton.Reset();
    BackButton.Reset();
    RefreshButton.Reset();
    MultiplayerButton.Reset();
    SettingsButton.Reset();
    ProjectsButton.Reset();
    HostButton.Reset();
    ClientButton.Reset();
    JoinButton.Reset();
    MainGameMode.Reset();
    Super::NativeDestruct();
}

void UStartWorldWidget::SetMainGameMode(AMainGameMode* InMainGameMode)
{
    MainGameMode = InMainGameMode;
}

void UStartWorldWidget::ExecuteStartGame()
{
    if (AMainGameMode* Owner = MainGameMode.Get())
    {
        Owner->StartGame();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot start the game because MainGameMode is not assigned."));
}

void UStartWorldWidget::ExecuteReturnToMainMenuFromWorldSelection()
{
    if (AMainGameMode* Owner = MainGameMode.Get())
    {
        Owner->ReturnToMainMenuFromWorldSelection();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot return to the main menu because MainGameMode is not assigned."));
}

void UStartWorldWidget::ExecuteShowStartMenu()
{
    if (AMainGameMode* Owner = MainGameMode.Get())
    {
        Owner->ShowStartMenu();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot show the start menu because MainGameMode is not assigned."));
}

void UStartWorldWidget::ExecuteShowWorldSelectionMenu()
{
    if (AMainGameMode* Owner = MainGameMode.Get())
    {
        Owner->ShowWorldSelectionMenu();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot show the world-selection menu because MainGameMode is not assigned."));
}

void UStartWorldWidget::ExecuteShowMultiplayerMenu()
{
    if (AMainGameMode* Owner = MainGameMode.Get())
    {
        Owner->ShowMultiplayerMenu();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot show the multiplayer menu because MainGameMode is not assigned."));
}

void UStartWorldWidget::ExecuteShowSettingsMenu()
{
    if (AMainGameMode* Owner = MainGameMode.Get())
    {
        Owner->ShowSettingsMenu();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot show settings because MainGameMode is not assigned."));
}

void UStartWorldWidget::ExecuteShowProjectSelectionWidget()
{
    if (!IsValid(ProjectSelectionWidget))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Projects button requires ProjectSelectionWidgetClass in the central AssetRegistry or an explicit SetProjectSelectionWidget() override."));
        return;
    }

    VisibilityBeforeProjectSelection = GetVisibility();
    SetVisibility(ESlateVisibility::Collapsed);
    ProjectSelectionWidget->SetOwnerStartWidget(this);
    ProjectSelectionWidget->SetVisibility(ESlateVisibility::Visible);
    ProjectSelectionWidget->RefreshProjects();
    ApplyProjectSelectionInputMode(ProjectSelectionWidget.Get());
}

void UStartWorldWidget::CloseProjectSelectionWidget()
{
    if (IsValid(ProjectSelectionWidget))
    {
        ProjectSelectionWidget->SetVisibility(ESlateVisibility::Collapsed);
    }

    if (IsInViewport())
    {
        SetVisibility(VisibilityBeforeProjectSelection);
        ApplyProjectSelectionInputMode(this);
    }
}

void UStartWorldWidget::ToggleProjectSelectionWidget()
{
    if (IsValid(ProjectSelectionWidget)
        && ProjectSelectionWidget->GetVisibility() != ESlateVisibility::Collapsed
        && ProjectSelectionWidget->GetVisibility() != ESlateVisibility::Hidden)
    {
        CloseProjectSelectionWidget();
    }
    else
    {
        ExecuteShowProjectSelectionWidget();
    }
}

void UStartWorldWidget::SetProjectSelectionWidget(UProjectSelectionWidget* InWidget)
{
    if (ProjectSelectionWidget == InWidget)
    {
        return;
    }

    if (IsValid(ProjectSelectionWidget))
    {
        ProjectSelectionWidget->SetOwnerStartWidget(nullptr);
    }

    ProjectSelectionWidget = InWidget;
    if (IsValid(ProjectSelectionWidget))
    {
        ProjectSelectionWidget->SetOwnerStartWidget(this);
        ProjectSelectionWidget->SetVisibility(ESlateVisibility::Collapsed);
    }
}

void UStartWorldWidget::HandleProjectSelectionWidgetRemoved(UProjectSelectionWidget* RemovedWidget)
{
    if (ProjectSelectionWidget.Get() != RemovedWidget)
    {
        return;
    }

    ProjectSelectionWidget = nullptr;
    if (IsInViewport())
    {
        SetVisibility(VisibilityBeforeProjectSelection);
        ApplyProjectSelectionInputMode(this);
    }
}

void UStartWorldWidget::ApplyProjectSelectionInputMode(UUserWidget* FocusWidget) const
{
    APlayerController* PlayerController = GetOwningPlayer();
    if (!IsValid(PlayerController))
    {
        return;
    }

    FInputModeUIOnly InputMode;
    // UIOnly logs an engine error if the requested Slate widget cannot accept keyboard focus.
    // Mouse-only menu/loading roots do not need explicit focus.
    if (IsValid(FocusWidget) && FocusWidget->IsFocusable())
    {
        InputMode.SetWidgetToFocus(FocusWidget->TakeWidget());
    }
    InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
    PlayerController->SetInputMode(InputMode);
    PlayerController->bShowMouseCursor = true;
}

void UStartWorldWidget::ExecuteRefreshWorldSelectionData()
{
    if (AMainGameMode* Owner = MainGameMode.Get())
    {
        Owner->RefreshWorldFolderNameMap();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot refresh world-selection data because MainGameMode is not assigned."));
}

void UStartWorldWidget::BindDefaultButtons()
{
    if (UButton* Button = StartButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteStartGame);
        Button->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteStartGame);
    }

    if (UButton* Button = WorldSelectionButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowWorldSelectionMenu);
        if (Button != StartButton.Get())
        {
            Button->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteShowWorldSelectionMenu);
        }
    }

    if (UButton* Button = BackButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteReturnToMainMenuFromWorldSelection);
        Button->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteReturnToMainMenuFromWorldSelection);
    }

    if (UButton* Button = RefreshButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteRefreshWorldSelectionData);
        Button->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteRefreshWorldSelectionData);
    }

    if (UButton* Button = MultiplayerButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowMultiplayerMenu);
        Button->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteShowMultiplayerMenu);
    }

    if (UButton* Button = SettingsButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowSettingsMenu);
        Button->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteShowSettingsMenu);
    }

    if (UButton* Button = ProjectsButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowProjectSelectionWidget);
        Button->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteShowProjectSelectionWidget);
    }

    if (UButton* Button = HostButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteHostSelectedWorld);
        Button->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteHostSelectedWorld);
    }

    if (UButton* Button = ClientButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteOpenClientConnectionWorld);
        Button->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteOpenClientConnectionWorld);
    }

    if (UButton* Button = JoinButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteJoinSelectedWorld);
        Button->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteJoinSelectedWorld);
    }
}

void UStartWorldWidget::UnbindDefaultButtons()
{
    if (UButton* Button = StartButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteStartGame);
    }

    if (UButton* Button = WorldSelectionButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowWorldSelectionMenu);
    }

    if (UButton* Button = BackButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteReturnToMainMenuFromWorldSelection);
    }

    if (UButton* Button = RefreshButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteRefreshWorldSelectionData);
    }

    if (UButton* Button = MultiplayerButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowMultiplayerMenu);
    }

    if (UButton* Button = SettingsButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowSettingsMenu);
    }

    if (UButton* Button = ProjectsButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowProjectSelectionWidget);
    }

    if (UButton* Button = HostButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteHostSelectedWorld);
    }

    if (UButton* Button = ClientButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteOpenClientConnectionWorld);
    }

    if (UButton* Button = JoinButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteJoinSelectedWorld);
    }
}

void UStartWorldWidget::SetStartButton(UButton* InButton)
{
    if (UButton* Button = StartButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteStartGame);
    }

    StartButton = InButton;

    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowWorldSelectionMenu);
        InButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteStartGame);
        InButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteStartGame);
    }
}

void UStartWorldWidget::SetWorldSelectionButton(UButton* InButton)
{
    if (UButton* Button = WorldSelectionButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowWorldSelectionMenu);
    }

    WorldSelectionButton = InButton;

    if (IsValid(InButton) && InButton != StartButton.Get())
    {
        InButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowWorldSelectionMenu);
        InButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteShowWorldSelectionMenu);
    }
}

void UStartWorldWidget::SetBackButton(UButton* InButton)
{
    if (UButton* Button = BackButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteReturnToMainMenuFromWorldSelection);
    }

    BackButton = InButton;

    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteReturnToMainMenuFromWorldSelection);
        InButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteReturnToMainMenuFromWorldSelection);
    }
}

void UStartWorldWidget::SetRefreshButton(UButton* InButton)
{
    if (UButton* Button = RefreshButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteRefreshWorldSelectionData);
    }

    RefreshButton = InButton;

    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteRefreshWorldSelectionData);
        InButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteRefreshWorldSelectionData);
    }
}

void UStartWorldWidget::SetMultiplayerButton(UButton* InButton)
{
    if (UButton* Button = MultiplayerButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowMultiplayerMenu);
    }

    MultiplayerButton = InButton;

    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowMultiplayerMenu);
        InButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteShowMultiplayerMenu);
    }
}

void UStartWorldWidget::SetSettingsButton(UButton* InButton)
{
    if (UButton* Button = SettingsButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowSettingsMenu);
    }
    SettingsButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowSettingsMenu);
        InButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteShowSettingsMenu);
    }
}

void UStartWorldWidget::SetProjectsButton(UButton* InButton)
{
    if (UButton* Button = ProjectsButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowProjectSelectionWidget);
    }
    ProjectsButton = InButton;
    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowProjectSelectionWidget);
        InButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteShowProjectSelectionWidget);
    }
}

void UStartWorldWidget::SetHostButton(UButton* InButton)
{
    if (UButton* Button = HostButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteHostSelectedWorld);
    }

    HostButton = InButton;

    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteHostSelectedWorld);
        InButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteHostSelectedWorld);
    }
}

void UStartWorldWidget::SetClientButton(UButton* InButton)
{
    if (UButton* Button = ClientButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteOpenClientConnectionWorld);
    }

    ClientButton = InButton;

    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteOpenClientConnectionWorld);
        InButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteOpenClientConnectionWorld);
    }
}

void UStartWorldWidget::SetJoinButton(UButton* InButton)
{
    if (UButton* Button = JoinButton.Get())
    {
        Button->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteJoinSelectedWorld);
    }

    JoinButton = InButton;

    if (IsValid(InButton))
    {
        InButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteJoinSelectedWorld);
        InButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteJoinSelectedWorld);
    }
}

void UStartWorldWidget::SetSelectedWorldFolderName(const FString& InWorldFolderName)
{
    FString ResolvedFolderName;
    if (ResolveWorldFolderName(InWorldFolderName, ResolvedFolderName))
    {
        SelectedWorldFolderName = ResolvedFolderName;
        return;
    }

    SelectedWorldFolderName = NormalizeStartWorldText(InWorldFolderName);
}

bool UStartWorldWidget::ResolveWorldFolderName(const FString& FolderOrDisplayName, FString& OutWorldFolderName) const
{
    const FString NormalizedInput = NormalizeStartWorldText(FolderOrDisplayName);
    if (NormalizedInput.IsEmpty())
    {
        OutWorldFolderName.Reset();
        return false;
    }

    const TMap<FString, FString> FolderMap = GetFolderNameMap();
    if (FolderMap.Contains(NormalizedInput))
    {
        OutWorldFolderName = NormalizedInput;
        return true;
    }

    for (const TPair<FString, FString>& Pair : FolderMap)
    {
        if (Pair.Key.Equals(NormalizedInput, ESearchCase::IgnoreCase)
            || NormalizeStartWorldText(Pair.Value).Equals(NormalizedInput, ESearchCase::IgnoreCase))
        {
            OutWorldFolderName = Pair.Key;
            return true;
        }
    }

    OutWorldFolderName.Reset();
    return false;
}

FString UStartWorldWidget::GetSelectedWorldRootPath() const
{
    FString Folder;
    if (!UGameManagerSubSystem::TryNormalizeWorldFolderName(SelectedWorldFolderName, Folder, true))
        return FString();
    return FPaths::ConvertRelativePathToFull(FPaths::Combine(PATH_ROOT, Folder));
}

bool UStartWorldWidget::OpenSelectedWorld()
{
    return OpenWorldByFolderName(SelectedWorldFolderName);
}

bool UStartWorldWidget::OpenWorldByFolderName(const FString& WorldFolderName)
{
    FString ResolvedFolderName;
    if (!ResolveWorldFolderName(WorldFolderName, ResolvedFolderName))
    {
        UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot open unknown world: %s"), *WorldFolderName);
        return false;
    }

    SelectedWorldFolderName = ResolvedFolderName;
    if (AMainGameMode* Owner = MainGameMode.Get())
    {
        Owner->OpenSinglePlayerWorldByFolderName(ResolvedFolderName);
        return true;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot open world %s because MainGameMode is not assigned."), *ResolvedFolderName);
    return false;
}

void UStartWorldWidget::ExecuteHostSelectedWorld()
{
    HostSelectedWorld();
}

bool UStartWorldWidget::HostSelectedWorld()
{
    return HostWorldByFolderName(SelectedWorldFolderName);
}

bool UStartWorldWidget::HostWorldByFolderName(const FString& WorldFolderName)
{
    FString ResolvedFolderName;
    if (!ResolveWorldFolderName(WorldFolderName, ResolvedFolderName))
    {
        UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot host unknown world: %s"), *WorldFolderName);
        return false;
    }

    SelectedWorldFolderName = ResolvedFolderName;
    if (AMainGameMode* Owner = MainGameMode.Get())
    {
        Owner->HostMultiplayerWorldByFolderName(ResolvedFolderName);
        return true;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot host world because MainGameMode is not assigned."));
    return false;
}

void UStartWorldWidget::ExecuteJoinSelectedWorld()
{
    JoinSelectedWorld();
}

bool UStartWorldWidget::JoinSelectedWorld()
{
    if (AMainGameMode* Owner = MainGameMode.Get())
    {
        Owner->JoinMultiplayerServer(ServerAddress, SelectedWorldFolderName);
        return true;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot join multiplayer because MainGameMode is not assigned."));
    return false;
}

bool UStartWorldWidget::JoinServer(const FString& InServerAddress)
{
    SetServerAddress(InServerAddress);
    if (AMainGameMode* Owner = MainGameMode.Get())
    {
        Owner->JoinMultiplayerServer(ServerAddress, SelectedWorldFolderName);
        return true;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot join server because MainGameMode is not assigned."));
    return false;
}

void UStartWorldWidget::ExecuteOpenClientConnectionWorld()
{
    OpenClientConnectionWorld();
}

bool UStartWorldWidget::OpenClientConnectionWorld()
{
    if (AMainGameMode* Owner = MainGameMode.Get())
    {
        Owner->OpenClientConnectionWorld(ServerAddress);
        return true;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot open ClientWorld because MainGameMode is not assigned."));
    return false;
}

void UStartWorldWidget::SetServerAddress(const FString& InServerAddress)
{
    ServerAddress = NormalizeStartWorldText(InServerAddress);
    if (ServerAddress.IsEmpty())
    {
        ServerAddress = TEXT("127.0.0.1:7777");
    }

    if (AMainGameMode* Owner = MainGameMode.Get())
    {
        Owner->SetPendingServerAddress(ServerAddress);
    }
}

TMap<FString, FString> UStartWorldWidget::GetFolderNameMap() const
{
    if (const AMainGameMode* Owner = MainGameMode.Get())
    {
        return Owner->GetFolderNameMap();
    }

    return CachedWorldSelectionData;
}

void UStartWorldWidget::SetWorldSelectionData(const TMap<FString, FString>& Values)
{
    CachedWorldSelectionData = Values;
}

