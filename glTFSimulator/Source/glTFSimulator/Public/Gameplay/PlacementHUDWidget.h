// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "System/GameManagerSubSystem.h"
#include "PlacementHUDWidget.generated.h"

class UGameManagerSubSystem;

/**
 * Compatibility shell only.
 * Native Gameplay HUD creation and fallback button UI were removed.
 * Prefer creating your own Blueprint UserWidget and calling UGameManagerSubSystem functions directly.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UPlacementHUDWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category="UI")
    void SetManager(UGameManagerSubSystem* InManager);

    UFUNCTION(BlueprintPure, Category="UI")
    UGameManagerSubSystem* GetManager() const { return Manager.Get(); }

    UFUNCTION(BlueprintCallable, Category="UI")
    void Refresh();

    UFUNCTION(BlueprintCallable, Category="UI")
    void RebindButtons();

private:
    UPROPERTY()
    TObjectPtr<UGameManagerSubSystem> Manager;
};
