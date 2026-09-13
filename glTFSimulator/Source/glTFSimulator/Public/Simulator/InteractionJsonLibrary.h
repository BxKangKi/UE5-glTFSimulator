/**
 * @file InteractionJsonLibrary.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Simulator/InteractionTypes.h"
#include "InteractionJsonLibrary.generated.h"

UCLASS()
class GLTFSIMULATOR_API USimulatorInteractionJsonLibrary final : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category="glTF|Interaction JSON")
    static bool ParseCharacterInteractionJson(const FString& Json, FSimulatorCharacterInteractionConfig& OutConfig, FString& OutError);

    UFUNCTION(BlueprintCallable, Category="glTF|Interaction JSON")
    static bool ParseEquipmentInteractionJson(const FString& Json, FSimulatorEquipmentInteractionConfig& OutConfig, FString& OutError);

    UFUNCTION(BlueprintCallable, Category="glTF|Interaction JSON")
    static bool CharacterInteractionToJson(const FSimulatorCharacterInteractionConfig& Config, FString& OutJson);

    UFUNCTION(BlueprintCallable, Category="glTF|Interaction JSON")
    static bool EquipmentInteractionToJson(const FSimulatorEquipmentInteractionConfig& Config, FString& OutJson);
};
