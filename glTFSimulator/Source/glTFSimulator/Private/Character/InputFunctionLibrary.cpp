// Copyright © 2025 BxKangKi. Licensed under the MIT License.
// Copyright © 2025 Epic Games, Inc. All rights reserved.

#include "Character/InputFunctionLibrary.h"
#include "InputMappingContext.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/PlayerController.h"

void UInputFunctionLibrary::AddInputMappingContext(APawn *pawn, UInputMappingContext *context, int32 priority)
{
    if (!pawn || !context)
        return;

    if (AController *ctrl = pawn->GetController())
    {
        if (APlayerController *playerCtrl = Cast<APlayerController>(ctrl))
        {
            if (ULocalPlayer *local = playerCtrl->GetLocalPlayer())
            {
                if (UEnhancedInputLocalPlayerSubsystem *Subsystem = local->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
                {
                    Subsystem->AddMappingContext(context, priority);
                }
            }
        }
    }
}

void UInputFunctionLibrary::RemoveInputMappingContext(APawn *pawn, UInputMappingContext *context)
{
    if (!pawn || !context)
        return;

    if (AController *ctrl = pawn->GetController())
    {
        if (APlayerController *playerCtrl = Cast<APlayerController>(ctrl))
        {
            if (ULocalPlayer *local = playerCtrl->GetLocalPlayer())
            {
                if (UEnhancedInputLocalPlayerSubsystem *Subsystem = local->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
                {
                    Subsystem->RemoveMappingContext(context);
                }
            }
        }
    }
}