#include "RuntimeFramework/SimulatorVehicleSelectionSubsystem.h"
#include "RuntimeFramework/SimulatorAssetPathLibrary.h"
#include "RuntimeFramework/SimulatorRuntimeAssetSource.h"
#include "Engine/World.h"
#include "Misc/Paths.h"

void USimulatorVehicleSelectionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    AllowedAssetRoots = {
        FPaths::ProjectContentDir(),
        FPaths::Combine(FPaths::ProjectDir(), TEXT("stream")),
        FPaths::Combine(FPaths::ProjectDir(), TEXT("models")),
        FPaths::Combine(FPaths::ProjectDir(), TEXT("prefab")),
        FPaths::Combine(FPaths::ProjectDir(), TEXT("vehicle")),
        FPaths::Combine(FPaths::ProjectDir(), TEXT("vehicles"))
    };
}

void USimulatorVehicleSelectionSubsystem::SetAllowedAssetRoots(const TArray<FString>& Roots) { AllowedAssetRoots = Roots; }
void USimulatorVehicleSelectionSubsystem::SetVehicleReferences(const TArray<FString>& References, const int32 PreserveIndex)
{
    VehicleReferences = References; const int32 Candidate = PreserveIndex != INDEX_NONE ? PreserveIndex : SelectedIndex;
    SelectedIndex = VehicleReferences.IsValidIndex(Candidate) ? Candidate : (VehicleReferences.IsEmpty() ? INDEX_NONE : 0);
    if (SelectedIndex != INDEX_NONE && ExplicitReference.IsEmpty()) ExplicitReference = VehicleReferences[SelectedIndex];
}
bool USimulatorVehicleSelectionSubsystem::SelectVehicle(const int32 Index, FString& OutError)
{
    if (!VehicleReferences.IsValidIndex(Index)) { OutError = FString::Printf(TEXT("Vehicle index %d is out of range"), Index); return false; }
    SelectedIndex = Index; ExplicitReference = VehicleReferences[Index]; OutError.Reset(); return true;
}
void USimulatorVehicleSelectionSubsystem::SetExplicitVehicleReference(const FString& Reference) { ExplicitReference = Reference; }
bool USimulatorVehicleSelectionSubsystem::ResolveSelectedVehiclePath(FString& OutCanonicalPath, FString& OutError) const
{
    return USimulatorAssetPathLibrary::ResolveSelectedReference(ExplicitReference, VehicleReferences, SelectedIndex, AllowedAssetRoots, OutCanonicalPath, OutError);
}
AActor* USimulatorVehicleSelectionSubsystem::SpawnSelectedVehicle(UObject* WorldContextObject, TSubclassOf<AActor> VehicleClass, const FTransform& Transform, FString& OutError)
{
    FString Path; if (!ResolveSelectedVehiclePath(Path, OutError) || !IsValid(WorldContextObject) || !VehicleClass) return nullptr;
    UWorld* World = WorldContextObject->GetWorld(); if (!IsValid(World)) { OutError = TEXT("No valid world for vehicle spawn"); return nullptr; }
    FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
    AActor* Vehicle = World->SpawnActor<AActor>(VehicleClass, Transform, Params);
    if (!IsValid(Vehicle)) { OutError = TEXT("Vehicle actor spawn failed"); return nullptr; }
    if (!Vehicle->GetClass()->ImplementsInterface(USimulatorRuntimeAssetSource::StaticClass()))
    {
        OutError = TEXT("Vehicle class must implement SimulatorRuntimeAssetSource"); Vehicle->Destroy(); return nullptr;
    }
    ISimulatorRuntimeAssetSource::Execute_SetRuntimeAssetSource(Vehicle, Path); OutError.Reset(); return Vehicle;
}
