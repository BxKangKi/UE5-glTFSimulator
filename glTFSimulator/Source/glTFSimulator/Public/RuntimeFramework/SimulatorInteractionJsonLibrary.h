#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "RuntimeFramework/SimulatorInteractionTypes.h"
#include "SimulatorInteractionJsonLibrary.generated.h"

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
