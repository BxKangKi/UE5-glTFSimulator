/**
 * @file InteractionBlueprintLibrary.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Simulator/InteractionBlueprintLibrary.h"
#include "Simulator/HeldStaticPreviewActor.h"
#include "Model/StaticActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"

bool USimulatorInteractionBlueprintLibrary::EquipActor(ACharacter* Character, AActor* Equipment,
    const FSimulatorCharacterInteractionConfig& CharacterConfig, const FSimulatorEquipmentInteractionConfig& InputConfig, FString& OutError)
{
    if (!IsValid(Character) || !IsValid(Character->GetMesh()) || !IsValid(Equipment)) { OutError = TEXT("Character, skeletal mesh, or equipment is invalid"); return false; }
    FSimulatorEquipmentInteractionConfig Config = InputConfig; Config.Sanitize(); const ESimulatorHand Hand = Config.ResolvePrimaryHand(CharacterConfig.DominantHand);
    const FSimulatorGripPoint* Grip = Config.GetGrip(Hand); const FName Socket = Grip && !Grip->CharacterSocket.IsNone() ? Grip->CharacterSocket : (Hand == ESimulatorHand::Right ? CharacterConfig.RightHandSocket : CharacterConfig.LeftHandSocket);
    if (!Character->GetMesh()->DoesSocketExist(Socket)) { OutError = FString::Printf(TEXT("Character socket '%s' does not exist"), *Socket.ToString()); return false; }
    Equipment->AttachToComponent(Character->GetMesh(), FAttachmentTransformRules::SnapToTargetNotIncludingScale, Socket);
    Equipment->SetActorRelativeTransform(Grip ? Grip->AttachmentOffset : FTransform::Identity);
    OutError.Reset(); return true;
}

void USimulatorInteractionBlueprintLibrary::UnequipActor(ACharacter* Character, AActor* Equipment, const bool bDestroyEquipment)
{
    if (IsValid(Equipment)) { Equipment->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform); if (bDestroyEquipment) Equipment->Destroy(); }
}

ASimulatorHeldStaticPreviewActor* USimulatorInteractionBlueprintLibrary::HoldStatic(UObject* WorldContextObject, ACharacter* Character,
    TSubclassOf<AStaticActor> StaticClass, const FString& CanonicalModelReference, const FSimulatorCharacterInteractionConfig& CharacterConfig,
    const FSimulatorEquipmentInteractionConfig& EquipmentConfig, FString& OutError)
{
    ASimulatorHeldStaticPreviewActor* Preview = ASimulatorHeldStaticPreviewActor::SpawnHeldPreview(WorldContextObject, Character, StaticClass, CanonicalModelReference, CharacterConfig, EquipmentConfig);
    if (!IsValid(Preview)) { OutError = TEXT("held Static preview spawn failed"); return nullptr; }
    OutError.Reset(); return Preview;
}
