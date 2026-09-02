#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "RuntimeFramework/SimulatorInteractionTypes.h"
#include "SimulatorInteractionAnimInstance.generated.h"

/**
 * Game-thread resolver for hand and body IK targets. The Animation Blueprint reads
 * the copied component-space transforms and scalar values; it never dereferences
 * an equipment actor from a worker-thread animation node.
 */
UCLASS(Transient, Blueprintable)
class GLTFSIMULATOR_API USimulatorInteractionAnimInstance : public UAnimInstance
{
    GENERATED_BODY()

public:
    virtual void NativeInitializeAnimation() override;
    virtual void NativeUpdateAnimation(float DeltaSeconds) override;

    UFUNCTION(BlueprintCallable, Category="Interaction|IK")
    void SetCharacterInteractionConfig(const FSimulatorCharacterInteractionConfig& InConfig);

    UFUNCTION(BlueprintCallable, Category="Interaction|IK")
    void EquipInteractionActor(AActor* InEquipmentActor, const FSimulatorEquipmentInteractionConfig& InConfig);

    UFUNCTION(BlueprintCallable, Category="Interaction|IK")
    void ClearInteractionActor();

    UFUNCTION(BlueprintPure, Category="Interaction|IK")
    ESimulatorHand GetResolvedPrimaryHand() const { return ResolvedPrimaryHand; }

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Interaction|IK")
    FSimulatorResolvedFullBodyIK ResolvedIK;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Interaction|IK")
    ESimulatorHand ResolvedPrimaryHand = ESimulatorHand::Right;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Interaction|IK")
    FSimulatorFullBodyIKConfig ActiveIKConfig;

private:
    TWeakObjectPtr<AActor> OwningActorWeak;
    TWeakObjectPtr<AActor> EquipmentActorWeak;
    FSimulatorCharacterInteractionConfig CharacterConfig;
    FSimulatorEquipmentInteractionConfig EquipmentConfig;
    FRotator PreviousOwnerRotation = FRotator::ZeroRotator;
    bool bHasEquipmentConfig = false;

    FSimulatorResolvedHandIK ResolveHand(ESimulatorHand Hand, float DeltaSeconds, const FSimulatorResolvedHandIK& Previous) const;
    USceneComponent* FindEquipmentAnchor(const FSimulatorGripPoint& Grip) const;
    void UpdateBodyIK(float DeltaSeconds);
    void ResetResolvedState(float DeltaSeconds);
    static FTransform InterpTransform(const FTransform& Current, const FTransform& Target, float DeltaSeconds, float Speed);
};
