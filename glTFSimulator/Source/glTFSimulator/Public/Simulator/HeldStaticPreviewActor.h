/**
 * @file HeldStaticPreviewActor.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Simulator/InteractionTypes.h"
#include "HeldStaticPreviewActor.generated.h"

class USceneComponent;
class ACharacter;
class AStaticActor;

/** Dedicated transient wrapper for an inventory Static miniature. */
UCLASS(BlueprintType)
class GLTFSIMULATOR_API ASimulatorHeldStaticPreviewActor : public AActor
{
    GENERATED_BODY()

public:
    ASimulatorHeldStaticPreviewActor();
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    UFUNCTION(BlueprintCallable, Category="Static|Held Preview", meta=(WorldContext="WorldContextObject"))
    static ASimulatorHeldStaticPreviewActor* SpawnHeldPreview(
        UObject* WorldContextObject,
        ACharacter* Holder,
        TSubclassOf<AStaticActor> StaticVisualClass,
        const FString& CanonicalModelReference,
        const FSimulatorCharacterInteractionConfig& CharacterConfig,
        const FSimulatorEquipmentInteractionConfig& EquipmentConfig);

    UFUNCTION(BlueprintCallable, Category="Static|Held Preview")
    bool InitializePreview(ACharacter* Holder, AStaticActor* SpawnedVisualActor, const FString& CanonicalModelReference,
        const FSimulatorCharacterInteractionConfig& CharacterConfig, const FSimulatorEquipmentInteractionConfig& EquipmentConfig);

    /** Call from the Static actor's async completion as an immediate alternative to bounded polling. */
    UFUNCTION(BlueprintCallable, Category="Static|Held Preview")
    void NotifyVisualContentReady();

    /** Spawns a new world actor. The held visual is never reused as the placed actor. */
    UFUNCTION(BlueprintCallable, Category="Static|Held Preview")
    AActor* PlaceStatic(const FTransform& WorldTransform, ESpawnActorCollisionHandlingMethod CollisionHandling = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Static|Held Preview")
    TObjectPtr<AStaticActor> VisualActor;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Static|Held Preview")
    FString ModelReference;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Static|Held Preview")
    bool bBoundsFinalized = false;

protected:
    UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> PreviewRoot;

private:
    TWeakObjectPtr<ACharacter> HolderWeak;
    UPROPERTY(Transient)
    TSubclassOf<AStaticActor> StaticActorClass;
    FSimulatorCharacterInteractionConfig StoredCharacterConfig;
    FSimulatorEquipmentInteractionConfig StoredEquipmentConfig;
    FTimerHandle BoundsRetryTimer;
    int32 RemainingBoundsAttempts = 0;

    void ConfigureVisualForPreview();
    void AttachToResolvedHand();
    void TryFinalizeBounds();
    FBox CalculateVisualBounds() const;
    void StopBoundsRetry();
    void DestroyVisualActor();
};
