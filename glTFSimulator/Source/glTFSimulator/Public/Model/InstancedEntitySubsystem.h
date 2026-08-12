// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "InstancedEntitySubsystem.generated.h"

class AActor;
class AInstancedEntityRenderActor;
class UPrimitiveComponent;
class UStaticMesh;

/** One renderable mesh node belonging to an entity prefab. */
struct GLTFSIMULATOR_API FInstancedEntityMeshPart
{
    int32 MeshKey = INDEX_NONE;
    UStaticMesh* Mesh = nullptr;
    FTransform LocalTransform = FTransform::Identity;
};

/** Runtime policy shared by all mesh parts of one physics/entity proxy. Distances are centimeters. */
struct GLTFSIMULATOR_API FInstancedEntityRegistrationOptions
{
    bool bDynamic = false;
    bool bAllowPhysicsDistanceDeactivation = false;
    bool bAlwaysRelevant = false;
    bool bStoreAsPrefabTemplate = false;
    bool bStoreAsVehicleTemplate = false;

    float InterpolationSpeed = 18.0f;
    float TeleportDistance = 2500.0f;
    float MidDistance = 40000.0f;
    float PhysicsSuspendDistance = 80000.0f;
    float EndCullDistance = 150000.0f;
    float MidUpdateInterval = 1.0f / 30.0f;
    float FarUpdateInterval = 0.20f;
};

/** Authored vehicle geometry metadata retained beside the shared mesh instances. */
struct GLTFSIMULATOR_API FInstancedVehicleTemplateData
{
    TArray<int32> WheelPartIndices;
    TArray<FQuat> WheelBaseRotations;
    TArray<FVector> WheelBaseScales;
    TArray<FVector> WheelVisualCenterOffsets;
    TArray<FVector> AuthoredWheelOffsets;
    TArray<float> WheelTargetSpringLengths;
    /** Per-wheel vertical center-to-road radius derived from the authored mesh bounds. */
    TArray<float> WheelGroundRadii;
    FBox BodyVisualBounds = FBox(ForceInit);
    FBox WheelVisualRestBounds = FBox(ForceInit);
    FBox CombinedLocalBounds = FBox(ForceInit);
    float RuntimeWheelRadius = 0.0f;

    bool HasConsistentWheelData() const
    {
        const int32 WheelCount = WheelPartIndices.Num();
        return WheelCount > 0
            && WheelBaseRotations.Num() == WheelCount
            && WheelBaseScales.Num() == WheelCount
            && WheelVisualCenterOffsets.Num() == WheelCount
            && AuthoredWheelOffsets.Num() == WheelCount
            && WheelTargetSpringLengths.Num() == WheelCount
            && WheelGroundRadii.Num() == WheelCount;
    }
};

/**
 * Separates physics/entity actors from rendering.
 *
 * - One AInstancedEntityRenderActor is created per normalized source prefab path.
 * - Every entity keeps only its physics/collision proxy and contributes transforms to shared ISMs.
 * - Interpolation math is performed from immutable snapshots in ParallelFor and applied in one batch
 *   per ISM component on the game thread.
 * - Dynamic physics proxies are suspended outside the configured distance and restored with their
 *   transform and velocities when an observer returns.
 * - The render actor and its runtime meshes are released as soon as the last entity unregisters.
 */
UCLASS()
class GLTFSIMULATOR_API UInstancedEntitySubsystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    static UInstancedEntitySubsystem* Get(const UObject* WorldContextObject);

    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    int32 RegisterEntity(
        const FString& SourceFilePath,
        AActor* Owner,
        UPrimitiveComponent* PhysicsRoot,
        const TArray<FInstancedEntityMeshPart>& MeshParts,
        const FInstancedEntityRegistrationOptions& Options,
        const FBox& LocalBounds = FBox(ForceInit));

    /** Reuses the first entity's mesh-node template without loading the glTF meshes again. */
    int32 RegisterEntityFromPrefabTemplate(
        const FString& SourceFilePath,
        AActor* Owner,
        UPrimitiveComponent* PhysicsRoot,
        const FInstancedEntityRegistrationOptions& Options,
        FBox& OutLocalBounds);

    /** Registers a vehicle from the retained authored part list without reopening the glTF parser. */
    int32 RegisterVehicleEntityFromTemplate(
        const FString& SourceFilePath,
        AActor* Owner,
        UPrimitiveComponent* PhysicsRoot,
        const FInstancedEntityRegistrationOptions& Options);

    bool GetVehicleTemplateData(
        const FString& SourceFilePath,
        FInstancedVehicleTemplateData& OutTemplateData) const;

    bool StoreVehicleTemplateData(
        const FString& SourceFilePath,
        const FInstancedVehicleTemplateData& TemplateData);

    void UnregisterEntity(int32 RegistrationId);

    bool UpdateEntityPartLocalTransform(int32 RegistrationId, int32 PartIndex, const FTransform& LocalTransform);
    bool UpdateEntityPartLocalTransforms(int32 RegistrationId, const TArray<FTransform>& LocalTransforms);
    void SetEntityAlwaysRelevant(int32 RegistrationId, bool bAlwaysRelevant);
    bool IsEntityPhysicsActive(int32 RegistrationId) const;

    /** Returns a mesh already owned by the shared renderer for this source path, if available. */
    UStaticMesh* FindSharedMesh(const FString& SourceFilePath, int32 MeshKey) const;

    /** Safe Outer for generated runtime meshes; meshes are kept alive by the shared ISM components. */
    UObject* GetRuntimeMeshOuter() { return this; }

    void UpdateFromGameUpdate(float DeltaSeconds);

private:
    enum class EDistanceTier : uint8
    {
        Near,
        Mid,
        Far
    };

    struct FEntityPartBinding
    {
        int32 MeshKey = INDEX_NONE;
        int32 InstanceIndex = INDEX_NONE;
        FTransform LocalTransform = FTransform::Identity;
        FTransform RenderWorldTransform = FTransform::Identity;
    };

    struct FEntityRegistration
    {
        int32 Id = INDEX_NONE;
        FString ResourceKey;
        TWeakObjectPtr<AActor> Owner;
        TWeakObjectPtr<UPrimitiveComponent> PhysicsRoot;
        TArray<FEntityPartBinding> Parts;
        FInstancedEntityRegistrationOptions Options;

        FTransform LastRootTransform = FTransform::Identity;
        float UpdateAccumulator = 0.0f;
        bool bForceUpdate = true;
        bool bRenderAtTarget = true;
        bool bPhysicsSuspended = false;
        bool bOriginalSimulatePhysics = false;
        ECollisionEnabled::Type OriginalCollisionEnabled = ECollisionEnabled::NoCollision;
        FTransform SuspendedWorldTransform = FTransform::Identity;
        FVector SuspendedLinearVelocity = FVector::ZeroVector;
        FVector SuspendedAngularVelocity = FVector::ZeroVector;
    };

    struct FPrefabTemplatePart
    {
        int32 MeshKey = INDEX_NONE;
        FTransform LocalTransform = FTransform::Identity;
    };

    struct FResourceState
    {
        TArray<FPrefabTemplatePart> PrefabTemplateParts;
        FBox PrefabTemplateBounds = FBox(ForceInit);
        TArray<FPrefabTemplatePart> VehicleTemplateParts;
        FInstancedVehicleTemplateData VehicleTemplateData;
        bool bHasVehicleTemplateData = false;
    };

    UPROPERTY(Transient)
    TMap<FString, TObjectPtr<AInstancedEntityRenderActor>> RenderActors;

    TMap<FString, FResourceState> ResourceStates;
    TMap<int32, FEntityRegistration> Registrations;
    int32 NextRegistrationId = 1;
    int32 GameUpdateHandle = INDEX_NONE;

    FString MakeResourceKey(const FString& SourceFilePath) const;
    void RegisterGameUpdate();
    AInstancedEntityRenderActor* GetOrCreateRenderActor(const FString& ResourceKey);
    FTransform GetRegistrationRootTransform(const FEntityRegistration& Registration) const;
    void GatherObserverLocations(TArray<FVector>& OutLocations) const;
    float CalculateNearestObserverDistanceSquared(const FVector& Location, const TArray<FVector>& ObserverLocations) const;
    EDistanceTier ResolveDistanceTier(const FEntityRegistration& Registration, float DistanceSquared) const;
    float ResolveUpdateInterval(const FEntityRegistration& Registration, EDistanceTier Tier) const;
    void UpdatePhysicsActivation(FEntityRegistration& Registration, bool bShouldBeActive);
    void ReleaseResourceIfUnused(const FString& ResourceKey);
    bool IsResourceReferenced(const FString& ResourceKey) const;
};
