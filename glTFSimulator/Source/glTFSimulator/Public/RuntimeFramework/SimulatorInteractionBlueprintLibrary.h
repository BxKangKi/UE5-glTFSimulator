#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "RuntimeFramework/SimulatorInteractionTypes.h"
#include "SimulatorInteractionBlueprintLibrary.generated.h"

class ACharacter;
class ASimulatorHeldPrefabPreviewActor;

UCLASS()
class GLTFSIMULATOR_API USimulatorInteractionBlueprintLibrary final : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    /** Attaches equipment to the resolved hand and updates SimulatorInteractionAnimInstance. */
    UFUNCTION(BlueprintCallable, Category="Interaction|Equipment")
    static bool EquipActor(ACharacter* Character, AActor* Equipment, const FSimulatorCharacterInteractionConfig& CharacterConfig,
        const FSimulatorEquipmentInteractionConfig& EquipmentConfig, FString& OutError);

    UFUNCTION(BlueprintCallable, Category="Interaction|Equipment")
    static void UnequipActor(ACharacter* Character, AActor* Equipment, bool bDestroyEquipment);

    UFUNCTION(BlueprintCallable, Category="Interaction|Prefab", meta=(WorldContext="WorldContextObject"))
    static ASimulatorHeldPrefabPreviewActor* HoldPrefab(UObject* WorldContextObject, ACharacter* Character, TSubclassOf<AActor> PrefabClass,
        const FString& CanonicalSourcePath, const FSimulatorCharacterInteractionConfig& CharacterConfig,
        const FSimulatorEquipmentInteractionConfig& EquipmentConfig, FString& OutError);
};
