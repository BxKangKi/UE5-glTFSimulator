/**
 * @file InteractionBlueprintLibrary.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Simulator/InteractionTypes.h"
#include "InteractionBlueprintLibrary.generated.h"

class ACharacter;
class AStaticActor;
class ASimulatorHeldStaticPreviewActor;

UCLASS()
class GLTFSIMULATOR_API USimulatorInteractionBlueprintLibrary final : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    /** Attaches equipment directly to the resolved character hand socket. */
    UFUNCTION(BlueprintCallable, Category="Interaction|Equipment")
    static bool EquipActor(ACharacter* Character, AActor* Equipment, const FSimulatorCharacterInteractionConfig& CharacterConfig,
        const FSimulatorEquipmentInteractionConfig& EquipmentConfig, FString& OutError);

    UFUNCTION(BlueprintCallable, Category="Interaction|Equipment")
    static void UnequipActor(ACharacter* Character, AActor* Equipment, bool bDestroyEquipment);

    UFUNCTION(BlueprintCallable, Category="Interaction|Static", meta=(WorldContext="WorldContextObject"))
    static ASimulatorHeldStaticPreviewActor* HoldStatic(UObject* WorldContextObject, ACharacter* Character, TSubclassOf<AStaticActor> StaticClass,
        const FString& CanonicalModelReference, const FSimulatorCharacterInteractionConfig& CharacterConfig,
        const FSimulatorEquipmentInteractionConfig& EquipmentConfig, FString& OutError);
};
