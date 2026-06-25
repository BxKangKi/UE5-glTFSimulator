// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "InputFunctionLibrary.generated.h"

class UInputMappingContext;

UCLASS() class UInputFunctionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// Add an Input Mapping Context.
	UFUNCTION(BlueprintCallable, Category = "System|Enhanced Input")
	static void AddInputMappingContext(APawn *pawn, UInputMappingContext *context, int32 priority);

	// Remove an Input Mapping Context.
	UFUNCTION(BlueprintCallable, Category = "System|Enhanced Input")
	static void RemoveInputMappingContext(APawn *pawn, UInputMappingContext *context);
};