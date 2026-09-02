#include "RuntimeFramework/SimulatorBoundsCacheSubsystem.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"

bool USimulatorBoundsCacheSubsystem::TryGetBounds(const FString& ResourceKey, const FString& SourceFingerprint, const int32 SchemaRevision, FSimulatorCachedBounds& OutBounds) const
{
    const FSimulatorCachedBounds* Entry = Entries.Find(ResourceKey);
    if (!Entry || !Entry->LocalBox.IsValid || Entry->SourceFingerprint != SourceFingerprint || Entry->SchemaRevision != SchemaRevision) return false;
    OutBounds = *Entry; return true;
}

void USimulatorBoundsCacheSubsystem::StoreSCZBounds(const FString& ResourceKey, const FBox& LocalBox, const FString& SourceFingerprint, const int32 SchemaRevision)
{
    if (ResourceKey.IsEmpty() || !LocalBox.IsValid) return;
    FSimulatorCachedBounds& Entry = Entries.FindOrAdd(ResourceKey);
    // A post-load runtime measurement is authoritative for this exact revision.
    if (Entry.bRuntimeValidated && Entry.SourceFingerprint == SourceFingerprint && Entry.SchemaRevision == SchemaRevision) return;
    Entry.LocalBox = LocalBox; Entry.Source = ESimulatorBoundsSource::SCZ; Entry.SourceFingerprint = SourceFingerprint; Entry.SchemaRevision = SchemaRevision; Entry.bRuntimeValidated = false;
}

FBox USimulatorBoundsCacheSubsystem::CalculateActorLocalBounds(AActor* Actor)
{
    FBox Local(ForceInit); if (!IsValid(Actor)) return Local;
    const FTransform ActorToWorld = Actor->GetActorTransform();
    TInlineComponentArray<UPrimitiveComponent*> Components; Actor->GetComponents(Components);
    for (const UPrimitiveComponent* Component : Components)
    {
        if (!IsValid(Component) || !Component->IsRegistered()) continue;
        const FBox WorldBox = Component->Bounds.GetBox();
        const FVector Corners[8] = {
            {WorldBox.Min.X,WorldBox.Min.Y,WorldBox.Min.Z},{WorldBox.Min.X,WorldBox.Min.Y,WorldBox.Max.Z},
            {WorldBox.Min.X,WorldBox.Max.Y,WorldBox.Min.Z},{WorldBox.Min.X,WorldBox.Max.Y,WorldBox.Max.Z},
            {WorldBox.Max.X,WorldBox.Min.Y,WorldBox.Min.Z},{WorldBox.Max.X,WorldBox.Min.Y,WorldBox.Max.Z},
            {WorldBox.Max.X,WorldBox.Max.Y,WorldBox.Min.Z},{WorldBox.Max.X,WorldBox.Max.Y,WorldBox.Max.Z}};
        for (const FVector& Corner : Corners) Local += ActorToWorld.InverseTransformPosition(Corner);
    }
    return Local;
}

bool USimulatorBoundsCacheSubsystem::MeasureActorBoundsOnce(const FString& ResourceKey, AActor* LoadedActor, const FString& SourceFingerprint, const int32 SchemaRevision, FSimulatorCachedBounds& OutBounds)
{
    if (TryGetBounds(ResourceKey, SourceFingerprint, SchemaRevision, OutBounds) && OutBounds.bRuntimeValidated) return true;
    const FBox Measured = CalculateActorLocalBounds(LoadedActor); if (!Measured.IsValid) return false;
    FSimulatorCachedBounds& Entry = Entries.FindOrAdd(ResourceKey); Entry.LocalBox = Measured; Entry.Source = ESimulatorBoundsSource::RuntimeMeasured;
    Entry.SourceFingerprint = SourceFingerprint; Entry.SchemaRevision = SchemaRevision; Entry.bRuntimeValidated = true; OutBounds = Entry; return true;
}

void USimulatorBoundsCacheSubsystem::InvalidateResource(const FString& ResourceKey) { Entries.Remove(ResourceKey); }
void USimulatorBoundsCacheSubsystem::InvalidateFingerprint(const FString& SourceFingerprint)
{
    for (auto It = Entries.CreateIterator(); It; ++It) if (It.Value().SourceFingerprint == SourceFingerprint) It.RemoveCurrent();
}
