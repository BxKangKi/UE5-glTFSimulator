// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Model/MaterialDefaultAsset.h"
#include "GlTFSimulatorAssetRegistry.generated.h"

class AActor;
class AGameModeBase;
class AMainGameMode;
class ASingleplayGameMode;
class AMultiplayGameMode;
class AStaticActor;
class ADynamicActor;
class AVehiclePawn;
class AWeaponActor;
class AWeaponProjectileActor;
class AWorldEnvManager;
class AWaterActor;
class UCreatorHUDWidget;
class UPauseMenuWidget;
class USettingsMenuWidget;
class UBuildStatusWidget;
class UStartWorldWidget;
class UWorldSelectionWidget;
class UProjectSelectionWidget;
class UMaterialInterface;
class UStaticMesh;
class USkeletalMesh;
class UPhysicsAsset;
class UPhysicalMaterial;
class USkeleton;
class USoundBase;
class UInputMappingContext;
class UInputAction;
class UAnimInstance;
class UNiagaraSystem;
class UUserWidget;
class UWorld;


USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FGlTFSimulatorFootstepBinding
{
    GENERATED_BODY()

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Character|Footsteps")
    TSoftObjectPtr<UPhysicalMaterial> PhysicalMaterial;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Character|Footsteps")
    TSoftObjectPtr<USoundBase> Sound;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Character|Footsteps")
    TSoftObjectPtr<UNiagaraSystem> Effect;
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FGlTFWorldLaunchProfile
{
    GENERATED_BODY()

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="World Launch")
    FString WorldFolderName;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="World Launch")
    TSoftObjectPtr<UWorld> SinglePlayerWorld;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="World Launch")
    TSoftObjectPtr<UWorld> HostWorld;

};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FGlTFSimulatorInputMappingContextConfig
{
    GENERATED_BODY()

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputMappingContext> MappingContext;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    int32 Priority = 50;
};

/**
 * Project-wide editor asset registry.
 *
 * The custom GameInstance creates one private transient instance from AssetRegistryClass. Every
 * heavyweight asset/class/world inside the registry is a soft reference, so creating the registry does not make those
 * packages resident. Runtime systems resolve only what they need and keep a reflected strong
 * reference for exactly as long as the resolved asset is in use.
 */
UCLASS(BlueprintType, Blueprintable)
class GLTFSIMULATOR_API UGlTFSimulatorAssetRegistry : public UDataAsset
{
    GENERATED_BODY()

public:
    // Native actor classes can be replaced by Blueprint subclasses without forcing them to load at startup.
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Actors")
    TSoftClassPtr<AStaticActor> StaticActorClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Actors")
    TSoftClassPtr<ADynamicActor> DynamicActorClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Actors")
    TSoftClassPtr<AVehiclePawn> VehiclePawnClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Actors")
    TSoftClassPtr<AWeaponActor> WeaponActorClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Actors")
    TSoftClassPtr<AWeaponProjectileActor> WeaponProjectileActorClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Actors")
    TSoftClassPtr<AWorldEnvManager> WorldEnvManagerClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Actors")
    TSoftClassPtr<AWaterActor> WaterActorClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Actors")
    TSoftClassPtr<AActor> RainWeatherActorClass;

    // UI classes are soft references. GameMode/PlayerController create missing top-level instances automatically at runtime.
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UStartWorldWidget> StartMenuWidgetClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UWorldSelectionWidget> WorldSelectionWidgetClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UStartWorldWidget> MultiplayerMenuWidgetClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UProjectSelectionWidget> ProjectSelectionWidgetClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UPauseMenuWidget> PauseMenuWidgetClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<USettingsMenuWidget> SettingsMenuWidgetClass;

    /** WBP subclass of UBuildStatusWidget shown while a Projects entry is being built. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UBuildStatusWidget> BuildStatusWidgetClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UCreatorHUDWidget> CreatorHUDWidgetClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UUserWidget> DebugWidgetClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UUserWidget> LoadingWidgetClass;

    // Menu/navigation worlds. Soft references prevent menu maps from being resident in gameplay.
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Worlds")
    TSoftObjectPtr<UWorld> GameplayWorld;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Worlds")
    TSoftObjectPtr<UWorld> HostWorld;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Worlds")
    TSoftObjectPtr<UWorld> ClientWorld;

    /** Single MainWorld. Start, world selection, multiplayer, project build and settings are UI states inside this same map. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Worlds")
    TSoftObjectPtr<UWorld> MainWorld;

    /** Gameplay GameMode classes used as explicit travel overrides when the common gameplay map is shared. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Game Modes")
    TSoftClassPtr<ASingleplayGameMode> SingleplayGameModeClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Game Modes")
    TSoftClassPtr<AMultiplayGameMode> MultiplayGameModeClass;

    /** Optional per-world map overrides. GameMode selection is fixed by SingleplayGameModeClass / MultiplayGameModeClass. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Worlds", meta=(TitleProperty="WorldFolderName"))
    TArray<FGlTFWorldLaunchProfile> WorldLaunchProfiles;

    // Rendering/default assets.
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Rendering")
    TSoftObjectPtr<UMaterialInterface> PlacementGridMaterial;

    /** Default glTF material soft references live directly in this one central data object. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Rendering")
    FglTFMaterialSoftAssetReferences GlTFMaterials;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Rendering")
    TSoftObjectPtr<UMaterialInterface> StaticDecalLightMaterial;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Environment")
    TSoftObjectPtr<UStaticMesh> SkyboxMesh;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Environment")
    TSoftObjectPtr<UMaterialInterface> CloudMaterial;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Water")
    TSoftObjectPtr<UStaticMesh> WaterMesh;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Water")
    TSoftObjectPtr<UMaterialInterface> WaterDecalMaterial;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Water")
    TSoftObjectPtr<UMaterialInterface> UnderWaterMaterial;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Weapon")
    TSoftObjectPtr<UStaticMesh> DefaultWeaponMesh;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Character")
    TSoftObjectPtr<USkeletalMesh> DefaultCharacterSkeletalMesh;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Character")
    TSoftObjectPtr<UPhysicsAsset> DefaultCharacterPhysicsAsset;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Character")
    TSoftObjectPtr<USkeleton> DefaultCharacterSkeleton;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Character")
    TSoftObjectPtr<UMaterialInterface> DefaultCharacterMaterial;

    /** Animation Blueprint class applied to the character mesh. The class is loaded only when a character begins play. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Character")
    TSoftClassPtr<UAnimInstance> DefaultCharacterAnimInstanceClass;

    /** Loaded on demand: sound/effect assets are resolved only when their surface is actually stepped on. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Character|Footsteps")
    TArray<FGlTFSimulatorFootstepBinding> FootstepBindings;

    // Enhanced Input is centralized for the same reason: controller Blueprints no longer hard-reference assets.
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputMappingContext> PrimaryInputMappingContext;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    int32 PrimaryInputMappingPriority = 50;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TArray<FGlTFSimulatorInputMappingContextConfig> AdditionalInputMappingContexts;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> MoveAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> LookAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> JumpAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> SprintAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> CrouchAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> FlyAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> RagdollAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> InteractAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> ToggleFirstPersonAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> ChangeCharacterAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> ToolbarScrollAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> ToggleItemListAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> SnapAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> VehicleMoveAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> VehicleThrottleAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> VehicleSteeringAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> VehicleStopAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> PauseAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> DebugAction;
};
