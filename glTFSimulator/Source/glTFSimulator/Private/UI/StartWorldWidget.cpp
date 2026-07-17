// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "UI/StartWorldWidget.h"

#include "Components/Button.h"
#include "World/StartActor.h"

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
    StartButton.Reset();
    WorldSelectionButton.Reset();
    BackButton.Reset();
    RefreshButton.Reset();
    MultiplayerButton.Reset();
    HostButton.Reset();
    ClientButton.Reset();
    JoinButton.Reset();
    StartActor.Reset();
    Super::NativeDestruct();
}

void UStartWorldWidget::SetStartActor(AStartActor* InStartActor)
{
    StartActor = InStartActor;
}

void UStartWorldWidget::ExecuteStartGame()
{
    if (AStartActor* Owner = StartActor.Get())
    {
        Owner->StartGame();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot start the game because StartActor is not assigned."));
}

void UStartWorldWidget::ExecuteReturnToMainMenuFromWorldSelection()
{
    if (AStartActor* Owner = StartActor.Get())
    {
        Owner->ReturnToMainMenuFromWorldSelection();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot return to the main menu because StartActor is not assigned."));
}

void UStartWorldWidget::ExecuteShowStartMenu()
{
    if (AStartActor* Owner = StartActor.Get())
    {
        Owner->ShowStartMenu();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot show the start menu because StartActor is not assigned."));
}

void UStartWorldWidget::ExecuteShowWorldSelectionMenu()
{
    if (AStartActor* Owner = StartActor.Get())
    {
        Owner->ShowWorldSelectionMenu();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot show the world-selection menu because StartActor is not assigned."));
}

void UStartWorldWidget::ExecuteShowMultiplayerMenu()
{
    if (AStartActor* Owner = StartActor.Get())
    {
        Owner->ShowMultiplayerMenu();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot show the multiplayer menu because StartActor is not assigned."));
}

void UStartWorldWidget::ExecuteRefreshWorldSelectionData()
{
    if (AStartActor* Owner = StartActor.Get())
    {
        Owner->RefreshWorldFolderNameMap();
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot refresh world-selection data because StartActor is not assigned."));
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
    if (AStartActor* Owner = StartActor.Get())
    {
        Owner->OpenSinglePlayerWorldByFolderName(ResolvedFolderName);
        return true;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot open world %s because StartActor is not assigned."), *ResolvedFolderName);
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
    if (AStartActor* Owner = StartActor.Get())
    {
        Owner->HostMultiplayerWorldByFolderName(ResolvedFolderName);
        return true;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot host world because StartActor is not assigned."));
    return false;
}

void UStartWorldWidget::ExecuteJoinSelectedWorld()
{
    JoinSelectedWorld();
}

bool UStartWorldWidget::JoinSelectedWorld()
{
    if (AStartActor* Owner = StartActor.Get())
    {
        Owner->JoinMultiplayerServer(ServerAddress, SelectedWorldFolderName);
        return true;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot join multiplayer because StartActor is not assigned."));
    return false;
}

bool UStartWorldWidget::JoinServer(const FString& InServerAddress)
{
    SetServerAddress(InServerAddress);
    if (AStartActor* Owner = StartActor.Get())
    {
        Owner->JoinMultiplayerServer(ServerAddress, SelectedWorldFolderName);
        return true;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot join server because StartActor is not assigned."));
    return false;
}

void UStartWorldWidget::ExecuteOpenClientConnectionWorld()
{
    OpenClientConnectionWorld();
}

bool UStartWorldWidget::OpenClientConnectionWorld()
{
    if (AStartActor* Owner = StartActor.Get())
    {
        Owner->OpenClientConnectionWorld(ServerAddress);
        return true;
    }

    UE_LOG(LogTemp, Warning, TEXT("StartWorldWidget cannot open ClientWorld because StartActor is not assigned."));
    return false;
}

void UStartWorldWidget::SetServerAddress(const FString& InServerAddress)
{
    ServerAddress = NormalizeStartWorldText(InServerAddress);
    if (ServerAddress.IsEmpty())
    {
        ServerAddress = TEXT("127.0.0.1:7777");
    }

    if (AStartActor* Owner = StartActor.Get())
    {
        Owner->SetPendingServerAddress(ServerAddress);
    }
}

TMap<FString, FString> UStartWorldWidget::GetFolderNameMap() const
{
    if (const AStartActor* Owner = StartActor.Get())
    {
        return Owner->GetFolderNameMap();
    }

    return CachedWorldSelectionData;
}

void UStartWorldWidget::SetWorldSelectionData(const TMap<FString, FString>& Values)
{
    CachedWorldSelectionData = Values;
}

