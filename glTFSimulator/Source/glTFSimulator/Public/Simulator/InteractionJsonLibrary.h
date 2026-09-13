/**
 * @file InteractionJsonLibrary.h
 * 역할: 상호작용 설정과 JSON을 변환합니다.
 * 핵심 기능: 상호작용 데이터 직렬화·역직렬화.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
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
