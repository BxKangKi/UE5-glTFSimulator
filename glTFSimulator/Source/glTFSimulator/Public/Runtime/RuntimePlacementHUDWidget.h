// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "RuntimePlacementHUDWidget.generated.h"

class ARuntimeGameplayManager;

/**
 * Compatibility shell only.
 * Native Runtime HUD creation and fallback button UI were removed.
 * Prefer creating your own Blueprint UserWidget and calling ARuntimeGameplayManager functions directly.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API URuntimePlacementHUDWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category="Runtime UI")
    void SetManager(ARuntimeGameplayManager* InManager);

    UFUNCTION(BlueprintPure, Category="Runtime UI")
    ARuntimeGameplayManager* GetManager() const { return Manager.Get(); }

    UFUNCTION(BlueprintCallable, Category="Runtime UI")
    void Refresh();

    UFUNCTION(BlueprintCallable, Category="Runtime UI")
    void RebindRuntimeButtons();

private:
    UPROPERTY()
    TObjectPtr<ARuntimeGameplayManager> Manager;
};
