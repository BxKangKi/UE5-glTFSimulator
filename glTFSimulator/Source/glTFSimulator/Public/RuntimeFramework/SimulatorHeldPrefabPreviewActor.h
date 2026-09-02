#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RuntimeFramework/SimulatorInteractionTypes.h"
#include "SimulatorHeldPrefabPreviewActor.generated.h"

class USceneComponent;
class ACharacter;

/** Dedicated transient wrapper for an inventory prefab miniature. */
UCLASS(BlueprintType)
class GLTFSIMULATOR_API ASimulatorHeldPrefabPreviewActor : public AActor
{
    GENERATED_BODY()

public:
    ASimulatorHeldPrefabPreviewActor();
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    UFUNCTION(BlueprintCallable, Category="Prefab|Held Preview", meta=(WorldContext="WorldContextObject"))
    static ASimulatorHeldPrefabPreviewActor* SpawnHeldPreview(
        UObject* WorldContextObject,
        ACharacter* Holder,
        TSubclassOf<AActor> PrefabVisualClass,
        const FString& CanonicalSourcePath,
        const FSimulatorCharacterInteractionConfig& CharacterConfig,
        const FSimulatorEquipmentInteractionConfig& EquipmentConfig);

    UFUNCTION(BlueprintCallable, Category="Prefab|Held Preview")
    bool InitializePreview(ACharacter* Holder, AActor* SpawnedVisualActor, const FString& CanonicalSourcePath,
        const FSimulatorCharacterInteractionConfig& CharacterConfig, const FSimulatorEquipmentInteractionConfig& EquipmentConfig);

    /** Call from the prefab actor's async completion as an immediate alternative to bounded polling. */
    UFUNCTION(BlueprintCallable, Category="Prefab|Held Preview")
    void NotifyVisualContentReady();

    /** Spawns a new world actor. The held visual is never reused as the placed actor. */
    UFUNCTION(BlueprintCallable, Category="Prefab|Held Preview")
    AActor* PlacePrefab(const FTransform& WorldTransform, ESpawnActorCollisionHandlingMethod CollisionHandling = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Prefab|Held Preview")
    TObjectPtr<AActor> VisualActor;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Prefab|Held Preview")
    FString SourcePath;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Prefab|Held Preview")
    bool bBoundsFinalized = false;

protected:
    UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> PreviewRoot;

private:
    TWeakObjectPtr<ACharacter> HolderWeak;
    TSubclassOf<AActor> PrefabClass;
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
