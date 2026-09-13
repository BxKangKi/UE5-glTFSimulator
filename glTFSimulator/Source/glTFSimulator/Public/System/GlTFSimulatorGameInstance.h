// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "GlTFSimulatorGameInstance.generated.h"

class UGlTFSimulatorAssetRegistry;

/**
 * Project GameInstance. Assign only AssetRegistryClass in the GameInstance defaults.
 * A transient registry object is created internally from that class; no registry instance is exposed
 * as project configuration. Heavy entries inside the registry remain soft references.
 */
UCLASS(Blueprintable, BlueprintType)
class GLTFSIMULATOR_API UGlTFSimulatorGameInstance : public UGameInstance
{
    GENERATED_BODY()

public:
    virtual void Init() override;

    /** Ensures the private runtime registry instance exists. Safe to call repeatedly from GameMode startup. */
    bool EnsureAssetRegistry();

    UFUNCTION(BlueprintPure, Category="glTFSimulator|Assets")
    UGlTFSimulatorAssetRegistry* GetAssetRegistry() const;

    UFUNCTION(BlueprintPure, Category="glTFSimulator|Assets", meta=(WorldContext="WorldContextObject"))
    static UGlTFSimulatorAssetRegistry* GetAssetRegistryFromContext(const UObject* WorldContextObject);

protected:
    /**
     * The only project-facing registry setting. Assign a Blueprint subclass of
     * UGlTFSimulatorAssetRegistry in the GameInstance defaults.
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="glTFSimulator|Assets")
    TSoftClassPtr<UGlTFSimulatorAssetRegistry> AssetRegistryClass;

private:
    /** Private runtime instance created from AssetRegistryClass. Never exposed as an editable project setting. */
    UPROPERTY(Transient)
    TObjectPtr<UGlTFSimulatorAssetRegistry> RuntimeAssetRegistry;
};
