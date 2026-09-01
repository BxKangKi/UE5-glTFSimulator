// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SimulatorCommandSubsystem.generated.h"

class APlayerController;

/**
 * Single parser/executor for simulator commands.
 *
 * PlayerController console input is only one frontend. A future chat widget can pass exactly the
 * same command line to ExecuteCommand without duplicating parsing, permission, or validation code.
 */
UCLASS()
class GLTFSIMULATOR_API USimulatorCommandSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    /** Returns true when CommandLine begins with a simulator command, even if its arguments are invalid. */
    bool ExecuteCommand(APlayerController* RequestingController, const FString& CommandLine, FString& OutMessage);

    /** Lightweight recognizer used before deciding whether a client command should be sent to authority. */
    static bool IsSimulatorCommand(const TCHAR* CommandLine);

private:
    bool ExecuteWeather(APlayerController* Controller, const TArray<FString>& Args, FString& OutMessage);
    bool ExecuteTime(APlayerController* Controller, const TArray<FString>& Args, FString& OutMessage);
    bool ExecuteTeleport(APlayerController* Controller, const TArray<FString>& Args, FString& OutMessage);

    static void Tokenize(const FString& Input, TArray<FString>& OutTokens);
    static bool ParseFiniteDouble(const FString& Token, double& OutValue);
    static APlayerController* ResolveTargetPlayer(APlayerController* RequestingController, const FString& PlayerName);
};
