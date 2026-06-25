// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Runtime/RuntimeVehiclePawn.h"

#include "Camera/CameraComponent.h"
#include "Character/CharacterComponent.h"
#include "Character/CharacterController.h"
#include "Character/PlayerCharacterController.h"
#include "Components/BoxComponent.h"
#include "GameFramework/Character.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "GameFramework/PlayerController.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/SpringArmComponent.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "ProceduralMeshComponent.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeFunctionLibrary.h"
#include "glTFRuntimeParser.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "World/RuntimeBuoyancyComponent.h"


static TMap<EglTFRuntimeMaterialType, UMaterialInterface*> BuildRuntimeVehicleLitMaterialOverrides()
    {
        TMap<EglTFRuntimeMaterialType, UMaterialInterface*> Overrides;

        if (UMaterialInterface* Opaque = LoadObject<UMaterialInterface>(nullptr, TEXT("/glTFRuntime/M_glTFRuntimeBase")))
        {
            Overrides.Add(EglTFRuntimeMaterialType::Opaque, Opaque);
            Overrides.Add(EglTFRuntimeMaterialType::Masked, Opaque);
        }
        if (UMaterialInterface* Translucent = LoadObject<UMaterialInterface>(nullptr, TEXT("/glTFRuntime/M_glTFRuntimeTranslucent_Inst")))
        {
            Overrides.Add(EglTFRuntimeMaterialType::Translucent, Translucent);
            Overrides.Add(EglTFRuntimeMaterialType::TwoSidedTranslucent, Translucent);
        }
        if (UMaterialInterface* TwoSided = LoadObject<UMaterialInterface>(nullptr, TEXT("/glTFRuntime/M_glTFRuntimeTwoSided_Inst")))
        {
            Overrides.Add(EglTFRuntimeMaterialType::TwoSided, TwoSided);
            Overrides.Add(EglTFRuntimeMaterialType::TwoSidedMasked, TwoSided);
        }

        return Overrides;
    }

static bool IsRuntimeVehicleWheelTaggedName(const FString& Name)
    {
        return Name.EndsWith(TEXT(";WHEL"), ESearchCase::IgnoreCase)
            || Name.EndsWith(TEXT(";WHEEL"), ESearchCase::IgnoreCase);
    }

static FTransform GetRuntimeVehicleNodeWorldTransform(const TMap<int32, FglTFRuntimeNode>& NodeMap, const FglTFRuntimeNode& Node)
    {
        FTransform WorldTransform = Node.Transform;
        int32 ParentIndex = Node.ParentIndex;
        TSet<int32> VisitedParents;

        while (const FglTFRuntimeNode* Parent = NodeMap.Find(ParentIndex))
        {
            if (VisitedParents.Contains(ParentIndex))
            {
                break;
            }
            VisitedParents.Add(ParentIndex);
            WorldTransform = WorldTransform * Parent->Transform;
            ParentIndex = Parent->ParentIndex;
        }

        return WorldTransform;
    }

struct FRuntimeVehicleWheelVisual
    {
        FglTFRuntimeNode Node;
        FTransform Transform = FTransform::Identity;
};

ARuntimeVehiclePawn::ARuntimeVehiclePawn()
{
    PrimaryActorTick.bCanEverTick = true;

    Body = CreateDefaultSubobject<UBoxComponent>(TEXT("PhysicsBody"));
    SetRootComponent(Body);
    Body->InitBoxExtent(BodyExtent);
    Body->SetCollisionProfileName(TEXT("Vehicle"));
    Body->SetSimulatePhysics(true);
    ApplyVehicleBodyPhysicsSettings();
    Body->SetUseCCD(true);

    LowFrictionPhysicalMaterial = CreateDefaultSubobject<UPhysicalMaterial>(TEXT("RuntimeVehicleLowFriction"));
    if (LowFrictionPhysicalMaterial)
    {
        LowFrictionPhysicalMaterial->Friction = 0.06f;
        LowFrictionPhysicalMaterial->StaticFriction = 0.06f;
        LowFrictionPhysicalMaterial->bOverrideFrictionCombineMode = true;
        LowFrictionPhysicalMaterial->FrictionCombineMode = EFrictionCombineMode::Min;
        LowFrictionPhysicalMaterial->Restitution = 0.0f;
        Body->SetPhysMaterialOverride(LowFrictionPhysicalMaterial);
    }

    BodyMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("BodyMesh"));
    BodyMesh->SetupAttachment(Body);
    BodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

    for (int32 WheelIndex = 0; WheelIndex < 4; ++WheelIndex)
    {
        UProceduralMeshComponent* WheelMesh = CreateDefaultSubobject<UProceduralMeshComponent>(*FString::Printf(TEXT("WheelMesh_%d"), WheelIndex));
        WheelMesh->SetupAttachment(Body);
        WheelMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        WheelMeshes.Add(WheelMesh);
    }

    BuoyancyComponent = CreateDefaultSubobject<URuntimeBuoyancyComponent>(TEXT("RuntimeBuoyancy"));

    bUseControllerRotationPitch = false;
    bUseControllerRotationYaw = false;
    bUseControllerRotationRoll = false;

    SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("VehicleSpringArm"));
    SpringArm->SetupAttachment(Body);
    SpringArm->TargetArmLength = 550.0f;
    SpringArm->SetRelativeRotation(FRotator(-12.0f, 0.0f, 0.0f));
    SpringArm->bUsePawnControlRotation = true;
    SpringArm->bInheritPitch = true;
    SpringArm->bInheritYaw = true;
    SpringArm->bInheritRoll = false;
    SpringArm->bEnableCameraLag = true;
    SpringArm->CameraLagSpeed = 8.0f;
    SpringArm->bEnableCameraRotationLag = true;
    SpringArm->CameraRotationLagSpeed = 12.0f;

    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("VehicleCamera"));
    Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
    Camera->bUsePawnControlRotation = false;

    // Chassis-space suspension mount points. +X is front, +Y is right.
    WheelOffsets.Add(FVector(112.0f, 66.0f, -36.0f));
    WheelOffsets.Add(FVector(112.0f, -66.0f, -36.0f));
    WheelOffsets.Add(FVector(-112.0f, 66.0f, -36.0f));
    WheelOffsets.Add(FVector(-112.0f, -66.0f, -36.0f));
}

void ARuntimeVehiclePawn::BeginPlay()
{
    Super::BeginPlay();
    Body->InitBoxExtent(BodyExtent);
    ApplyVehicleBodyPhysicsSettings();
    BuildBodyMesh();
    BuildWheelMeshes();
    ResetVehiclePoseAboveGround();

    WheelSpinDegrees.Init(0.0f, WheelOffsets.Num());
    WheelSpringLengths.Init(SuspensionRestLength * FMath::Clamp(LoadedWheelVisualRestLengthRatio, 0.2f, 0.95f), WheelOffsets.Num());
    WheelGrounded.Init(false, WheelOffsets.Num());
}

void ARuntimeVehiclePawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);
}

float ARuntimeVehiclePawn::GetVehicleMassScale() const
{
    return FMath::Max(1.0f, VehicleMassKg) / 1000.0f;
}

void ARuntimeVehiclePawn::ApplyVehicleBodyPhysicsSettings()
{
    if (!IsValid(Body))
    {
        return;
    }

    Body->SetMassOverrideInKg(NAME_None, FMath::Max(1.0f, VehicleMassKg), true);
    Body->SetLinearDamping(0.32f);
    Body->SetAngularDamping(2.10f);
    Body->SetCenterOfMass(FVector(4.0f, 0.0f, -48.0f), NAME_None);
    Body->SetPhysicsMaxAngularVelocityInRadians(FMath::Max(0.5f, MaxAngularVelocityRadians), false);
}

void ARuntimeVehiclePawn::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    const float SafeDeltaTime = FMath::Clamp(DeltaSeconds, 0.0f, 0.05f);
    SmoothedThrottleInput = FMath::FInterpTo(SmoothedThrottleInput, ThrottleInput, SafeDeltaTime, FMath::Max(0.1f, ThrottleInputInterpSpeed));
    SmoothedSteeringInput = FMath::FInterpTo(SmoothedSteeringInput, SteeringInput, SafeDeltaTime, FMath::Max(0.1f, SteeringInputInterpSpeed));
    ApplySuspensionAndDrive(SafeDeltaTime);
    UpdateWheelVisuals(SafeDeltaTime);
}

void ARuntimeVehiclePawn::SetDriveInput(float Throttle, float Steering)
{
    ThrottleInput = FMath::Clamp(Throttle, -1.0f, 1.0f);
    SteeringInput = FMath::Clamp(Steering, -1.0f, 1.0f);
    if (IsValid(Body) && !FMath::IsNearlyZero(ThrottleInput, 0.01f))
    {
        Body->WakeRigidBody();
    }
}

void ARuntimeVehiclePawn::SetThrottleInput(float Throttle)
{
    ThrottleInput = FMath::Clamp(Throttle, -1.0f, 1.0f);
    if (IsValid(Body) && !FMath::IsNearlyZero(ThrottleInput, 0.01f))
    {
        Body->WakeRigidBody();
    }
}

void ARuntimeVehiclePawn::SetSteeringInput(float Steering)
{
    SteeringInput = FMath::Clamp(Steering, -1.0f, 1.0f);
}

void ARuntimeVehiclePawn::ClearDriveInput()
{
    ThrottleInput = 0.0f;
    SteeringInput = 0.0f;
    SmoothedThrottleInput = 0.0f;
    SmoothedSteeringInput = 0.0f;
}

void ARuntimeVehiclePawn::ClearLoadedVehicleModel()
{
    for (UStaticMeshComponent* Component : LoadedBodyMeshComponents)
    {
        if (IsValid(Component))
        {
            Component->UnregisterComponent();
            Component->DestroyComponent();
        }
    }
    for (UStaticMeshComponent* Component : LoadedWheelMeshComponents)
    {
        if (IsValid(Component))
        {
            Component->UnregisterComponent();
            Component->DestroyComponent();
        }
    }

    LoadedBodyMeshComponents.Empty();
    LoadedWheelMeshComponents.Empty();
    LoadedWheelBaseRotations.Empty();
    MeshCache.Empty();

    if (IsValid(RuntimeAsset))
    {
        RuntimeAsset->ClearCache();
        RuntimeAsset->MarkAsGarbage();
        RuntimeAsset = nullptr;
    }

    HideProceduralDefaultVisuals(false);
}

void ARuntimeVehiclePawn::HideProceduralDefaultVisuals(bool bHide)
{
    if (IsValid(BodyMesh))
    {
        BodyMesh->SetHiddenInGame(bHide);
        BodyMesh->SetVisibility(!bHide, true);
    }

    const bool bHideDefaultWheels = bHide && LoadedWheelMeshComponents.Num() > 0;
    for (UProceduralMeshComponent* WheelMesh : WheelMeshes)
    {
        if (IsValid(WheelMesh))
        {
            WheelMesh->SetHiddenInGame(bHideDefaultWheels);
            WheelMesh->SetVisibility(!bHideDefaultWheels, true);
        }
    }
}

UStaticMesh* ARuntimeVehiclePawn::LoadMeshByIndex(int32 MeshIndex)
{
    if (!IsValid(RuntimeAsset) || MeshIndex == INDEX_NONE)
    {
        return nullptr;
    }

    if (TObjectPtr<UStaticMesh>* Existing = MeshCache.Find(MeshIndex))
    {
        return Existing->Get();
    }

    FglTFRuntimeStaticMeshConfig MeshConfig;
    MeshConfig.Outer = this;
    MeshConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    MeshConfig.MaterialsConfig.CacheMode = EglTFRuntimeCacheMode::ReadWrite;
    const TMap<EglTFRuntimeMaterialType, UMaterialInterface*> LitOverrides = BuildRuntimeVehicleLitMaterialOverrides();
    if (LitOverrides.Num() > 0)
    {
        MeshConfig.MaterialsConfig.UnlitOverrideMap = LitOverrides;
    }
    MeshConfig.bAllowCPUAccess = true;
    MeshConfig.bBuildSimpleCollision = false;
    MeshConfig.bBuildComplexCollision = false;

    UStaticMesh* Mesh = RuntimeAsset->LoadStaticMesh(MeshIndex, MeshConfig);
    if (IsValid(Mesh))
    {
        MeshCache.Add(MeshIndex, Mesh);
    }
    return Mesh;
}

bool ARuntimeVehiclePawn::LoadVehicleModel(const FString& InFilePath, const FString& InRuntimeName)
{
    ClearLoadedVehicleModel();

    SourceFilePath = FPaths::ConvertRelativePathToFull(InFilePath);
    BaseName = FPaths::GetBaseFilename(SourceFilePath);
    RuntimeName = InRuntimeName.IsEmpty() ? BaseName : InRuntimeName;

    FglTFRuntimeConfig LoaderConfig;
    LoaderConfig.bAllowExternalFiles = true;
    RuntimeAsset = UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(SourceFilePath, false, LoaderConfig);
    if (!IsValid(RuntimeAsset))
    {
        UE_LOG(LogTemp, Warning, TEXT("RuntimeVehiclePawn: failed to load vehicle model %s"), *SourceFilePath);
        SourceFilePath.Reset();
        BaseName = TEXT("Vehicle");
        RuntimeName = TEXT("Vehicle");
        return false;
    }

    const TArray<FglTFRuntimeNode> Nodes = RuntimeAsset->GetNodes();
    TMap<int32, FglTFRuntimeNode> NodeMap;
    for (const FglTFRuntimeNode& Node : Nodes)
    {
        NodeMap.Add(Node.Index, Node);
    }

    TMap<int32, FString> MeshNamesByIndex;
    if (RuntimeAsset->GetParser().IsValid())
    {
        const TArray<TSharedRef<FJsonObject>> MeshObjects = RuntimeAsset->GetParser()->GetMeshes();
        for (int32 MeshIndex = 0; MeshIndex < MeshObjects.Num(); ++MeshIndex)
        {
            FString MeshName;
            if (MeshObjects[MeshIndex]->TryGetStringField(TEXT("name"), MeshName))
            {
                MeshNamesByIndex.Add(MeshIndex, MeshName);
            }
        }
    }

    TArray<FRuntimeVehicleWheelVisual> WheelNodes;
    int32 BodyComponentIndex = 0;
    for (const FglTFRuntimeNode& Node : Nodes)
    {
        if (Node.MeshIndex == INDEX_NONE)
        {
            continue;
        }

        UStaticMesh* Mesh = LoadMeshByIndex(Node.MeshIndex);
        if (!IsValid(Mesh))
        {
            continue;
        }

        const FTransform NodeWorldTransform = GetRuntimeVehicleNodeWorldTransform(NodeMap, Node);
        const FString MeshName = MeshNamesByIndex.FindRef(Node.MeshIndex);
        if (IsRuntimeVehicleWheelTaggedName(Node.Name) || IsRuntimeVehicleWheelTaggedName(MeshName))
        {
            FRuntimeVehicleWheelVisual WheelVisual;
            WheelVisual.Node = Node;
            WheelVisual.Transform = NodeWorldTransform;
            WheelNodes.Add(WheelVisual);
            continue;
        }

        UStaticMeshComponent* MeshComponent = NewObject<UStaticMeshComponent>(this, *FString::Printf(TEXT("VehicleBodyMesh_%d"), BodyComponentIndex++));
        if (!IsValid(MeshComponent))
        {
            continue;
        }

        AddInstanceComponent(MeshComponent);
        MeshComponent->SetMobility(EComponentMobility::Movable);
        MeshComponent->SetupAttachment(Body);
        MeshComponent->SetStaticMesh(Mesh);
        MeshComponent->SetRelativeTransform(NodeWorldTransform);
        MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        MeshComponent->RegisterComponent();
        LoadedBodyMeshComponents.Add(MeshComponent);
    }

    WheelNodes.Sort([](const FRuntimeVehicleWheelVisual& A, const FRuntimeVehicleWheelVisual& B)
    {
        const FVector AL = A.Transform.GetLocation();
        const FVector BL = B.Transform.GetLocation();
        if (!FMath::IsNearlyEqual(AL.X, BL.X, 1.0f))
        {
            return AL.X > BL.X;
        }
        return AL.Y > BL.Y;
    });

    if (WheelNodes.Num() > 0)
    {
        WheelOffsets.Empty();
        LoadedWheelBaseRotations.Empty();
    }

    int32 WheelComponentIndex = 0;
    for (const FRuntimeVehicleWheelVisual& WheelNode : WheelNodes)
    {
        UStaticMesh* Mesh = LoadMeshByIndex(WheelNode.Node.MeshIndex);
        if (!IsValid(Mesh))
        {
            continue;
        }

        UStaticMeshComponent* MeshComponent = NewObject<UStaticMeshComponent>(this, *FString::Printf(TEXT("VehicleWheelMesh_%d"), WheelComponentIndex++));
        if (!IsValid(MeshComponent))
        {
            continue;
        }

        AddInstanceComponent(MeshComponent);
        MeshComponent->SetMobility(EComponentMobility::Movable);
        MeshComponent->SetupAttachment(Body);
        MeshComponent->SetStaticMesh(Mesh);
        MeshComponent->SetRelativeTransform(WheelNode.Transform);
        MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        MeshComponent->RegisterComponent();
        LoadedWheelMeshComponents.Add(MeshComponent);
        LoadedWheelBaseRotations.Add(WheelNode.Transform.GetRotation());

        // glTF wheel nodes are authored as visual wheel centers. The physics code stores suspension mount points,
        // so lift the mount by a small resting spring length and let UpdateWheelVisuals put the wheel back near
        // the authored center. Using the full rest length made wheels appear far below the chassis.
        const float VisualRestLength = SuspensionRestLength * FMath::Clamp(LoadedWheelVisualRestLengthRatio, 0.2f, 0.95f);
        WheelOffsets.Add(WheelNode.Transform.GetLocation() + FVector(0.0f, 0.0f, VisualRestLength));
    }

    if (WheelOffsets.Num() == 0)
    {
        WheelOffsets.Add(FVector(112.0f, 66.0f, -36.0f));
        WheelOffsets.Add(FVector(112.0f, -66.0f, -36.0f));
        WheelOffsets.Add(FVector(-112.0f, 66.0f, -36.0f));
        WheelOffsets.Add(FVector(-112.0f, -66.0f, -36.0f));
    }

    WheelSpinDegrees.Init(0.0f, WheelOffsets.Num());
    WheelSpringLengths.Init(SuspensionRestLength * FMath::Clamp(LoadedWheelVisualRestLengthRatio, 0.2f, 0.95f), WheelOffsets.Num());
    WheelGrounded.Init(false, WheelOffsets.Num());

    const bool bLoadedAnyVisual = LoadedBodyMeshComponents.Num() > 0 || LoadedWheelMeshComponents.Num() > 0;
    HideProceduralDefaultVisuals(bLoadedAnyVisual);
    ResetVehiclePoseAboveGround();
    return bLoadedAnyVisual;
}

void ARuntimeVehiclePawn::ResetVehiclePoseAboveGround()
{
    if (!IsValid(Body))
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    const FVector CurrentLocation = GetActorLocation();
    const FVector TraceStart = CurrentLocation + FVector(0.0f, 0.0f, 400.0f);
    const FVector TraceEnd = CurrentLocation - FVector(0.0f, 0.0f, 1400.0f);
    FHitResult Hit;
    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(RuntimeVehicleGroundClearance), false, this);
    QueryParams.AddIgnoredActor(this);

    if (World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, QueryParams))
    {
        const float TargetZ = Hit.ImpactPoint.Z + GetDesiredCenterHeightAboveGround();
        SetActorLocation(FVector(CurrentLocation.X, CurrentLocation.Y, TargetZ), false, nullptr, ETeleportType::TeleportPhysics);
    }
    else
    {
        SetActorLocation(CurrentLocation + FVector(0.0f, 0.0f, GetDesiredCenterHeightAboveGround()), false, nullptr, ETeleportType::TeleportPhysics);
    }

    Body->SetPhysicsLinearVelocity(FVector::ZeroVector);
    Body->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
    Body->WakeRigidBody();
}

FRuntimePlacedObjectRecord ARuntimeVehiclePawn::ToPlacementRecord(int32 VehicleRecordIndex) const
{
    FRuntimePlacedObjectRecord Record;
    Record.RuntimeName = RuntimeName.IsEmpty()
        ? (VehicleRecordIndex == 0 ? TEXT("Vehicle") : TEXT("Vehicle;INST"))
        : RuntimeName;
    Record.BaseName = BaseName.IsEmpty() ? TEXT("Vehicle") : BaseName;
    Record.SourceFile = SourceFilePath;
    Record.Kind = ERuntimePlacedObjectKind::Vehicle;
    Record.Transform = GetActorTransform();
    return Record;
}

bool ARuntimeVehiclePawn::EnterVehicle(APlayerController* PlayerController, APawn* PreviousPawn)
{
    if (!IsValid(PlayerController) || IsOccupied())
    {
        return false;
    }

    if (ACharacterController* CharacterPawn = Cast<ACharacterController>(PreviousPawn))
    {
        if (UCharacterComponent* CharacterState = CharacterPawn->GetCharacterComponent())
        {
            if (CharacterState->IsRagdollActive() || CharacterState->IsGettingUp())
            {
                return false;
            }
        }

        CharacterPawn->ClearTransientInputState();
    }

    if (APlayerCharacterController* RuntimeController = Cast<APlayerCharacterController>(PlayerController))
    {
        RuntimeController->ClearLatchedMovementInput();
    }

    OccupyingController = PlayerController;
    StoredPawn = PreviousPawn;
    StoredControlRotation = PlayerController->GetControlRotation();
    bHasStoredControlRotation = true;
    bHasStoredPawnTransform = IsValid(StoredPawn);
    if (IsValid(StoredPawn))
    {
        StoredPawnTransformBeforeEnter = StoredPawn->GetActorTransform();
        StoredPawn->SetActorHiddenInGame(true);
        StoredPawn->SetActorEnableCollision(false);
        StoredPawn->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
    }

    ClearDriveInput();
    PlayerController->Possess(this);
    PlayerController->SetViewTarget(this);
    if (bHasStoredControlRotation)
    {
        PlayerController->SetControlRotation(StoredControlRotation);
    }
    return true;
}

void ARuntimeVehiclePawn::ExitVehicle()
{
    if (!IsValid(OccupyingController))
    {
        return;
    }

    ClearDriveInput();

    if (APlayerCharacterController* RuntimeController = Cast<APlayerCharacterController>(OccupyingController))
    {
        RuntimeController->ClearLatchedMovementInput();
    }

    if (ACharacterController* CharacterPawn = Cast<ACharacterController>(StoredPawn))
    {
        CharacterPawn->ClearTransientInputState();
    }

    if (IsValid(StoredPawn))
    {
        const FRotator RestoreRotation = bHasStoredControlRotation
            ? FRotator(0.0f, StoredControlRotation.Yaw, 0.0f)
            : (bHasStoredPawnTransform ? StoredPawnTransformBeforeEnter.GetRotation().Rotator() : StoredPawn->GetActorRotation());

        FVector SafeExitLocation = GetExitLocation();
        FRotator SafeExitRotation = RestoreRotation;
        if (!FindSafeExitTransform(StoredPawn, SafeExitLocation, SafeExitRotation))
        {
            UE_LOG(LogTemp, Warning, TEXT("RuntimeVehiclePawn: no walkable exit location found within one vehicle length. Staying in vehicle."));
            return;
        }

        StoredPawn->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
        StoredPawn->SetActorLocationAndRotation(SafeExitLocation, SafeExitRotation, false, nullptr, ETeleportType::TeleportPhysics);
        StoredPawn->SetActorHiddenInGame(false);
        StoredPawn->SetActorEnableCollision(true);
        OccupyingController->Possess(StoredPawn);
        RestoreStoredPawnCamera(OccupyingController, StoredPawn);

        if (APlayerCharacterController* RuntimeController = Cast<APlayerCharacterController>(OccupyingController))
        {
            RuntimeController->ApplyGameInputMode();
            RuntimeController->ClearLatchedMovementInput();
        }
    }

    OccupyingController = nullptr;
    StoredPawn = nullptr;
    StoredControlRotation = FRotator::ZeroRotator;
    StoredPawnTransformBeforeEnter = FTransform::Identity;
    bHasStoredControlRotation = false;
    bHasStoredPawnTransform = false;
}

FVector ARuntimeVehiclePawn::GetExitLocation() const
{
    return GetActorLocation() + GetActorRightVector() * 220.0f + FVector(0.0f, 0.0f, 80.0f);
}

bool ARuntimeVehiclePawn::FindSafeExitTransform(APawn* PawnToExit, FVector& OutLocation, FRotator& OutRotation) const
{
    if (!IsValid(PawnToExit))
    {
        return false;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return false;
    }

    float CapsuleRadius = 34.0f;
    float CapsuleHalfHeight = 88.0f;
    if (const ACharacter* CharacterToExit = Cast<ACharacter>(PawnToExit))
    {
        if (const UCapsuleComponent* Capsule = CharacterToExit->GetCapsuleComponent())
        {
            CapsuleRadius = FMath::Max(1.0f, Capsule->GetScaledCapsuleRadius());
            CapsuleHalfHeight = FMath::Max(CapsuleRadius + 1.0f, Capsule->GetScaledCapsuleHalfHeight());
        }
    }
    else
    {
        const FBox PawnBounds = PawnToExit->GetComponentsBoundingBox(true);
        if (PawnBounds.IsValid)
        {
            const FVector Extent = PawnBounds.GetExtent();
            CapsuleRadius = FMath::Max(20.0f, FMath::Max(Extent.X, Extent.Y));
            CapsuleHalfHeight = FMath::Max(CapsuleRadius + 1.0f, Extent.Z);
        }
    }

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(RuntimeVehicleExitTrace), false, this);
    QueryParams.AddIgnoredActor(this);
    QueryParams.AddIgnoredActor(PawnToExit);

    const FCollisionShape CapsuleShape = FCollisionShape::MakeCapsule(CapsuleRadius, CapsuleHalfHeight);
    const FVector VehicleLocation = GetActorLocation();
    const FVector Forward = GetActorForwardVector().GetSafeNormal2D();
    const FVector Right = GetActorRightVector().GetSafeNormal2D();
    const float OneVehicleLength = FMath::Max(BodyExtent.X * 2.0f, BodyExtent.Y * 2.0f) + CapsuleRadius + 80.0f;
    const float MinSideDistance = FMath::Clamp(BodyExtent.Y + CapsuleRadius + 65.0f, CapsuleRadius + 60.0f, OneVehicleLength);
    const float MinFrontBackDistance = FMath::Clamp(BodyExtent.X + CapsuleRadius + 55.0f, CapsuleRadius + 70.0f, OneVehicleLength);

    struct FExitCandidate
    {
        FVector Location = FVector::ZeroVector;
        float DistanceSquared = 0.0f;
    };

    TArray<FExitCandidate> Candidates;
    auto AddCandidate = [&](const FVector& Direction, float Distance)
    {
        const FVector FlatDirection = Direction.GetSafeNormal2D();
        if (FlatDirection.IsNearlyZero() || Distance > OneVehicleLength + KINDA_SMALL_NUMBER)
        {
            return;
        }

        FExitCandidate Candidate;
        Candidate.Location = VehicleLocation + FlatDirection * Distance;
        Candidate.DistanceSquared = FVector::DistSquared2D(Candidate.Location, VehicleLocation);
        Candidates.Add(Candidate);
    };

    // Preferred side candidates first, then a bounded ring search around the vehicle.
    AddCandidate(Right, MinSideDistance);
    AddCandidate(-Right, MinSideDistance);
    AddCandidate(-Forward, MinFrontBackDistance);
    AddCandidate(Forward, MinFrontBackDistance);

    const FVector Directions[] = {
        Right,
        -Right,
        Forward,
        -Forward,
        (Right + Forward).GetSafeNormal2D(),
        (-Right + Forward).GetSafeNormal2D(),
        (Right - Forward).GetSafeNormal2D(),
        (-Right - Forward).GetSafeNormal2D()
    };

    const float Step = FMath::Max(45.0f, CapsuleRadius + 22.0f);
    for (float Distance = FMath::Min(MinSideDistance, MinFrontBackDistance); Distance <= OneVehicleLength + KINDA_SMALL_NUMBER; Distance += Step)
    {
        for (const FVector& Direction : Directions)
        {
            AddCandidate(Direction, Distance);
        }
    }

    Candidates.Sort([](const FExitCandidate& A, const FExitCandidate& B)
    {
        return A.DistanceSquared < B.DistanceSquared;
    });

    const float TraceTopZ = VehicleLocation.Z + BodyExtent.Z + CapsuleHalfHeight + 420.0f;
    const float TraceBottomZ = VehicleLocation.Z - BodyExtent.Z - CapsuleHalfHeight - 700.0f;
    const float RequiredWalkableZ = 0.55f;

    for (const FExitCandidate& Candidate : Candidates)
    {
        const FVector TraceStart(Candidate.Location.X, Candidate.Location.Y, TraceTopZ);
        const FVector TraceEnd(Candidate.Location.X, Candidate.Location.Y, TraceBottomZ);

        FHitResult GroundHit;
        if (!World->LineTraceSingleByChannel(GroundHit, TraceStart, TraceEnd, ECC_Visibility, QueryParams))
        {
            continue;
        }

        if (!GroundHit.bBlockingHit || GroundHit.ImpactNormal.Z < RequiredWalkableZ)
        {
            continue;
        }

        const FVector CandidateActorLocation = GroundHit.ImpactPoint + FVector::UpVector * (CapsuleHalfHeight + 2.0f);
        const FQuat CandidateRotation = OutRotation.Quaternion();
        if (World->OverlapBlockingTestByChannel(CandidateActorLocation, CandidateRotation, ECC_Pawn, CapsuleShape, QueryParams))
        {
            continue;
        }

        // A short downward sweep confirms that the capsule can actually settle on the traced floor
        // instead of being placed on the far side of a thin one-sided surface.
        FHitResult FloorSweep;
        const FVector SweepStart = CandidateActorLocation + FVector::UpVector * 5.0f;
        const FVector SweepEnd = CandidateActorLocation - FVector::UpVector * 10.0f;
        if (World->SweepSingleByChannel(FloorSweep, SweepStart, SweepEnd, CandidateRotation, ECC_Pawn, CapsuleShape, QueryParams)
            && FloorSweep.bBlockingHit
            && FloorSweep.ImpactNormal.Z < RequiredWalkableZ)
        {
            continue;
        }

        OutLocation = CandidateActorLocation;
        return true;
    }

    return false;
}

void ARuntimeVehiclePawn::ApplyChassisClearanceProtection(UWorld* World, const FTransform& BodyTransform, const FCollisionQueryParams& QueryParams)
{
    if (!World || !IsValid(Body) || MaxChassisAntiGroundStickForce <= 0.0f || ChassisAntiGroundStickStrength <= 0.0f)
    {
        return;
    }

    const float MassScale = GetVehicleMassScale();
    const float DesiredClearance = FMath::Max(1.0f, MinimumBodyGroundClearance);
    const float Strength = FMath::Max(0.0f, ChassisAntiGroundStickStrength) * MassScale;
    const float Damping = FMath::Max(0.0f, ChassisAntiGroundStickDamping) * MassScale;
    const float MaxForcePerPoint = FMath::Max(1.0f, MaxChassisAntiGroundStickForce * MassScale / 5.0f);

    const FVector LocalBottomPoints[] = {
        FVector(BodyExtent.X * 0.82f, BodyExtent.Y * 0.82f, -BodyExtent.Z),
        FVector(BodyExtent.X * 0.82f, -BodyExtent.Y * 0.82f, -BodyExtent.Z),
        FVector(-BodyExtent.X * 0.82f, BodyExtent.Y * 0.82f, -BodyExtent.Z),
        FVector(-BodyExtent.X * 0.82f, -BodyExtent.Y * 0.82f, -BodyExtent.Z),
        FVector(0.0f, 0.0f, -BodyExtent.Z)
    };

    for (const FVector& LocalPoint : LocalBottomPoints)
    {
        const FVector BottomWorld = BodyTransform.TransformPosition(LocalPoint);
        const FVector TraceStart = BottomWorld + FVector::UpVector * 30.0f;
        const FVector TraceEnd = BottomWorld - FVector::UpVector * (DesiredClearance + 100.0f);

        FHitResult Hit;
        if (!World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, QueryParams))
        {
            continue;
        }

        const float CurrentClearance = FVector::DotProduct(BottomWorld - Hit.ImpactPoint, FVector::UpVector);
        const float ClearanceError = DesiredClearance - CurrentClearance;
        if (ClearanceError <= 0.0f)
        {
            continue;
        }

        const float VerticalSpeedAtPoint = FVector::DotProduct(Body->GetPhysicsLinearVelocityAtPoint(BottomWorld), FVector::UpVector);
        const float LiftForce = FMath::Clamp(ClearanceError * Strength - VerticalSpeedAtPoint * Damping, 0.0f, MaxForcePerPoint);
        if (LiftForce > KINDA_SMALL_NUMBER)
        {
            Body->AddForceAtLocation(FVector::UpVector * LiftForce, BottomWorld);
        }
    }
}

void ARuntimeVehiclePawn::ApplySuspensionAndDrive(float DeltaSeconds)
{
    if (!IsValid(Body) || !Body->IsSimulatingPhysics())
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World || DeltaSeconds <= 0.0f)
    {
        return;
    }

    const FTransform BodyTransform = Body->GetComponentTransform();
    const FVector Up = Body->GetUpVector().GetSafeNormal();
    const FVector Forward = Body->GetForwardVector().GetSafeNormal();
    const FVector BodyVelocity = Body->GetPhysicsLinearVelocity();
    const FVector AngularVelocity = Body->GetPhysicsAngularVelocityInRadians();
    const float BodyForwardSpeed = FVector::DotProduct(BodyVelocity, Forward);
    const float AbsBodyForwardSpeed = FMath::Abs(BodyForwardSpeed);
    const float SafeWheelRadius = FMath::Max(1.0f, WheelRadius);
    const float MassScale = GetVehicleMassScale();

    WheelGrounded.SetNum(WheelOffsets.Num());
    WheelSpringLengths.SetNum(WheelOffsets.Num());

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(RuntimeVehicleSuspension), false, this);
    QueryParams.AddIgnoredActor(this);

    ApplyChassisClearanceProtection(World, BodyTransform, QueryParams);

    // Center clearance guard: only lifts when the chassis is actually below the intended ride height.
    // It is deliberately capped below suspension capacity so it prevents scraping without making the car float.
    if (MaxChassisAntiGroundStickForce > 0.0f && ChassisAntiGroundStickStrength > 0.0f)
    {
        const FVector BodyLocation = Body->GetComponentLocation();
        const float DesiredCenterHeight = GetDesiredCenterHeightAboveGround();
        const FVector ClearanceTraceStart = BodyLocation + FVector(0.0f, 0.0f, 80.0f);
        const FVector ClearanceTraceEnd = BodyLocation - FVector(0.0f, 0.0f, DesiredCenterHeight + 260.0f);
        FHitResult ClearanceHit;
        if (World->LineTraceSingleByChannel(ClearanceHit, ClearanceTraceStart, ClearanceTraceEnd, ECC_Visibility, QueryParams))
        {
            const float CurrentCenterHeight = BodyLocation.Z - ClearanceHit.ImpactPoint.Z;
            const float HeightError = DesiredCenterHeight - CurrentCenterHeight;
            if (HeightError > 1.0f)
            {
                const float VerticalSpeed = FVector::DotProduct(BodyVelocity, FVector::UpVector);
                const float LiftForce = FMath::Clamp(HeightError * ChassisAntiGroundStickStrength * MassScale - VerticalSpeed * ChassisAntiGroundStickDamping * MassScale, 0.0f, MaxChassisAntiGroundStickForce * MassScale);
                Body->AddForce(FVector::UpVector * LiftForce);
            }
        }
    }

    struct FRuntimeVehicleWheelState
    {
        int32 Index = INDEX_NONE;
        FVector LocalOffset = FVector::ZeroVector;
        FVector MountWorld = FVector::ZeroVector;
        FVector ContactWorld = FVector::ZeroVector;
        FVector WheelForward = FVector::ForwardVector;
        FVector WheelRight = FVector::RightVector;
        bool bGrounded = false;
        bool bFront = false;
        bool bRightSide = false;
        float SpringLength = 0.0f;
        float Compression = 0.0f;
        float NormalForce = 0.0f;
        float ForwardSpeed = 0.0f;
        float LateralSpeed = 0.0f;
    };

    TArray<FRuntimeVehicleWheelState> WheelStates;
    WheelStates.SetNum(WheelOffsets.Num());

    float FrontMostWheelX = WheelOffsets.Num() > 0 ? WheelOffsets[0].X : 0.0f;
    float RearMostWheelX = FrontMostWheelX;
    for (const FVector& Offset : WheelOffsets)
    {
        FrontMostWheelX = FMath::Max(FrontMostWheelX, Offset.X);
        RearMostWheelX = FMath::Min(RearMostWheelX, Offset.X);
    }
    const float AxleSplitX = (FrontMostWheelX + RearMostWheelX) * 0.5f;
    const float Wheelbase = FMath::Max(80.0f, FrontMostWheelX - RearMostWheelX);

    const float SteeringSpeedAlphaRaw = FMath::Clamp(AbsBodyForwardSpeed / FMath::Max(100.0f, SteeringSpeedForFullAssist), 0.0f, 1.0f);
    const float SteeringSpeedAlpha = SteeringSpeedAlphaRaw * SteeringSpeedAlphaRaw * (3.0f - 2.0f * SteeringSpeedAlphaRaw);
    const float EffectiveMaxSteeringDegrees = FMath::Lerp(MaxSteeringAngleDegrees, HighSpeedSteeringAngleDegrees, SteeringSpeedAlpha);
    const float BaseSteeringAngle = FMath::DegreesToRadians(EffectiveMaxSteeringDegrees * SmoothedSteeringInput);
    const float HighSpeedGripAlphaRaw = FMath::Clamp(AbsBodyForwardSpeed / FMath::Max(100.0f, HighSpeedLateralGripSpeed), 0.0f, 1.0f);
    const float HighSpeedGripAlpha = HighSpeedGripAlphaRaw * HighSpeedGripAlphaRaw * (3.0f - 2.0f * HighSpeedGripAlphaRaw);
    const float SpeedLateralGripScale = FMath::Lerp(1.0f, FMath::Clamp(HighSpeedLateralGripScale, 0.1f, 1.0f), HighSpeedGripAlpha);

    int32 GroundedWheels = 0;
    int32 GroundedFrontWheels = 0;
    int32 GroundedRearWheels = 0;

    for (int32 WheelIndex = 0; WheelIndex < WheelOffsets.Num(); ++WheelIndex)
    {
        FRuntimeVehicleWheelState& WheelState = WheelStates[WheelIndex];
        WheelState.Index = WheelIndex;
        WheelState.LocalOffset = WheelOffsets[WheelIndex];
        WheelState.bFront = WheelState.LocalOffset.X >= AxleSplitX;
        WheelState.bRightSide = WheelState.LocalOffset.Y > 0.0f;
        WheelGrounded[WheelIndex] = false;
        WheelSpringLengths[WheelIndex] = SuspensionRestLength;

        const FVector MountWorld = BodyTransform.TransformPosition(WheelState.LocalOffset);
        const FVector TraceStart = MountWorld + Up * FMath::Max(2.0f, SafeWheelRadius * 0.20f);
        const FVector TraceEnd = MountWorld - Up * (SuspensionRestLength + SuspensionTraceExtra + SafeWheelRadius);
        FHitResult Hit;
        if (!World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, QueryParams))
        {
            WheelState.SpringLength = SuspensionRestLength;
            WheelState.MountWorld = MountWorld;
            continue;
        }

        const float HitDistance = (Hit.ImpactPoint - MountWorld).Size();
        const float SpringLength = FMath::Clamp(HitDistance - SafeWheelRadius, 0.0f, SuspensionRestLength);
        const float Compression = FMath::Max(0.0f, SuspensionRestLength - SpringLength);
        const FVector PointVelocity = Body->GetPhysicsLinearVelocityAtPoint(MountWorld);
        const float SuspensionVelocity = FVector::DotProduct(PointVelocity, Up);
        float SuspensionForce = Compression * SuspensionStrength * MassScale - SuspensionVelocity * SuspensionDamping * MassScale;
        SuspensionForce = FMath::Clamp(SuspensionForce, 0.0f, MaxSuspensionForcePerWheel * MassScale);

        if (SpringLength < 3.0f)
        {
            const float BumpStopForce = (3.0f - SpringLength) * SuspensionStrength * MassScale * 1.75f;
            SuspensionForce = FMath::Clamp(SuspensionForce + BumpStopForce, 0.0f, MaxSuspensionForcePerWheel * MassScale);
        }

        Body->AddForceAtLocation(Up * SuspensionForce, MountWorld);

        float WheelSteerAngle = 0.0f;
        if (WheelState.bFront)
        {
            WheelSteerAngle = BaseSteeringAngle;
            if (!FMath::IsNearlyZero(WheelSteerAngle, 0.001f) && AckermannStrength > 0.0f)
            {
                const float TurnSign = FMath::Sign(WheelSteerAngle);
                const float HalfTrack = FMath::Max(20.0f, FMath::Abs(WheelState.LocalOffset.Y));
                const float BaseTurnRadius = Wheelbase / FMath::Max(0.05f, FMath::Tan(FMath::Abs(WheelSteerAngle)));
                const bool bInnerWheel = TurnSign * WheelState.LocalOffset.Y > 0.0f;
                const float AdjustedRadius = FMath::Max(50.0f, BaseTurnRadius + (bInnerWheel ? -HalfTrack : HalfTrack));
                const float AckermannAngle = TurnSign * FMath::Atan(Wheelbase / AdjustedRadius);
                WheelSteerAngle = FMath::Lerp(WheelSteerAngle, AckermannAngle, FMath::Clamp(AckermannStrength, 0.0f, 1.0f));
            }
        }

        const FQuat SteerQuat(Up, WheelSteerAngle);
        const FVector WheelForward = SteerQuat.RotateVector(Forward).GetSafeNormal();
        const FVector WheelRight = FVector::CrossProduct(Up, WheelForward).GetSafeNormal();
        const FVector ContactVelocity = Body->GetPhysicsLinearVelocityAtPoint(Hit.ImpactPoint);

        WheelState.MountWorld = MountWorld;
        WheelState.ContactWorld = Hit.ImpactPoint;
        WheelState.WheelForward = WheelForward;
        WheelState.WheelRight = WheelRight;
        WheelState.bGrounded = true;
        WheelState.SpringLength = SpringLength;
        WheelState.Compression = Compression;
        WheelState.NormalForce = SuspensionForce;
        WheelState.ForwardSpeed = FVector::DotProduct(ContactVelocity, WheelForward);
        WheelState.LateralSpeed = FVector::DotProduct(ContactVelocity, WheelRight);

        WheelGrounded[WheelIndex] = true;
        WheelSpringLengths[WheelIndex] = SpringLength;
        ++GroundedWheels;
        if (WheelState.bFront)
        {
            ++GroundedFrontWheels;
        }
        else
        {
            ++GroundedRearWheels;
        }
    }

    auto ApplyAntiRollForAxle = [&](bool bFrontAxle)
    {
        FRuntimeVehicleWheelState* LeftWheel = nullptr;
        FRuntimeVehicleWheelState* RightWheel = nullptr;
        for (FRuntimeVehicleWheelState& WheelState : WheelStates)
        {
            if (!WheelState.bGrounded || WheelState.bFront != bFrontAxle)
            {
                continue;
            }

            if (WheelState.bRightSide)
            {
                RightWheel = &WheelState;
            }
            else
            {
                LeftWheel = &WheelState;
            }
        }

        if (!LeftWheel || !RightWheel)
        {
            return;
        }

        const float CompressionDifference = RightWheel->Compression - LeftWheel->Compression;
        const float AntiRollForce = FMath::Clamp(CompressionDifference * AntiRollBarStiffness * MassScale, -MaxAntiRollForce * MassScale, MaxAntiRollForce * MassScale);
        if (!FMath::IsNearlyZero(AntiRollForce, 1.0f))
        {
            Body->AddForceAtLocation(Up * AntiRollForce, RightWheel->MountWorld);
            Body->AddForceAtLocation(-Up * AntiRollForce, LeftWheel->MountWorld);
        }
    };

    ApplyAntiRollForAxle(true);
    ApplyAntiRollForAxle(false);

    const float FrontDriveShare = FMath::Clamp(DrivenFrontTorqueShare, 0.0f, 1.0f);
    const float RearDriveShare = 1.0f - FrontDriveShare;
    const float AbsThrottle = FMath::Abs(SmoothedThrottleInput);
    const float SafeSpeedLimit = FMath::Max(500.0f, MaxSpeedForward);
    const float SpeedLimitAlpha = FMath::Clamp((AbsBodyForwardSpeed - SafeSpeedLimit * 0.92f) / FMath::Max(1.0f, SafeSpeedLimit * 0.08f), 0.0f, 1.0f);
    const bool bAcceleratingTowardLimit = !FMath::IsNearlyZero(SmoothedThrottleInput, 0.01f) && FMath::Sign(SmoothedThrottleInput) == FMath::Sign(BodyForwardSpeed);
    const float SpeedLimiter = bAcceleratingTowardLimit ? (1.0f - SpeedLimitAlpha) : 1.0f;

    for (const FRuntimeVehicleWheelState& WheelState : WheelStates)
    {
        if (!WheelState.bGrounded || WheelState.NormalForce <= KINDA_SMALL_NUMBER)
        {
            continue;
        }

        const int32 GroundedAxleCount = WheelState.bFront ? FMath::Max(1, GroundedFrontWheels) : FMath::Max(1, GroundedRearWheels);
        const float AxleDriveShare = WheelState.bFront ? FrontDriveShare : RearDriveShare;
        const bool bThrottleIsBrake = AbsThrottle > 0.05f && FMath::Abs(WheelState.ForwardSpeed) > 120.0f && FMath::Sign(SmoothedThrottleInput) != FMath::Sign(WheelState.ForwardSpeed);

        float LongitudinalDemand = 0.0f;
        if (bThrottleIsBrake)
        {
            LongitudinalDemand = -FMath::Sign(WheelState.ForwardSpeed) * BrakeForce * MassScale * AbsThrottle / static_cast<float>(FMath::Max(1, GroundedWheels));
        }
        else if (AbsThrottle > 0.02f)
        {
            const float DriveMagnitude = (SmoothedThrottleInput >= 0.0f ? EngineForce : ReverseForce) * MassScale;
            LongitudinalDemand = SmoothedThrottleInput * DriveMagnitude * AxleDriveShare * SpeedLimiter / static_cast<float>(GroundedAxleCount);
        }
        else if (FMath::Abs(WheelState.ForwardSpeed) > 25.0f)
        {
            LongitudinalDemand = -FMath::Sign(WheelState.ForwardSpeed) * EngineBrakingForce * MassScale / static_cast<float>(FMath::Max(1, GroundedWheels));
        }

        if (FMath::Abs(WheelState.ForwardSpeed) > 15.0f && RollingResistance > 0.0f)
        {
            LongitudinalDemand += -FMath::Sign(WheelState.ForwardSpeed) * WheelState.NormalForce * RollingResistance;
        }

        const float LongitudinalLimit = FMath::Max(1.0f, WheelState.NormalForce * TireLongitudinalFriction);
        float LongitudinalForce = FMath::Clamp(LongitudinalDemand, -LongitudinalLimit, LongitudinalLimit);

        const float SlipReferenceSpeed = FMath::Max(1.0f, TireSlipReferenceSpeed + FMath::Abs(WheelState.ForwardSpeed) * 0.18f);
        const float SlipAngle = FMath::Atan2(WheelState.LateralSpeed, SlipReferenceSpeed + FMath::Abs(WheelState.ForwardSpeed));
        const float SteeringGripMultiplier = WheelState.bFront ? FMath::Max(0.1f, FrontSteeringGripMultiplier) : FMath::Max(0.1f, RearSteeringGripMultiplier);
        const float CorneringStiffness = FMath::Max(0.1f, TireCorneringStiffness) * FMath::Max(0.1f, LateralGrip) * SteeringGripMultiplier * SpeedLateralGripScale;
        const float LateralDemand = -SlipAngle * CorneringStiffness * WheelState.NormalForce;
        const float LateralLimitBase = FMath::Min(WheelState.NormalForce * TireLateralFriction, MaxLateralGripForce * MassScale) * SpeedLateralGripScale;
        const float LongitudinalUsage = FMath::Clamp(FMath::Abs(LongitudinalForce) / LongitudinalLimit, 0.0f, 1.0f);
        const float LateralLimit = LateralLimitBase * FMath::Sqrt(FMath::Max(0.0f, 1.0f - LongitudinalUsage * LongitudinalUsage));
        const float LateralForce = FMath::Clamp(LateralDemand, -LateralLimit, LateralLimit);

        const FVector CenterOfMassWorld = Body->GetCenterOfMass();
        const float HeightToCenter = FVector::DotProduct(CenterOfMassWorld - WheelState.ContactWorld, Up);
        const FVector CenterHeightLocation = WheelState.ContactWorld + Up * HeightToCenter;
        const FVector LongitudinalForceLocation = FMath::Lerp(WheelState.ContactWorld, CenterHeightLocation, FMath::Clamp(DriveForceCenterOfMassHeightBlend, 0.0f, 1.0f));
        const FVector LateralForceLocation = FMath::Lerp(WheelState.ContactWorld, CenterHeightLocation, FMath::Clamp(LateralForceCenterOfMassHeightBlend, 0.0f, 1.0f));

        if (!FMath::IsNearlyZero(LongitudinalForce, 1.0f))
        {
            Body->AddForceAtLocation(WheelState.WheelForward * LongitudinalForce, LongitudinalForceLocation);
        }
        if (!FMath::IsNearlyZero(LateralForce, 1.0f))
        {
            Body->AddForceAtLocation(WheelState.WheelRight * LateralForce, LateralForceLocation);
        }
    }

    ApplyAeroDownforce(GroundedWheels);
    ApplyPitchStabilization(GroundedWheels);
    ApplyRollStabilization(GroundedWheels);

    if (GroundedWheels > 0)
    {
        const float GroundedRatio = static_cast<float>(GroundedWheels) / static_cast<float>(FMath::Max(1, WheelOffsets.Num()));
        const float YawRate = FVector::DotProduct(AngularVelocity, Up);
        const float DesiredYawRate = BodyForwardSpeed * FMath::Tan(BaseSteeringAngle) / FMath::Max(80.0f, Wheelbase);
        const float YawAssistTorque = (DesiredYawRate - YawRate) * SteeringYawRateAssist * MassScale * GroundedRatio;
        const float YawDampingTorque = -YawRate * SteeringYawDamping * MassScale * GroundedRatio;
        const float SteeringTotalTorque = FMath::Clamp(YawAssistTorque + YawDampingTorque, -MaxSteeringAssistTorque * MassScale, MaxSteeringAssistTorque * MassScale);
        if (!FMath::IsNearlyZero(SteeringTotalTorque, 1.0f))
        {
            Body->AddTorqueInRadians(Up * SteeringTotalTorque);
        }

        const FVector CurrentUp = Body->GetUpVector();
        const FVector UprightAxis = FVector::CrossProduct(CurrentUp, FVector::UpVector);
        const FVector WorldYawAngularVelocity = FVector::UpVector * FVector::DotProduct(AngularVelocity, FVector::UpVector);
        const FVector NonYawAngularVelocity = AngularVelocity - WorldYawAngularVelocity;
        const FVector UprightTorque = (UprightAxis * UprightTorqueStrength * MassScale - NonYawAngularVelocity * UprightTorqueDamping * MassScale).GetClampedToMaxSize(FMath::Max(1.0f, UprightTorqueStrength * MassScale * 1.5f));
        Body->AddTorqueInRadians(UprightTorque);
    }
    else
    {
        const FVector AirDampingTorque = (-AngularVelocity * FMath::Max(0.0f, AirborneAngularDampingTorque) * MassScale)
            .GetClampedToMaxSize(FMath::Max(1.0f, MaxAirborneAngularDampingTorque * MassScale));
        Body->AddTorqueInRadians(AirDampingTorque);
    }
}

void ARuntimeVehiclePawn::ApplyAeroDownforce(int32 GroundedWheels)
{
    if (!IsValid(Body) || !Body->IsSimulatingPhysics())
    {
        return;
    }

    const FVector LinearVelocity = Body->GetPhysicsLinearVelocity();
    const float Speed = LinearVelocity.Size();
    const bool bGrounded = GroundedWheels > 0;
    const float ClearanceScale = GetDownforceClearanceScale();
    const float MassScale = GetVehicleMassScale();

    if (Speed > 50.0f && MaxAerodynamicDrag > 0.0f && AerodynamicDragCoefficient > 0.0f)
    {
        const float DragMagnitude = FMath::Clamp(Speed * Speed * FMath::Max(0.0f, AerodynamicDragCoefficient) * MassScale, 0.0f, MaxAerodynamicDrag * MassScale);
        Body->AddForce(-LinearVelocity.GetSafeNormal() * DragMagnitude);
    }

    if (Speed >= MinimumDownforceSpeed)
    {
        const float Coefficient = bGrounded ? GroundedDownforceCoefficient : AirborneDownforceCoefficient;
        const float MaxForce = (bGrounded ? MaxGroundedDownforce : MaxAirborneDownforce) * MassScale;
        if (Coefficient > 0.0f && MaxForce > 0.0f)
        {
            const FVector BodyUp = Body->GetUpVector();
            const float UprightAlpha = FMath::Clamp(FVector::DotProduct(BodyUp, FVector::UpVector), 0.0f, 1.0f);
            const FVector ChassisDown = (-BodyUp).GetSafeNormal();
            const FVector DownDirection = FMath::Lerp(-FVector::UpVector, ChassisDown, UprightAlpha).GetSafeNormal();
            const float Downforce = FMath::Clamp(Speed * Speed * Coefficient * MassScale, 0.0f, MaxForce) * ClearanceScale;

            if (Downforce > KINDA_SMALL_NUMBER)
            {
                Body->AddForce(DownDirection * Downforce);
            }
        }
    }

    if (bGrounded && MaxFrontDownforce > 0.0f)
    {
        const float ForwardSpeed = FMath::Abs(FVector::DotProduct(LinearVelocity, Body->GetForwardVector()));
        const float SpeedFrontForce = ForwardSpeed * ForwardSpeed * FMath::Max(0.0f, FrontDownforceCoefficient) * MassScale;
        const float ThrottleFrontForce = FMath::Max(0.0f, SmoothedThrottleInput) * FMath::Max(0.0f, ThrottleFrontDownforce) * MassScale;
        const float GroundedRatio = static_cast<float>(GroundedWheels) / static_cast<float>(FMath::Max(1, WheelOffsets.Num()));
        const float FrontForce = FMath::Clamp((SpeedFrontForce + ThrottleFrontForce) * FMath::Clamp(GroundedRatio, 0.25f, 1.0f), 0.0f, MaxFrontDownforce * MassScale) * ClearanceScale;

        if (FrontForce > KINDA_SMALL_NUMBER)
        {
            Body->AddForceAtLocation(-FVector::UpVector * FrontForce, GetFrontAxleForceLocation());
        }
    }
}

FVector ARuntimeVehiclePawn::GetFrontAxleForceLocation() const
{
    if (!IsValid(Body))
    {
        return GetActorLocation() + GetActorForwardVector() * BodyExtent.X * 0.75f;
    }

    FVector LocalFrontOffset(BodyExtent.X * 0.75f, 0.0f, 0.0f);
    float FrontX = 0.0f;
    bool bHasWheelOffset = false;
    for (const FVector& Offset : WheelOffsets)
    {
        FrontX = bHasWheelOffset ? FMath::Max(FrontX, Offset.X) : Offset.X;
        bHasWheelOffset = true;
    }

    if (bHasWheelOffset)
    {
        FVector Sum = FVector::ZeroVector;
        int32 Count = 0;
        const float FrontBand = FMath::Max(8.0f, BodyExtent.X * 0.15f);
        for (const FVector& Offset : WheelOffsets)
        {
            if (Offset.X >= FrontX - FrontBand)
            {
                Sum += Offset;
                ++Count;
            }
        }

        if (Count > 0)
        {
            LocalFrontOffset = Sum / static_cast<float>(Count);
            LocalFrontOffset.Z = 0.0f;
        }
    }

    return Body->GetComponentTransform().TransformPosition(LocalFrontOffset);
}

void ARuntimeVehiclePawn::ApplyPitchStabilization(int32 GroundedWheels)
{
    if (!IsValid(Body) || !Body->IsSimulatingPhysics() || GroundedWheels <= 0 || MaxPitchStabilizationTorque <= 0.0f)
    {
        return;
    }

    const FVector Forward = Body->GetForwardVector();
    const FVector Right = Body->GetRightVector();
    const FVector AngularVelocity = Body->GetPhysicsAngularVelocityInRadians();
    const float NoseUpAmount = FVector::DotProduct(Forward, FVector::UpVector);
    const float PitchRate = FVector::DotProduct(AngularVelocity, Right);
    const float GroundedRatio = static_cast<float>(GroundedWheels) / static_cast<float>(FMath::Max(1, WheelOffsets.Num()));
    const float MassScale = GetVehicleMassScale();
    const float TorqueMagnitude = FMath::Clamp(
        NoseUpAmount * FMath::Max(0.0f, PitchStabilizationTorqueStrength) * MassScale - PitchRate * FMath::Max(0.0f, PitchStabilizationTorqueDamping) * MassScale,
        -MaxPitchStabilizationTorque * MassScale,
        MaxPitchStabilizationTorque * MassScale) * FMath::Clamp(GroundedRatio, 0.25f, 1.0f);

    if (!FMath::IsNearlyZero(TorqueMagnitude, 1.0f))
    {
        Body->AddTorqueInRadians(Right * TorqueMagnitude);
    }
}

void ARuntimeVehiclePawn::ApplyRollStabilization(int32 GroundedWheels)
{
    if (!IsValid(Body) || !Body->IsSimulatingPhysics() || GroundedWheels <= 0 || MaxRollStabilizationTorque <= 0.0f)
    {
        return;
    }

    const FVector Forward = Body->GetForwardVector();
    const FVector Right = Body->GetRightVector();
    const FVector AngularVelocity = Body->GetPhysicsAngularVelocityInRadians();
    const float RollError = FVector::DotProduct(Right, FVector::UpVector);
    const float RollRate = FVector::DotProduct(AngularVelocity, Forward);
    const float GroundedRatio = static_cast<float>(GroundedWheels) / static_cast<float>(FMath::Max(1, WheelOffsets.Num()));
    const float MassScale = GetVehicleMassScale();
    const float TorqueMagnitude = FMath::Clamp(
        -RollError * FMath::Max(0.0f, RollStabilizationTorqueStrength) * MassScale - RollRate * FMath::Max(0.0f, RollStabilizationTorqueDamping) * MassScale,
        -MaxRollStabilizationTorque * MassScale,
        MaxRollStabilizationTorque * MassScale) * FMath::Clamp(GroundedRatio, 0.25f, 1.0f);

    if (!FMath::IsNearlyZero(TorqueMagnitude, 1.0f))
    {
        Body->AddTorqueInRadians(Forward * TorqueMagnitude);
    }
}

float ARuntimeVehiclePawn::GetDesiredCenterHeightAboveGround() const
{
    float LowestMountOffsetZ = 0.0f;
    if (WheelOffsets.Num() > 0)
    {
        LowestMountOffsetZ = WheelOffsets[0].Z;
        for (const FVector& Offset : WheelOffsets)
        {
            LowestMountOffsetZ = FMath::Min(LowestMountOffsetZ, Offset.Z);
        }
    }

    const float StaticSpringLength = SuspensionRestLength * FMath::Clamp(LoadedWheelVisualRestLengthRatio, 0.2f, 0.95f);
    const float WheelRideHeight = WheelRadius + StaticSpringLength - LowestMountOffsetZ;
    const float ChassisRideHeight = BodyExtent.Z + MinimumBodyGroundClearance;
    return FMath::Max(ChassisRideHeight, WheelRideHeight);
}

float ARuntimeVehiclePawn::GetDownforceClearanceScale() const
{
    if (!IsValid(Body))
    {
        return 1.0f;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return 1.0f;
    }

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(RuntimeVehicleDownforceClearance), false, this);
    QueryParams.AddIgnoredActor(this);

    const FVector BodyLocation = Body->GetComponentLocation();
    const float DesiredCenterHeight = GetDesiredCenterHeightAboveGround();
    const float TraceUp = FMath::Max(80.0f, BodyExtent.Z + 80.0f);
    const FVector Start = BodyLocation + FVector::UpVector * TraceUp;
    const FVector End = BodyLocation - FVector::UpVector * (DesiredCenterHeight + TraceUp + 160.0f);

    FHitResult Hit;
    if (!World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, QueryParams))
    {
        return 1.0f;
    }

    const float CurrentCenterHeight = BodyLocation.Z - Hit.ImpactPoint.Z;
    const float FadeRange = FMath::Max(12.0f, MinimumBodyGroundClearance);
    const float LowHeight = DesiredCenterHeight - FadeRange;
    return FMath::Clamp((CurrentCenterHeight - LowHeight) / FadeRange, 0.0f, 1.0f);
}

void ARuntimeVehiclePawn::RestoreStoredPawnCamera(APlayerController* PlayerController, APawn* PawnToRestore) const
{
    if (!bResetCharacterCameraOnExit || !IsValid(PlayerController) || !IsValid(PawnToRestore))
    {
        return;
    }

    // Do not keep the vehicle camera's spring-arm pitch/roll after repossessing the character.
    // Align yaw to the restored character and reset pitch/roll to the normal character camera state.
    FRotator CleanCharacterRotation = PawnToRestore->GetActorRotation();
    CleanCharacterRotation.Pitch = 0.0f;
    CleanCharacterRotation.Roll = 0.0f;

    PlayerController->SetViewTarget(PawnToRestore);
    PlayerController->SetControlRotation(CleanCharacterRotation);
    PlayerController->ClientSetRotation(CleanCharacterRotation, true);
}

void ARuntimeVehiclePawn::UpdateWheelVisuals(float DeltaSeconds)
{
    const FTransform BodyTransform = Body ? Body->GetComponentTransform() : GetActorTransform();
    const FVector Forward = Body ? Body->GetForwardVector() : GetActorForwardVector();
    const float SafeWheelRadius = FMath::Max(1.0f, WheelRadius);
    const float AbsForwardSpeed = Body ? FMath::Abs(FVector::DotProduct(Body->GetPhysicsLinearVelocity(), Forward)) : 0.0f;
    const float SteerAlphaRaw = FMath::Clamp(AbsForwardSpeed / FMath::Max(1.0f, SteeringSpeedForFullAssist), 0.0f, 1.0f);
    const float SteerAlpha = SteerAlphaRaw * SteerAlphaRaw * (3.0f - 2.0f * SteerAlphaRaw);
    const float SteeringDegrees = SmoothedSteeringInput * FMath::Lerp(MaxSteeringAngleDegrees, HighSpeedSteeringAngleDegrees, SteerAlpha);
    WheelSpinDegrees.SetNum(WheelOffsets.Num());

    auto CalculateWheelVisual = [&](int32 WheelIndex, FVector& OutLocalCenter, FRotator& OutRelativeRotation)
    {
        const FVector MountWorld = BodyTransform.TransformPosition(WheelOffsets[WheelIndex]);
        const float SpringLength = WheelSpringLengths.IsValidIndex(WheelIndex) ? WheelSpringLengths[WheelIndex] : SuspensionRestLength * FMath::Clamp(LoadedWheelVisualRestLengthRatio, 0.2f, 0.95f);
        const FVector WheelCenterWorld = MountWorld - BodyTransform.GetUnitAxis(EAxis::Z) * SpringLength;
        OutLocalCenter = BodyTransform.InverseTransformPosition(WheelCenterWorld);
        const FVector Velocity = Body ? Body->GetPhysicsLinearVelocityAtPoint(WheelCenterWorld) : FVector::ZeroVector;
        const float ForwardSpeed = FVector::DotProduct(Velocity, Forward);
        WheelSpinDegrees[WheelIndex] += FMath::RadiansToDegrees((ForwardSpeed / SafeWheelRadius) * DeltaSeconds);

        const bool bFrontWheel = WheelOffsets[WheelIndex].X > 0.0f;
        OutRelativeRotation = FRotator(WheelSpinDegrees[WheelIndex], bFrontWheel ? SteeringDegrees : 0.0f, 0.0f);
    };

    for (int32 WheelIndex = 0; WheelIndex < WheelMeshes.Num() && WheelIndex < WheelOffsets.Num(); ++WheelIndex)
    {
        UProceduralMeshComponent* WheelMesh = WheelMeshes[WheelIndex];
        if (!IsValid(WheelMesh))
        {
            continue;
        }

        FVector LocalWheelCenter;
        FRotator RelativeRotation;
        CalculateWheelVisual(WheelIndex, LocalWheelCenter, RelativeRotation);
        WheelMesh->SetRelativeLocation(LocalWheelCenter);
        WheelMesh->SetRelativeRotation(RelativeRotation);
    }

    for (int32 WheelIndex = 0; WheelIndex < LoadedWheelMeshComponents.Num() && WheelIndex < WheelOffsets.Num(); ++WheelIndex)
    {
        UStaticMeshComponent* WheelMeshComponent = LoadedWheelMeshComponents[WheelIndex];
        if (!IsValid(WheelMeshComponent))
        {
            continue;
        }

        FVector LocalWheelCenter;
        FRotator RelativeRotation;
        CalculateWheelVisual(WheelIndex, LocalWheelCenter, RelativeRotation);
        const FQuat BaseRotation = LoadedWheelBaseRotations.IsValidIndex(WheelIndex) ? LoadedWheelBaseRotations[WheelIndex] : FQuat::Identity;
        WheelMeshComponent->SetRelativeLocation(LocalWheelCenter);
        WheelMeshComponent->SetRelativeRotation((RelativeRotation.Quaternion() * BaseRotation).Rotator());
    }
}

void ARuntimeVehiclePawn::BuildBodyMesh()
{
    if (!IsValid(BodyMesh))
    {
        return;
    }

    const FVector E = BodyExtent;
    TArray<FVector> Vertices = {
        FVector(E.X, E.Y, E.Z), FVector(E.X, -E.Y, E.Z), FVector(-E.X, -E.Y, E.Z), FVector(-E.X, E.Y, E.Z),
        FVector(E.X, E.Y, -E.Z), FVector(E.X, -E.Y, -E.Z), FVector(-E.X, -E.Y, -E.Z), FVector(-E.X, E.Y, -E.Z)
    };
    TArray<int32> Triangles = {
        0, 1, 2, 0, 2, 3,
        4, 6, 5, 4, 7, 6,
        0, 4, 5, 0, 5, 1,
        1, 5, 6, 1, 6, 2,
        2, 6, 7, 2, 7, 3,
        3, 7, 4, 3, 4, 0
    };
    TArray<FVector> Normals;
    Normals.Init(FVector::UpVector, Vertices.Num());
    TArray<FVector2D> UV0;
    UV0.Init(FVector2D::ZeroVector, Vertices.Num());
    TArray<FColor> Colors;
    Colors.Init(FColor(80, 80, 85), Vertices.Num());
    TArray<FProcMeshTangent> Tangents;
    Tangents.Init(FProcMeshTangent(1.0f, 0.0f, 0.0f), Vertices.Num());
    BodyMesh->CreateMeshSection(0, Vertices, Triangles, Normals, UV0, Colors, Tangents, false);
}

void ARuntimeVehiclePawn::BuildWheelMeshes()
{
    for (UProceduralMeshComponent* WheelMesh : WheelMeshes)
    {
        BuildWheelMesh(WheelMesh);
    }
}

void ARuntimeVehiclePawn::BuildWheelMesh(UProceduralMeshComponent* MeshComponent) const
{
    if (!IsValid(MeshComponent))
    {
        return;
    }

    constexpr int32 Segments = 20;
    const float Radius = FMath::Max(4.0f, WheelRadius);
    const float HalfWidth = FMath::Max(2.0f, WheelWidth * 0.5f);
    TArray<FVector> Vertices;
    TArray<int32> Triangles;
    TArray<FVector> Normals;
    TArray<FVector2D> UV0;
    TArray<FColor> Colors;

    for (int32 Index = 0; Index < Segments; ++Index)
    {
        const float Angle = 2.0f * PI * static_cast<float>(Index) / static_cast<float>(Segments);
        const float X = FMath::Cos(Angle) * Radius;
        const float Z = FMath::Sin(Angle) * Radius;
        Vertices.Add(FVector(X, -HalfWidth, Z));
        Vertices.Add(FVector(X, HalfWidth, Z));
        Normals.Add(FVector(X, 0.0f, Z).GetSafeNormal());
        Normals.Add(FVector(X, 0.0f, Z).GetSafeNormal());
        UV0.Add(FVector2D(static_cast<float>(Index) / Segments, 0.0f));
        UV0.Add(FVector2D(static_cast<float>(Index) / Segments, 1.0f));
        Colors.Add(FColor(18, 18, 18));
        Colors.Add(FColor(18, 18, 18));
    }

    const int32 LeftCenter = Vertices.Add(FVector(0.0f, -HalfWidth, 0.0f));
    const int32 RightCenter = Vertices.Add(FVector(0.0f, HalfWidth, 0.0f));
    Normals.Add(FVector(0.0f, -1.0f, 0.0f));
    Normals.Add(FVector(0.0f, 1.0f, 0.0f));
    UV0.Add(FVector2D(0.5f, 0.5f));
    UV0.Add(FVector2D(0.5f, 0.5f));
    Colors.Add(FColor(12, 12, 12));
    Colors.Add(FColor(12, 12, 12));

    for (int32 Index = 0; Index < Segments; ++Index)
    {
        const int32 Next = (Index + 1) % Segments;
        const int32 L0 = Index * 2;
        const int32 R0 = L0 + 1;
        const int32 L1 = Next * 2;
        const int32 R1 = L1 + 1;

        Triangles.Add(L0);
        Triangles.Add(R0);
        Triangles.Add(R1);
        Triangles.Add(L0);
        Triangles.Add(R1);
        Triangles.Add(L1);
        Triangles.Add(LeftCenter);
        Triangles.Add(L1);
        Triangles.Add(L0);
        Triangles.Add(RightCenter);
        Triangles.Add(R0);
        Triangles.Add(R1);
    }

    TArray<FProcMeshTangent> Tangents;
    Tangents.Init(FProcMeshTangent(1.0f, 0.0f, 0.0f), Vertices.Num());
    MeshComponent->CreateMeshSection(0, Vertices, Triangles, Normals, UV0, Colors, Tangents, false);
}
