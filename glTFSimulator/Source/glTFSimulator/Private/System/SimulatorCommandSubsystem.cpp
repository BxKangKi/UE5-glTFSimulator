// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "System/SimulatorCommandSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/Parse.h"
#include "System/GameManagerSubSystem.h"
#include "Weather/WeatherSubsystem.h"
#include "World/WorldData.h"

namespace
{
    bool IsCommandName(const FString& Value, const TCHAR* Expected)
    {
        return Value.Equals(Expected, ESearchCase::IgnoreCase);
    }
}

bool USimulatorCommandSubsystem::ExecuteCommand(
    APlayerController* RequestingController,
    const FString& CommandLine,
    FString& OutMessage)
{
    OutMessage.Reset();

    if (!IsValid(RequestingController) || RequestingController->GetGameInstance() != GetGameInstance())
    {
        OutMessage = TEXT("Command rejected: invalid player controller.");
        return false;
    }

    TArray<FString> Tokens;
    Tokenize(CommandLine, Tokens);
    if (Tokens.IsEmpty())
    {
        return false;
    }

    const FString Command = Tokens[0];
    Tokens.RemoveAt(0, 1, EAllowShrinking::No);

    if (IsCommandName(Command, TEXT("weather")))
    {
        ExecuteWeather(RequestingController, Tokens, OutMessage);
        return true;
    }
    if (IsCommandName(Command, TEXT("time")))
    {
        ExecuteTime(RequestingController, Tokens, OutMessage);
        return true;
    }
    if (IsCommandName(Command, TEXT("tp")))
    {
        ExecuteTeleport(RequestingController, Tokens, OutMessage);
        return true;
    }

    return false;
}

bool USimulatorCommandSubsystem::IsSimulatorCommand(const TCHAR* CommandLine)
{
    if (!CommandLine)
    {
        return false;
    }

    const TCHAR* Cursor = CommandLine;
    const FString Name = FParse::Token(Cursor, false);
    return IsCommandName(Name, TEXT("weather")) ||
        IsCommandName(Name, TEXT("time")) ||
        IsCommandName(Name, TEXT("tp"));
}

bool USimulatorCommandSubsystem::ExecuteWeather(
    APlayerController* Controller,
    const TArray<FString>& Args,
    FString& OutMessage)
{
    if (Args.Num() < 1 || Args.Num() > 3)
    {
        OutMessage = TEXT("Usage: weather <clear|rain|snow> [timer <seconds>|timer=<seconds>|seconds]");
        return false;
    }

    const FString Preset = Args[0].ToLower();
    if (Preset != TEXT("clear") && Preset != TEXT("rain") && Preset != TEXT("snow"))
    {
        OutMessage = TEXT("Weather must be clear, rain, or snow.");
        return false;
    }

    // Keep the short positional form for convenience, while also accepting the explicit
    // `timer` form requested by level/chat command UX. Both reach the same subsystem API.
    double Duration = -1.0;
    if (Args.Num() >= 2)
    {
        FString DurationToken;
        if (Args[1].Equals(TEXT("timer"), ESearchCase::IgnoreCase))
        {
            if (Args.Num() != 3)
            {
                OutMessage = TEXT("Usage: weather <clear|rain|snow> timer <seconds>");
                return false;
            }
            DurationToken = Args[2];
        }
        else if (Args[1].StartsWith(TEXT("timer="), ESearchCase::IgnoreCase))
        {
            if (Args.Num() != 2)
            {
                OutMessage = TEXT("Use either timer=<seconds> or timer <seconds>, not both.");
                return false;
            }
            DurationToken = Args[1].RightChop(6);
        }
        else
        {
            if (Args.Num() != 2)
            {
                OutMessage = TEXT("Usage: weather <clear|rain|snow> [timer <seconds>|timer=<seconds>|seconds]");
                return false;
            }
            DurationToken = Args[1];
        }

        if (!ParseFiniteDouble(DurationToken, Duration) || Duration < 0.0)
        {
            OutMessage = TEXT("Weather duration must be a finite number >= 0 seconds.");
            return false;
        }
    }

    UGameInstance* GameInstance = Controller->GetGameInstance();
    UWeatherSubsystem* Weather = IsValid(GameInstance)
        ? GameInstance->GetSubsystem<UWeatherSubsystem>()
        : nullptr;
    if (!IsValid(Weather))
    {
        OutMessage = TEXT("Weather subsystem is unavailable.");
        return false;
    }

    // clear is still an enabled simulated state: the effect actor is unloaded immediately and the
    // optional duration controls when auto-cycle chooses the next state.
    if (!Weather->ApplyWeather(Preset, 1.0f, true, static_cast<float>(Duration)))
    {
        OutMessage = TEXT("Weather command failed.");
        return false;
    }

    OutMessage = Duration >= 0.0
        ? FString::Printf(TEXT("Weather set to %s for %.2f seconds."), *Preset, Duration)
        : FString::Printf(TEXT("Weather set to %s."), *Preset);
    return true;
}

bool USimulatorCommandSubsystem::ExecuteTime(
    APlayerController* Controller,
    const TArray<FString>& Args,
    FString& OutMessage)
{
    if (Args.Num() != 2)
    {
        OutMessage = TEXT("Usage: time <dt|sec|day> <value>");
        return false;
    }

    double Value = 0.0;
    if (!ParseFiniteDouble(Args[1], Value))
    {
        OutMessage = TEXT("Time value must be a finite number.");
        return false;
    }

    UGameManagerSubSystem* GameManager = UGameManagerSubSystem::GetSubSystem(Controller);
    if (!IsValid(GameManager) || !IsValid(GameManager->GetWorldData()))
    {
        OutMessage = TEXT("World time is unavailable.");
        return false;
    }

    const FString Mode = Args[0].ToLower();
    bool bSuccess = false;
    if (Mode == TEXT("dt"))
    {
        bSuccess = GameManager->AddWorldTimeSeconds(Value);
    }
    else if (Mode == TEXT("sec"))
    {
        bSuccess = GameManager->SetWorldTimeSeconds(Value);
    }
    else if (Mode == TEXT("day"))
    {
        bSuccess = GameManager->SetWorldDay(Value);
    }
    else
    {
        OutMessage = TEXT("Time mode must be dt, sec, or day.");
        return false;
    }

    if (!bSuccess)
    {
        OutMessage = TEXT("Time command was rejected by the active world.");
        return false;
    }

    OutMessage = FString::Printf(TEXT("Time %s applied: %.3f"), *Mode, Value);
    return true;
}

bool USimulatorCommandSubsystem::ExecuteTeleport(
    APlayerController* Controller,
    const TArray<FString>& Args,
    FString& OutMessage)
{
    if (Args.Num() < 3)
    {
        OutMessage = TEXT("Usage: tp <x> <y> <z> [playerName]");
        return false;
    }

    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;
    if (!ParseFiniteDouble(Args[0], X) || !ParseFiniteDouble(Args[1], Y) || !ParseFiniteDouble(Args[2], Z))
    {
        OutMessage = TEXT("Teleport coordinates must be finite numbers.");
        return false;
    }

    constexpr double MaxCoordinateMagnitude = 1.0e9;
    if (FMath::Abs(X) > MaxCoordinateMagnitude || FMath::Abs(Y) > MaxCoordinateMagnitude || FMath::Abs(Z) > MaxCoordinateMagnitude)
    {
        OutMessage = TEXT("Teleport coordinate exceeds the simulator safety limit.");
        return false;
    }

    FString PlayerName;
    if (Args.Num() >= 4)
    {
        // TArray does not expose FString-style Mid(). Build the optional player name
        // explicitly so names containing spaces remain supported on every UE5 version.
        PlayerName = Args[3];
        for (int32 Index = 4; Index < Args.Num(); ++Index)
        {
            PlayerName += TEXT(" ");
            PlayerName += Args[Index];
        }
    }

    APlayerController* TargetController = ResolveTargetPlayer(Controller, PlayerName);
    APawn* TargetPawn = IsValid(TargetController) ? TargetController->GetPawn() : nullptr;
    if (!IsValid(TargetPawn))
    {
        OutMessage = TEXT("Target player/pawn was not found.");
        return false;
    }

    const FVector Destination(X, Y, Z);
    const bool bMoved = TargetPawn->TeleportTo(
        Destination,
        TargetPawn->GetActorRotation(),
        false,
        true);
    if (!bMoved)
    {
        OutMessage = TEXT("Teleport failed because the destination was rejected.");
        return false;
    }

    const FString TargetName = IsValid(TargetController->PlayerState)
        ? TargetController->PlayerState->GetPlayerName()
        : TargetController->GetName();
    OutMessage = FString::Printf(TEXT("Teleported %s to %.3f %.3f %.3f"), *TargetName, X, Y, Z);
    return true;
}

void USimulatorCommandSubsystem::Tokenize(const FString& Input, TArray<FString>& OutTokens)
{
    OutTokens.Reset();

    const TCHAR* Cursor = *Input;
    while (Cursor && *Cursor)
    {
        FString Token = FParse::Token(Cursor, false);
        if (Token.IsEmpty())
        {
            break;
        }
        OutTokens.Add(MoveTemp(Token));
    }
}

bool USimulatorCommandSubsystem::ParseFiniteDouble(const FString& Token, double& OutValue)
{
    OutValue = 0.0;
    return FDefaultValueHelper::ParseDouble(Token, OutValue) && FMath::IsFinite(OutValue);
}

APlayerController* USimulatorCommandSubsystem::ResolveTargetPlayer(
    APlayerController* RequestingController,
    const FString& PlayerName)
{
    if (!IsValid(RequestingController) || PlayerName.IsEmpty())
    {
        return RequestingController;
    }

    UWorld* World = RequestingController->GetWorld();
    if (!World)
    {
        return nullptr;
    }

    for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
    {
        APlayerController* Candidate = It->Get();
        if (!IsValid(Candidate))
        {
            continue;
        }

        const FString CandidateName = IsValid(Candidate->PlayerState)
            ? Candidate->PlayerState->GetPlayerName()
            : Candidate->GetName();
        if (CandidateName.Equals(PlayerName, ESearchCase::IgnoreCase))
        {
            return Candidate;
        }
    }

    return nullptr;
}
