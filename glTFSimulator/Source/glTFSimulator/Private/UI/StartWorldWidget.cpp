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
    StartActor.Reset();
    Super::NativeDestruct();
}

void UStartWorldWidget::SetStartActor(AStartActor* InStartActor)
{
    StartActor = InStartActor;
    OnStartActorAssigned(InStartActor);
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
    if (StartButton)
    {
        StartButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteStartGame);
        StartButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteStartGame);
    }

    if (WorldSelectionButton && WorldSelectionButton != StartButton)
    {
        WorldSelectionButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowWorldSelectionMenu);
        WorldSelectionButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteShowWorldSelectionMenu);
    }

    if (BackButton)
    {
        BackButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteReturnToMainMenuFromWorldSelection);
        BackButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteReturnToMainMenuFromWorldSelection);
    }

    if (RefreshButton)
    {
        RefreshButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteRefreshWorldSelectionData);
        RefreshButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteRefreshWorldSelectionData);
    }

    if (MultiplayerButton)
    {
        MultiplayerButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowMultiplayerMenu);
        MultiplayerButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteShowMultiplayerMenu);
    }

    if (HostButton)
    {
        HostButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteHostSelectedWorld);
        HostButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteHostSelectedWorld);
    }

    if (ClientButton)
    {
        ClientButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteOpenClientConnectionWorld);
        ClientButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteOpenClientConnectionWorld);
    }

    if (JoinButton)
    {
        JoinButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteJoinSelectedWorld);
        JoinButton->OnClicked.AddDynamic(this, &UStartWorldWidget::ExecuteJoinSelectedWorld);
    }
}

void UStartWorldWidget::UnbindDefaultButtons()
{
    if (StartButton)
    {
        StartButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteStartGame);
    }
    if (WorldSelectionButton)
    {
        WorldSelectionButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowWorldSelectionMenu);
    }
    if (BackButton)
    {
        BackButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteReturnToMainMenuFromWorldSelection);
    }
    if (RefreshButton)
    {
        RefreshButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteRefreshWorldSelectionData);
    }
    if (MultiplayerButton)
    {
        MultiplayerButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteShowMultiplayerMenu);
    }
    if (HostButton)
    {
        HostButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteHostSelectedWorld);
    }
    if (ClientButton)
    {
        ClientButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteOpenClientConnectionWorld);
    }
    if (JoinButton)
    {
        JoinButton->OnClicked.RemoveDynamic(this, &UStartWorldWidget::ExecuteJoinSelectedWorld);
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
    OnWorldSelectionDataUpdated(CachedWorldSelectionData);
}

