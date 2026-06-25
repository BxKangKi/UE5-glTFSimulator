// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "StartActor.generated.h"

UCLASS() class GLTFSIMULATOR_API AStartActor : public AActor
{
    GENERATED_BODY()
protected:
    virtual void BeginPlay() override;
    UFUNCTION(BlueprintCallable)
    TMap<FString, FString> GetFolderNameMap() const { return FolderNameMap; }
private:
    void BuildLevelFolderNameMap();
    UPROPERTY()
    TMap<FString, FString> FolderNameMap;
};