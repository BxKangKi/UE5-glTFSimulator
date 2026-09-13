/**
 * @file InteractionBlueprintLibrary.h
 * 역할: 상호작용 기능을 Blueprint에 노출합니다.
 * 핵심 기능: 장비 부착, 캐릭터 소켓 및 상호작용 보조.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
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
