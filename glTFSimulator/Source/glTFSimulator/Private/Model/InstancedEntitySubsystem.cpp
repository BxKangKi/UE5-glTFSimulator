// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "Model/InstancedEntitySubsystem.h"

#include "Async/ParallelFor.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Model/InstancedEntityRenderActor.h"
#include "System/GlbValidation.h"
#include "System/GameUpdateSubSystem.h"

namespace
{
    constexpr int32 ParallelInterpolationThreshold = 64;
    constexpr float PhysicsReactivateHysteresis = 0.85f;

    struct FEntityInterpolationWorkItem
    {
        int32 RegistrationId = INDEX_NONE;
        int32 PartIndex = INDEX_NONE;
        FTransform CurrentTransform = FTransform::Identity;
        FTransform TargetTransform = FTransform::Identity;
        FTransform ResultTransform = FTransform::Identity;
        float DeltaSeconds = 0.0f;
        float InterpolationSpeed = 0.0f;
        float TeleportDistance = 0.0f;
    };

    static FTransform InterpolateEntityTransform(const FEntityInterpolationWorkItem& Work)
    {
        if (Work.TargetTransform.ContainsNaN())
        {
            return Work.CurrentTransform;
        }
        if (Work.CurrentTransform.ContainsNaN())
        {
            return Work.TargetTransform;
        }

        const FVector CurrentLocation = Work.CurrentTransform.GetLocation();
        const FVector TargetLocation = Work.TargetTransform.GetLocation();
        const bool bTeleport = Work.TeleportDistance > 0.0f
            && FVector::DistSquared(CurrentLocation, TargetLocation) > FMath::Square(Work.TeleportDistance);

        if (bTeleport || Work.InterpolationSpeed <= KINDA_SMALL_NUMBER || Work.DeltaSeconds <= 0.0f)
        {
            return Work.TargetTransform;
        }

        const float Alpha = FMath::Clamp(
            1.0f - FMath::Exp(-Work.InterpolationSpeed * Work.DeltaSeconds),
            0.0f,
            1.0f);
        const FVector Location = FMath::Lerp(CurrentLocation, TargetLocation, Alpha);
        const FQuat Rotation = FQuat::Slerp(
            Work.CurrentTransform.GetRotation().GetNormalized(),
            Work.TargetTransform.GetRotation().GetNormalized(),
            Alpha).GetNormalized();
        const FVector Scale = FMath::Lerp(
            Work.CurrentTransform.GetScale3D(),
            Work.TargetTransform.GetScale3D(),
            Alpha);

        FTransform Result(Rotation, Location, Scale);
        if (Result.Equals(Work.TargetTransform, 0.001f))
        {
            Result = Work.TargetTransform;
        }
        return Result;
    }
}

UInstancedEntitySubsystem* UInstancedEntitySubsystem::Get(const UObject* WorldContextObject)
{
    if (!IsValid(WorldContextObject))
    {
        return nullptr;
    }

    UWorld* World = WorldContextObject->GetWorld();
    return World ? World->GetSubsystem<UInstancedEntitySubsystem>() : nullptr;
}

void UInstancedEntitySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    check(IsInGameThread());
    Super::Initialize(Collection);

    RegisterGameUpdate();
}

void UInstancedEntitySubsystem::RegisterGameUpdate()
{
    check(IsInGameThread());
    if (GameUpdateHandle != INDEX_NONE)
    {
        return;
    }

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdateHandle = GameUpdate->RegisterUpdate(
            this,
            [this](const float DeltaSeconds)
            {
                UpdateFromGameUpdate(DeltaSeconds);
            },
            80);
    }
}

void UInstancedEntitySubsystem::Deinitialize()
{
    check(IsInGameThread());

    if (UGameUpdateSubSystem* GameUpdate = UGameUpdateSubSystem::Get(this))
    {
        GameUpdate->UnregisterUpdate(GameUpdateHandle);
    }
    GameUpdateHandle = INDEX_NONE;

    TArray<int32> RegistrationIds;
    Registrations.GetKeys(RegistrationIds);
    for (const int32 RegistrationId : RegistrationIds)
    {
        UnregisterEntity(RegistrationId);
    }

    for (TPair<FString, TObjectPtr<AInstancedEntityRenderActor>>& Pair : RenderActors)
    {
        if (AInstancedEntityRenderActor* Actor = Pair.Value.Get())
        {
            Actor->ReleaseRuntimeResources();
            Actor->Destroy();
        }
    }
    RenderActors.Empty();
    ResourceStates.Empty();
    Registrations.Empty();

    Super::Deinitialize();
}

FString UInstancedEntitySubsystem::MakeResourceKey(const FString& SourceFilePath) const
{
    if (SourceFilePath.IsEmpty())
    {
        return FString();
    }

    FString ResourceKey = GlbValidation::NormalizePath(SourceFilePath);
#if PLATFORM_WINDOWS
    // Windows paths are case-insensitive. Canonicalizing the key avoids creating duplicate
    // render resources when the same file arrives through differently-cased replicated paths.
    ResourceKey.ToLowerInline();
#endif
    return ResourceKey;
}

AInstancedEntityRenderActor* UInstancedEntitySubsystem::GetOrCreateRenderActor(const FString& ResourceKey)
{
    if (ResourceKey.IsEmpty())
    {
        return nullptr;
    }

    if (TObjectPtr<AInstancedEntityRenderActor>* Existing = RenderActors.Find(ResourceKey))
    {
        if (IsValid(Existing->Get()))
        {
            return Existing->Get();
        }
        RenderActors.Remove(ResourceKey);
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return nullptr;
    }

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.ObjectFlags |= RF_Transient;
    SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AInstancedEntityRenderActor* Actor = World->SpawnActor<AInstancedEntityRenderActor>(
        AInstancedEntityRenderActor::StaticClass(),
        FTransform::Identity,
        SpawnParameters);
    if (!IsValid(Actor))
    {
        return nullptr;
    }

    Actor->InitializeResource(ResourceKey);
    RenderActors.Add(ResourceKey, Actor);
    ResourceStates.FindOrAdd(ResourceKey);
    return Actor;
}

int32 UInstancedEntitySubsystem::RegisterEntity(
    const FString& SourceFilePath,
    AActor* Owner,
    UPrimitiveComponent* PhysicsRoot,
    const TArray<FInstancedEntityMeshPart>& MeshParts,
    const FInstancedEntityRegistrationOptions& Options,
    const FBox& LocalBounds)
{
    check(IsInGameThread());
    RegisterGameUpdate();

    if (!IsValid(Owner) || MeshParts.Num() == 0)
    {
        return INDEX_NONE;
    }

    const FString NormalizedSourcePath = GlbValidation::NormalizePath(SourceFilePath);
    if (NormalizedSourcePath.IsEmpty() || !IFileManager::Get().FileExists(*NormalizedSourcePath))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("InstancedEntitySubsystem: source file is missing; registration skipped. Path=%s"),
            *NormalizedSourcePath);
        return INDEX_NONE;
    }

    // Validate the complete descriptor first. Part order is a public contract used by vehicles to
    // update wheel transforms, so registration is all-or-nothing rather than silently compacting it.
    for (const FInstancedEntityMeshPart& Part : MeshParts)
    {
        if (Part.MeshKey == INDEX_NONE || !IsValid(Part.Mesh) || Part.LocalTransform.ContainsNaN())
        {
            return INDEX_NONE;
        }
    }

    const FString ResourceKey = MakeResourceKey(NormalizedSourcePath);
    AInstancedEntityRenderActor* RenderActor = GetOrCreateRenderActor(ResourceKey);
    if (!IsValid(RenderActor))
    {
        return INDEX_NONE;
    }

    const FTransform RootTransform = IsValid(PhysicsRoot)
        ? PhysicsRoot->GetComponentTransform()
        : Owner->GetActorTransform();
    if (RootTransform.ContainsNaN())
    {
        ReleaseResourceIfUnused(ResourceKey);
        return INDEX_NONE;
    }

    FEntityRegistration Registration;
    Registration.Id = NextRegistrationId++;
    Registration.ResourceKey = ResourceKey;
    Registration.Owner = Owner;
    Registration.PhysicsRoot = PhysicsRoot;
    Registration.Options = Options;
    Registration.LastRootTransform = RootTransform;
    Registration.bOriginalSimulatePhysics = IsValid(PhysicsRoot) && PhysicsRoot->IsSimulatingPhysics();
    Registration.OriginalCollisionEnabled = IsValid(PhysicsRoot)
        ? PhysicsRoot->GetCollisionEnabled()
        : ECollisionEnabled::NoCollision;
    Registration.bRenderAtTarget = true;
    Registration.Parts.Reserve(MeshParts.Num());

    const int32 StartCullDistance = FMath::Max(0, FMath::RoundToInt(Options.MidDistance));
    const int32 EndCullDistance = FMath::Max(
        StartCullDistance,
        FMath::RoundToInt(FMath::Max(Options.EndCullDistance, Options.PhysicsSuspendDistance)));

    for (const FInstancedEntityMeshPart& Part : MeshParts)
    {
        const FTransform WorldTransform = Part.LocalTransform * RootTransform;
        const int32 InstanceIndex = RenderActor->AddMeshInstance(
            Part.MeshKey,
            Part.Mesh,
            WorldTransform,
            StartCullDistance,
            EndCullDistance);
        if (InstanceIndex == INDEX_NONE)
        {
            for (const FEntityPartBinding& AddedBinding : Registration.Parts)
            {
                RenderActor->RemoveMeshInstance(AddedBinding.MeshKey, AddedBinding.InstanceIndex);
            }
            RenderActor->FlushDirtyTransforms();
            ReleaseResourceIfUnused(ResourceKey);
            return INDEX_NONE;
        }

        FEntityPartBinding& Binding = Registration.Parts.AddDefaulted_GetRef();
        Binding.MeshKey = Part.MeshKey;
        Binding.InstanceIndex = InstanceIndex;
        Binding.LocalTransform = Part.LocalTransform;
        Binding.RenderWorldTransform = WorldTransform;
    }
    RenderActor->FlushDirtyTransforms();

    if (Options.bStoreAsPrefabTemplate)
    {
        FResourceState& ResourceState = ResourceStates.FindOrAdd(ResourceKey);
        if (ResourceState.PrefabTemplateParts.Num() == 0)
        {
            ResourceState.PrefabTemplateParts.Reserve(Registration.Parts.Num());
            for (const FEntityPartBinding& Binding : Registration.Parts)
            {
                FPrefabTemplatePart& TemplatePart = ResourceState.PrefabTemplateParts.AddDefaulted_GetRef();
                TemplatePart.MeshKey = Binding.MeshKey;
                TemplatePart.LocalTransform = Binding.LocalTransform;
            }
            ResourceState.PrefabTemplateBounds = LocalBounds;
        }
    }

    if (Options.bStoreAsVehicleTemplate)
    {
        FResourceState& ResourceState = ResourceStates.FindOrAdd(ResourceKey);
        if (ResourceState.VehicleTemplateParts.Num() == 0)
        {
            ResourceState.VehicleTemplateParts.Reserve(Registration.Parts.Num());
            for (const FEntityPartBinding& Binding : Registration.Parts)
            {
                FPrefabTemplatePart& TemplatePart = ResourceState.VehicleTemplateParts.AddDefaulted_GetRef();
                TemplatePart.MeshKey = Binding.MeshKey;
                TemplatePart.LocalTransform = Binding.LocalTransform;
            }
        }
    }

    const int32 RegistrationId = Registration.Id;
    Registrations.Add(RegistrationId, MoveTemp(Registration));
    return RegistrationId;
}


int32 UInstancedEntitySubsystem::RegisterEntityFromPrefabTemplate(
    const FString& SourceFilePath,
    AActor* Owner,
    UPrimitiveComponent* PhysicsRoot,
    const FInstancedEntityRegistrationOptions& Options,
    FBox& OutLocalBounds)
{
    check(IsInGameThread());
    OutLocalBounds.Init();

    const FString ResourceKey = MakeResourceKey(SourceFilePath);
    const FResourceState* ResourceState = ResourceStates.Find(ResourceKey);
    const TObjectPtr<AInstancedEntityRenderActor>* RenderActorPtr = RenderActors.Find(ResourceKey);
    AInstancedEntityRenderActor* RenderActor = RenderActorPtr ? RenderActorPtr->Get() : nullptr;
    if (!ResourceState || ResourceState->PrefabTemplateParts.Num() == 0 || !IsValid(RenderActor))
    {
        return INDEX_NONE;
    }

    TArray<FInstancedEntityMeshPart> MeshParts;
    MeshParts.Reserve(ResourceState->PrefabTemplateParts.Num());
    for (const FPrefabTemplatePart& TemplatePart : ResourceState->PrefabTemplateParts)
    {
        UStaticMesh* Mesh = RenderActor->FindMesh(TemplatePart.MeshKey);
        if (!IsValid(Mesh))
        {
            return INDEX_NONE;
        }

        FInstancedEntityMeshPart& Part = MeshParts.AddDefaulted_GetRef();
        Part.MeshKey = TemplatePart.MeshKey;
        Part.Mesh = Mesh;
        Part.LocalTransform = TemplatePart.LocalTransform;
    }

    OutLocalBounds = ResourceState->PrefabTemplateBounds;
    FInstancedEntityRegistrationOptions TemplateOptions = Options;
    TemplateOptions.bStoreAsPrefabTemplate = false;
    return RegisterEntity(SourceFilePath, Owner, PhysicsRoot, MeshParts, TemplateOptions, OutLocalBounds);
}

int32 UInstancedEntitySubsystem::RegisterVehicleEntityFromTemplate(
    const FString& SourceFilePath,
    AActor* Owner,
    UPrimitiveComponent* PhysicsRoot,
    const FInstancedEntityRegistrationOptions& Options)
{
    check(IsInGameThread());

    const FString ResourceKey = MakeResourceKey(SourceFilePath);
    const FResourceState* ResourceState = ResourceStates.Find(ResourceKey);
    const TObjectPtr<AInstancedEntityRenderActor>* RenderActorPtr = RenderActors.Find(ResourceKey);
    AInstancedEntityRenderActor* RenderActor = RenderActorPtr ? RenderActorPtr->Get() : nullptr;
    if (!ResourceState
        || !ResourceState->bHasVehicleTemplateData
        || ResourceState->VehicleTemplateParts.Num() == 0
        || !ResourceState->VehicleTemplateData.HasConsistentWheelData()
        || !IsValid(RenderActor))
    {
        return INDEX_NONE;
    }

    TArray<FInstancedEntityMeshPart> MeshParts;
    MeshParts.Reserve(ResourceState->VehicleTemplateParts.Num());
    for (const FPrefabTemplatePart& TemplatePart : ResourceState->VehicleTemplateParts)
    {
        UStaticMesh* Mesh = RenderActor->FindMesh(TemplatePart.MeshKey);
        if (!IsValid(Mesh))
        {
            return INDEX_NONE;
        }

        FInstancedEntityMeshPart& Part = MeshParts.AddDefaulted_GetRef();
        Part.MeshKey = TemplatePart.MeshKey;
        Part.Mesh = Mesh;
        Part.LocalTransform = TemplatePart.LocalTransform;
    }

    FInstancedEntityRegistrationOptions TemplateOptions = Options;
    TemplateOptions.bStoreAsPrefabTemplate = false;
    TemplateOptions.bStoreAsVehicleTemplate = false;
    const FBox CombinedLocalBounds = ResourceState->VehicleTemplateData.CombinedLocalBounds;
    return RegisterEntity(
        SourceFilePath,
        Owner,
        PhysicsRoot,
        MeshParts,
        TemplateOptions,
        CombinedLocalBounds);
}

bool UInstancedEntitySubsystem::GetVehicleTemplateData(
    const FString& SourceFilePath,
    FInstancedVehicleTemplateData& OutTemplateData) const
{
    OutTemplateData = FInstancedVehicleTemplateData();
    const FString ResourceKey = MakeResourceKey(SourceFilePath);
    const FResourceState* ResourceState = ResourceStates.Find(ResourceKey);
    const TObjectPtr<AInstancedEntityRenderActor>* RenderActorPtr = RenderActors.Find(ResourceKey);
    if (!ResourceState
        || !ResourceState->bHasVehicleTemplateData
        || ResourceState->VehicleTemplateParts.Num() == 0
        || !ResourceState->VehicleTemplateData.HasConsistentWheelData()
        || !RenderActorPtr
        || !IsValid(RenderActorPtr->Get()))
    {
        return false;
    }

    for (const int32 WheelPartIndex : ResourceState->VehicleTemplateData.WheelPartIndices)
    {
        if (!ResourceState->VehicleTemplateParts.IsValidIndex(WheelPartIndex))
        {
            return false;
        }
    }

    OutTemplateData = ResourceState->VehicleTemplateData;
    return true;
}

bool UInstancedEntitySubsystem::StoreVehicleTemplateData(
    const FString& SourceFilePath,
    const FInstancedVehicleTemplateData& TemplateData)
{
    check(IsInGameThread());

    if (!TemplateData.HasConsistentWheelData())
    {
        return false;
    }

    const FString ResourceKey = MakeResourceKey(SourceFilePath);
    FResourceState* ResourceState = ResourceStates.Find(ResourceKey);
    if (!ResourceState || ResourceState->VehicleTemplateParts.Num() == 0)
    {
        return false;
    }

    for (const int32 WheelPartIndex : TemplateData.WheelPartIndices)
    {
        if (!ResourceState->VehicleTemplateParts.IsValidIndex(WheelPartIndex))
        {
            return false;
        }
    }

    ResourceState->VehicleTemplateData = TemplateData;
    ResourceState->bHasVehicleTemplateData = true;
    return true;
}

void UInstancedEntitySubsystem::UnregisterEntity(int32 RegistrationId)
{
    check(IsInGameThread());

    FEntityRegistration* Registration = Registrations.Find(RegistrationId);
    if (!Registration)
    {
        return;
    }

    const FString ResourceKey = Registration->ResourceKey;
    if (Registration->bPhysicsSuspended && Registration->Owner.IsValid())
    {
        UpdatePhysicsActivation(*Registration, true);
    }

    AInstancedEntityRenderActor* RenderActor = RenderActors.FindRef(ResourceKey).Get();
    if (IsValid(RenderActor))
    {
        for (const FEntityPartBinding& Binding : Registration->Parts)
        {
            if (Binding.MeshKey != INDEX_NONE && Binding.InstanceIndex != INDEX_NONE)
            {
                RenderActor->RemoveMeshInstance(Binding.MeshKey, Binding.InstanceIndex);
            }
        }
        RenderActor->FlushDirtyTransforms();
    }

    Registrations.Remove(RegistrationId);
    ReleaseResourceIfUnused(ResourceKey);
}


bool UInstancedEntitySubsystem::UpdateEntityPartLocalTransform(
    int32 RegistrationId,
    int32 PartIndex,
    const FTransform& LocalTransform)
{
    check(IsInGameThread());
    if (LocalTransform.ContainsNaN())
    {
        return false;
    }

    FEntityRegistration* Registration = Registrations.Find(RegistrationId);
    if (!Registration || !Registration->Parts.IsValidIndex(PartIndex))
    {
        return false;
    }

    FEntityPartBinding& Binding = Registration->Parts[PartIndex];
    if (Binding.LocalTransform.Equals(LocalTransform, 0.001f))
    {
        return true;
    }

    Binding.LocalTransform = LocalTransform;
    // Keep distance-tier throttling intact. Near/always-relevant entities are due every frame,
    // while mid/far entities consume local animation changes at their configured cadence.
    Registration->bRenderAtTarget = false;
    return true;
}

bool UInstancedEntitySubsystem::UpdateEntityPartLocalTransforms(
    int32 RegistrationId,
    const TArray<FTransform>& LocalTransforms)
{
    check(IsInGameThread());

    FEntityRegistration* Registration = Registrations.Find(RegistrationId);
    if (!Registration || LocalTransforms.Num() != Registration->Parts.Num())
    {
        return false;
    }

    for (int32 Index = 0; Index < LocalTransforms.Num(); ++Index)
    {
        if (LocalTransforms[Index].ContainsNaN())
        {
            return false;
        }
    }

    bool bChanged = false;
    for (int32 Index = 0; Index < LocalTransforms.Num(); ++Index)
    {
        FEntityPartBinding& Binding = Registration->Parts[Index];
        if (!Binding.LocalTransform.Equals(LocalTransforms[Index], 0.001f))
        {
            Binding.LocalTransform = LocalTransforms[Index];
            bChanged = true;
        }
    }
    if (!bChanged)
    {
        return true;
    }

    // Do not bypass the distance-tier interval merely because an animated local part changed.
    Registration->bRenderAtTarget = false;
    return true;
}

void UInstancedEntitySubsystem::SetEntityAlwaysRelevant(int32 RegistrationId, bool bAlwaysRelevant)
{
    check(IsInGameThread());

    FEntityRegistration* Registration = Registrations.Find(RegistrationId);
    if (!Registration)
    {
        return;
    }

    if (Registration->Options.bAlwaysRelevant == bAlwaysRelevant)
    {
        return;
    }

    Registration->Options.bAlwaysRelevant = bAlwaysRelevant;
    Registration->bForceUpdate = true;
    if (bAlwaysRelevant)
    {
        UpdatePhysicsActivation(*Registration, true);
    }
}

bool UInstancedEntitySubsystem::IsEntityPhysicsActive(int32 RegistrationId) const
{
    const FEntityRegistration* Registration = Registrations.Find(RegistrationId);
    return Registration && !Registration->bPhysicsSuspended;
}

UStaticMesh* UInstancedEntitySubsystem::FindSharedMesh(const FString& SourceFilePath, int32 MeshKey) const
{
    const FString ResourceKey = MakeResourceKey(SourceFilePath);
    const TObjectPtr<AInstancedEntityRenderActor>* ActorPtr = RenderActors.Find(ResourceKey);
    return ActorPtr && IsValid(ActorPtr->Get()) ? ActorPtr->Get()->FindMesh(MeshKey) : nullptr;
}

FTransform UInstancedEntitySubsystem::GetRegistrationRootTransform(const FEntityRegistration& Registration) const
{
    if (const UPrimitiveComponent* Primitive = Registration.PhysicsRoot.Get())
    {
        const FTransform Transform = Primitive->GetComponentTransform();
        return Transform.ContainsNaN() ? Registration.LastRootTransform : Transform;
    }

    if (const AActor* Owner = Registration.Owner.Get())
    {
        const FTransform Transform = Owner->GetActorTransform();
        return Transform.ContainsNaN() ? Registration.LastRootTransform : Transform;
    }

    return Registration.LastRootTransform;
}

void UInstancedEntitySubsystem::GatherObserverLocations(TArray<FVector>& OutLocations) const
{
    OutLocations.Reset();
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    for (FConstPlayerControllerIterator Iterator = World->GetPlayerControllerIterator(); Iterator; ++Iterator)
    {
        const APlayerController* PlayerController = Iterator->Get();
        if (!IsValid(PlayerController))
        {
            continue;
        }

        if (IsValid(PlayerController->PlayerCameraManager))
        {
            OutLocations.Add(PlayerController->PlayerCameraManager->GetCameraLocation());
        }
        else if (const APawn* Pawn = PlayerController->GetPawn())
        {
            OutLocations.Add(Pawn->GetActorLocation());
        }
    }
}

float UInstancedEntitySubsystem::CalculateNearestObserverDistanceSquared(
    const FVector& Location,
    const TArray<FVector>& ObserverLocations) const
{
    if (ObserverLocations.Num() == 0)
    {
        return 0.0f;
    }

    float BestDistanceSquared = TNumericLimits<float>::Max();
    for (const FVector& ObserverLocation : ObserverLocations)
    {
        BestDistanceSquared = FMath::Min(BestDistanceSquared, FVector::DistSquared(Location, ObserverLocation));
    }
    return BestDistanceSquared;
}

UInstancedEntitySubsystem::EDistanceTier UInstancedEntitySubsystem::ResolveDistanceTier(
    const FEntityRegistration& Registration,
    float DistanceSquared) const
{
    if (Registration.Options.bAlwaysRelevant)
    {
        return EDistanceTier::Near;
    }

    const float MidDistance = FMath::Max(0.0f, Registration.Options.MidDistance);
    const float FarDistance = FMath::Max(MidDistance, Registration.Options.PhysicsSuspendDistance);
    if (DistanceSquared <= FMath::Square(MidDistance))
    {
        return EDistanceTier::Near;
    }
    if (DistanceSquared <= FMath::Square(FarDistance))
    {
        return EDistanceTier::Mid;
    }
    return EDistanceTier::Far;
}

float UInstancedEntitySubsystem::ResolveUpdateInterval(
    const FEntityRegistration& Registration,
    EDistanceTier Tier) const
{
    switch (Tier)
    {
    case EDistanceTier::Near:
        return 0.0f;
    case EDistanceTier::Mid:
        return FMath::Max(0.0f, Registration.Options.MidUpdateInterval);
    case EDistanceTier::Far:
    default:
        return FMath::Max(Registration.Options.MidUpdateInterval, Registration.Options.FarUpdateInterval);
    }
}

void UInstancedEntitySubsystem::UpdatePhysicsActivation(FEntityRegistration& Registration, bool bShouldBeActive)
{
    if (!Registration.Options.bDynamic || !Registration.Options.bAllowPhysicsDistanceDeactivation)
    {
        return;
    }

    UPrimitiveComponent* Primitive = Registration.PhysicsRoot.Get();
    if (!IsValid(Primitive))
    {
        return;
    }

    if (bShouldBeActive)
    {
        if (!Registration.bPhysicsSuspended)
        {
            return;
        }

        Primitive->SetWorldTransform(
            Registration.SuspendedWorldTransform,
            false,
            nullptr,
            ETeleportType::TeleportPhysics);
        Primitive->SetCollisionEnabled(Registration.OriginalCollisionEnabled);
        if (Registration.bOriginalSimulatePhysics)
        {
            Primitive->SetSimulatePhysics(true);
            Primitive->SetPhysicsLinearVelocity(Registration.SuspendedLinearVelocity);
            Primitive->SetPhysicsAngularVelocityInRadians(Registration.SuspendedAngularVelocity);
        }
        Registration.bPhysicsSuspended = false;
        Registration.bForceUpdate = true;
        Registration.bRenderAtTarget = false;
        return;
    }

    if (Registration.bPhysicsSuspended)
    {
        return;
    }

    Registration.SuspendedWorldTransform = Primitive->GetComponentTransform();
    Registration.bOriginalSimulatePhysics = Primitive->IsSimulatingPhysics();
    Registration.OriginalCollisionEnabled = Primitive->GetCollisionEnabled();
    Registration.SuspendedLinearVelocity = Primitive->IsSimulatingPhysics()
        ? Primitive->GetPhysicsLinearVelocity()
        : FVector::ZeroVector;
    Registration.SuspendedAngularVelocity = Primitive->IsSimulatingPhysics()
        ? Primitive->GetPhysicsAngularVelocityInRadians()
        : FVector::ZeroVector;

    if (Primitive->IsSimulatingPhysics())
    {
        Primitive->SetSimulatePhysics(false);
    }
    Primitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Registration.bPhysicsSuspended = true;
    Registration.bForceUpdate = true;
    Registration.bRenderAtTarget = false;
}

void UInstancedEntitySubsystem::UpdateFromGameUpdate(float DeltaSeconds)
{
    check(IsInGameThread());
    const float SafeDeltaSeconds = FMath::Clamp(DeltaSeconds, 0.0f, 1.0f);
    if (SafeDeltaSeconds <= 0.0f)
    {
        return;
    }

    TArray<int32> InvalidRegistrationIds;
    TArray<FVector> ObserverLocations;
    GatherObserverLocations(ObserverLocations);

    TArray<FEntityInterpolationWorkItem> WorkItems;
    for (TPair<int32, FEntityRegistration>& Pair : Registrations)
    {
        FEntityRegistration& Registration = Pair.Value;
        if (!Registration.Owner.IsValid())
        {
            InvalidRegistrationIds.Add(Pair.Key);
            continue;
        }

        const FTransform RootTransform = GetRegistrationRootTransform(Registration);
        const float DistanceSquared = CalculateNearestObserverDistanceSquared(
            RootTransform.GetLocation(),
            ObserverLocations);
        const EDistanceTier Tier = ResolveDistanceTier(Registration, DistanceSquared);

        bool bShouldPhysicsBeActive = true;
        if (!Registration.Options.bAlwaysRelevant
            && Registration.Options.bDynamic
            && Registration.Options.bAllowPhysicsDistanceDeactivation)
        {
            const float SuspendDistance = FMath::Max(0.0f, Registration.Options.PhysicsSuspendDistance);
            const float ReactivateDistance = SuspendDistance * PhysicsReactivateHysteresis;
            bShouldPhysicsBeActive = Registration.bPhysicsSuspended
                ? DistanceSquared <= FMath::Square(ReactivateDistance)
                : DistanceSquared <= FMath::Square(SuspendDistance);
        }
        UpdatePhysicsActivation(Registration, bShouldPhysicsBeActive);

        const FTransform EffectiveRootTransform = Registration.bPhysicsSuspended
            ? Registration.SuspendedWorldTransform
            : GetRegistrationRootTransform(Registration);
        const bool bRootChanged = !Registration.LastRootTransform.Equals(EffectiveRootTransform, 0.001f);
        Registration.UpdateAccumulator += SafeDeltaSeconds;
        const float UpdateInterval = ResolveUpdateInterval(Registration, Tier);
        const bool bUpdateDue = UpdateInterval <= 0.0f || Registration.UpdateAccumulator >= UpdateInterval;

        if (!Registration.bForceUpdate && !bUpdateDue)
        {
            continue;
        }
        if (!Registration.bForceUpdate && !bRootChanged && Registration.bRenderAtTarget)
        {
            Registration.UpdateAccumulator = 0.0f;
            continue;
        }

        const float InterpolationDelta = FMath::Max(SafeDeltaSeconds, Registration.UpdateAccumulator);
        Registration.UpdateAccumulator = 0.0f;
        Registration.bForceUpdate = false;
        Registration.bRenderAtTarget = true;
        Registration.LastRootTransform = EffectiveRootTransform;

        for (int32 PartIndex = 0; PartIndex < Registration.Parts.Num(); ++PartIndex)
        {
            const FEntityPartBinding& Binding = Registration.Parts[PartIndex];
            FEntityInterpolationWorkItem& Work = WorkItems.AddDefaulted_GetRef();
            Work.RegistrationId = Registration.Id;
            Work.PartIndex = PartIndex;
            Work.CurrentTransform = Binding.RenderWorldTransform;
            Work.TargetTransform = Binding.LocalTransform * EffectiveRootTransform;
            Work.DeltaSeconds = InterpolationDelta;
            Work.InterpolationSpeed = FMath::Max(0.0f, Registration.Options.InterpolationSpeed);
            Work.TeleportDistance = FMath::Max(0.0f, Registration.Options.TeleportDistance);
        }
    }

    auto ComputeWorkItem = [&WorkItems](const int32 Index)
    {
        WorkItems[Index].ResultTransform = InterpolateEntityTransform(WorkItems[Index]);
    };

    if (WorkItems.Num() >= ParallelInterpolationThreshold)
    {
        ParallelFor(WorkItems.Num(), ComputeWorkItem);
    }
    else
    {
        for (int32 Index = 0; Index < WorkItems.Num(); ++Index)
        {
            ComputeWorkItem(Index);
        }
    }

    for (const FEntityInterpolationWorkItem& Work : WorkItems)
    {
        FEntityRegistration* Registration = Registrations.Find(Work.RegistrationId);
        if (!Registration || !Registration->Parts.IsValidIndex(Work.PartIndex))
        {
            continue;
        }

        FEntityPartBinding& Binding = Registration->Parts[Work.PartIndex];
        const TObjectPtr<AInstancedEntityRenderActor>* RenderActorPtr = RenderActors.Find(Registration->ResourceKey);
        AInstancedEntityRenderActor* RenderActor = RenderActorPtr ? RenderActorPtr->Get() : nullptr;
        if (!IsValid(RenderActor))
        {
            continue;
        }

        Binding.RenderWorldTransform = Work.ResultTransform;
        Registration->bRenderAtTarget = Registration->bRenderAtTarget
            && Work.ResultTransform.Equals(Work.TargetTransform, 0.001f);
        RenderActor->SetCachedInstanceTransform(
            Binding.MeshKey,
            Binding.InstanceIndex,
            Work.ResultTransform);
    }

    for (TPair<FString, TObjectPtr<AInstancedEntityRenderActor>>& Pair : RenderActors)
    {
        if (AInstancedEntityRenderActor* Actor = Pair.Value.Get())
        {
            Actor->FlushDirtyTransforms();
        }
    }

    for (const int32 RegistrationId : InvalidRegistrationIds)
    {
        UnregisterEntity(RegistrationId);
    }
}



bool UInstancedEntitySubsystem::IsResourceReferenced(const FString& ResourceKey) const
{
    for (const TPair<int32, FEntityRegistration>& Pair : Registrations)
    {
        if (Pair.Value.ResourceKey == ResourceKey)
        {
            return true;
        }
    }
    return false;
}

void UInstancedEntitySubsystem::ReleaseResourceIfUnused(const FString& ResourceKey)
{
    if (ResourceKey.IsEmpty() || IsResourceReferenced(ResourceKey))
    {
        return;
    }

    TObjectPtr<AInstancedEntityRenderActor> Actor = RenderActors.FindRef(ResourceKey);
    if (IsValid(Actor.Get()))
    {
        Actor->ReleaseRuntimeResources();
        Actor->Destroy();
    }
    RenderActors.Remove(ResourceKey);
    ResourceStates.Remove(ResourceKey);
}
