/**
 * @file HeldStaticPreviewActor.h
 * 역할: 손에 든 정적 객체 미리보기 액터를 제공합니다.
 * 핵심 기능: 임시 프리뷰 메시와 렌더 상태 수명 관리.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
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
