#include "RuntimeFramework/SimulatorInteractionBlueprintLibrary.h"
#include "RuntimeFramework/SimulatorHeldPrefabPreviewActor.h"
#include "RuntimeFramework/SimulatorInteractionAnimInstance.h"
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
    USimulatorInteractionAnimInstance* Anim = Cast<USimulatorInteractionAnimInstance>(Character->GetMesh()->GetAnimInstance());
    if (!Anim) { Equipment->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform); OutError = TEXT("Character AnimInstance is not SimulatorInteractionAnimInstance"); return false; }
    Anim->SetCharacterInteractionConfig(CharacterConfig); Anim->EquipInteractionActor(Equipment, Config); OutError.Reset(); return true;
}

void USimulatorInteractionBlueprintLibrary::UnequipActor(ACharacter* Character, AActor* Equipment, const bool bDestroyEquipment)
{
    if (IsValid(Character) && IsValid(Character->GetMesh())) if (USimulatorInteractionAnimInstance* Anim = Cast<USimulatorInteractionAnimInstance>(Character->GetMesh()->GetAnimInstance())) Anim->ClearInteractionActor();
    if (IsValid(Equipment)) { Equipment->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform); if (bDestroyEquipment) Equipment->Destroy(); }
}

ASimulatorHeldPrefabPreviewActor* USimulatorInteractionBlueprintLibrary::HoldPrefab(UObject* WorldContextObject, ACharacter* Character,
    TSubclassOf<AActor> PrefabClass, const FString& CanonicalSourcePath, const FSimulatorCharacterInteractionConfig& CharacterConfig,
    const FSimulatorEquipmentInteractionConfig& EquipmentConfig, FString& OutError)
{
    ASimulatorHeldPrefabPreviewActor* Preview = ASimulatorHeldPrefabPreviewActor::SpawnHeldPreview(WorldContextObject, Character, PrefabClass, CanonicalSourcePath, CharacterConfig, EquipmentConfig);
    if (!IsValid(Preview)) { OutError = TEXT("Held prefab preview spawn failed"); return nullptr; }
    OutError.Reset(); return Preview;
}
