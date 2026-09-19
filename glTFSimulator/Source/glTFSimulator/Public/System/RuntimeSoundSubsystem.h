// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "RuntimeSoundSubsystem.generated.h"

class USoundWaveProcedural;

DECLARE_DYNAMIC_DELEGATE_TwoParams(
    FRuntimeSoundLoadCompleted,
    USoundWaveProcedural*, Sound,
    const FString&, Error);

/**
 * Runtime loader for AssetType=Sound entries stored in .gworld/.gasset archives.
 *
 * Archive range I/O and WAV decoding run on a tracked worker. Only USoundWaveProcedural creation
 * and PCM publication run on GameThread. No platform codec or source-project path is used at runtime.
 */
UCLASS()
class GLTFSIMULATOR_API URuntimeSoundSubsystem final : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintPure, Category="Sound", meta=(WorldContext="WorldContextObject"))
    static URuntimeSoundSubsystem* Get(const UObject* WorldContextObject);

    /** Loads an exact gworld://sound/<UUID> reference asynchronously. */
    UFUNCTION(BlueprintCallable, Category="Sound")
    void LoadSoundAsync(const FString& SoundReference, FRuntimeSoundLoadCompleted Completion);

private:
    uint64 RequestGeneration = 1;
};
