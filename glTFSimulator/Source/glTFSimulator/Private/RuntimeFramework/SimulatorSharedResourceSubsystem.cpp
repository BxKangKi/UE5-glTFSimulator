#include "RuntimeFramework/SimulatorSharedResourceSubsystem.h"
#include "Async/Async.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace SimulatorSharedResources
{
    static FString HashToHex(const uint8* Hash, const int32 Count)
    {
        FString Out; Out.Reserve(Count * 2);
        for (int32 Index = 0; Index < Count; ++Index) Out += FString::Printf(TEXT("%02x"), Hash[Index]);
        return Out;
    }
}

void USimulatorSharedResourceSubsystem::Deinitialize()
{
    ++CacheEpoch; SourceAssets.Reset(); GeneratedResources.Reset(); Super::Deinitialize();
}

bool USimulatorSharedResourceSubsystem::FingerprintFile(const FString& CanonicalPath, FString& OutFingerprint, FString& OutError)
{
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*CanonicalPath));
    if (!Handle) { OutError = FString::Printf(TEXT("Cannot open '%s'"), *CanonicalPath); return false; }
    FSHA1 Hasher; constexpr int64 ChunkSize = 4 * 1024 * 1024; TArray<uint8> Buffer; Buffer.SetNumUninitialized(static_cast<int32>(ChunkSize));
    int64 Remaining = Handle->Size();
    while (Remaining > 0)
    {
        const int64 ReadSize = FMath::Min(Remaining, ChunkSize);
        if (!Handle->Read(Buffer.GetData(), ReadSize)) { OutError = FString::Printf(TEXT("Cannot read '%s'"), *CanonicalPath); return false; }
        Hasher.Update(Buffer.GetData(), static_cast<uint32>(ReadSize)); Remaining -= ReadSize;
    }
    Hasher.Final(); uint8 Digest[FSHA1::DigestSize]; Hasher.GetHash(Digest);
    OutFingerprint = TEXT("sha1:") + SimulatorSharedResources::HashToHex(Digest, FSHA1::DigestSize); return true;
}

FString USimulatorSharedResourceSubsystem::FingerprintBytes(const TArray<uint8>& Bytes)
{
    uint8 Digest[FSHA1::DigestSize]; FSHA1::HashBuffer(Bytes.GetData(), static_cast<uint32>(Bytes.Num()), Digest);
    return TEXT("sha1:") + SimulatorSharedResources::HashToHex(Digest, FSHA1::DigestSize);
}

static FString BuildVariantCacheKey(const FString& SourceFingerprint, const FString& LoaderVariant)
{
    if (LoaderVariant.IsEmpty())
    {
        return SourceFingerprint;
    }

    FTCHARToUTF8 VariantUtf8(*LoaderVariant);
    uint8 Digest[FSHA1::DigestSize];
    FSHA1::HashBuffer(VariantUtf8.Get(), static_cast<uint32>(VariantUtf8.Length()), Digest);
    return SourceFingerprint + TEXT("|cfg:") + SimulatorSharedResources::HashToHex(Digest, FSHA1::DigestSize);
}

FSimulatorResourceLease USimulatorSharedResourceSubsystem::AcquireOrCreateOnGameThread(const FString& Fingerprint, FSimulatorSharedAssetFactory& Factory, FString& OutError)
{
    check(IsInGameThread()); FSimulatorSharedResourceEntry& Entry = SourceAssets.FindOrAdd(Fingerprint);
    if (!IsValid(Entry.Object))
    {
        Entry.Object = Factory ? Factory() : nullptr;
        if (!IsValid(Entry.Object)) { SourceAssets.Remove(Fingerprint); OutError = TEXT("Shared asset factory returned no valid UObject"); return {}; }
        ++Entry.Generation;
    }
    ++Entry.ReferenceCount; FSimulatorResourceLease Lease; Lease.Fingerprint = Fingerprint; Lease.Generation = Entry.Generation; Lease.bSourceAsset = true; return Lease;
}

FSimulatorResourceLease USimulatorSharedResourceSubsystem::AcquireSourceAssetSync(
    const FString& CanonicalPath,
    FSimulatorSharedAssetFactory&& Factory,
    UObject*& OutAsset,
    FString& OutError,
    const FString& LoaderVariant)
{
    check(IsInGameThread());
    OutAsset = nullptr;
    OutError.Reset();

    FString SourceFingerprint;
    if (!FingerprintFile(CanonicalPath, SourceFingerprint, OutError))
    {
        return {};
    }

    const FString CacheKey = BuildVariantCacheKey(SourceFingerprint, LoaderVariant);
    FSimulatorSharedAssetFactory LocalFactory = MoveTemp(Factory);
    FSimulatorResourceLease Lease = AcquireOrCreateOnGameThread(CacheKey, LocalFactory, OutError);
    if (Lease.IsValid())
    {
        OutAsset = FindSourceAsset(Lease.Fingerprint);
        if (!IsValid(OutAsset))
        {
            Release(Lease);
            OutError = TEXT("Shared source asset became invalid during synchronous acquisition");
            return {};
        }
    }
    return Lease;
}

UObject* USimulatorSharedResourceSubsystem::AcquireSourceAssetSyncForOwner(
    const FString& CanonicalPath,
    UObject* RequestOwner,
    FSimulatorSharedAssetFactory&& Factory,
    FString& OutError,
    const FString& LoaderVariant)
{
    check(IsInGameThread());
    if (!IsValid(RequestOwner))
    {
        OutError = TEXT("Shared asset request owner is invalid");
        return nullptr;
    }

    UObject* Asset = nullptr;
    FSimulatorResourceLease Lease = AcquireSourceAssetSync(CanonicalPath, MoveTemp(Factory), Asset, OutError, LoaderVariant);
    if (!Lease.IsValid())
    {
        return nullptr;
    }

    // Owner call sites immediately assign the returned UObject to a UPROPERTY/TObjectPtr.
    // Keep the cache entry, but drop the explicit lease count so repeated reloads do not leak references.
    Release(Lease);
    return Asset;
}

int32 USimulatorSharedResourceSubsystem::AcquireSourceAssetAsync(const FString& CanonicalPath, UObject* RequestOwner, FSimulatorSharedAssetFactory&& Factory, FSimulatorSharedAssetCompletion&& Completion)
{
    check(IsInGameThread()); const int32 RequestId = RequestCounter.Increment(); const uint64 Epoch = CacheEpoch;
    const bool bRequiresOwner = RequestOwner != nullptr; const TWeakObjectPtr<UObject> OwnerWeak(RequestOwner); const TWeakObjectPtr<USimulatorSharedResourceSubsystem> WeakThis(this);
    Async(EAsyncExecution::ThreadPool, [WeakThis, OwnerWeak, bRequiresOwner, CanonicalPath, Epoch, Factory = MoveTemp(Factory), Completion = MoveTemp(Completion)]() mutable
    {
        FString Fingerprint, Error; const bool bHasFingerprint = USimulatorSharedResourceSubsystem::FingerprintFile(CanonicalPath, Fingerprint, Error);
        AsyncTask(ENamedThreads::GameThread, [WeakThis, OwnerWeak, bRequiresOwner, Epoch, bHasFingerprint, Fingerprint = MoveTemp(Fingerprint), Error = MoveTemp(Error), Factory = MoveTemp(Factory), Completion = MoveTemp(Completion)]() mutable
        {
            USimulatorSharedResourceSubsystem* Self = WeakThis.Get();
            if (!IsValid(Self) || Self->CacheEpoch != Epoch || (bRequiresOwner && !OwnerWeak.IsValid())) return;
            FSimulatorResourceLease Lease; UObject* Asset = nullptr; FString FinalError = Error;
            if (bHasFingerprint)
            {
                Lease = Self->AcquireOrCreateOnGameThread(Fingerprint, Factory, FinalError);
                if (Lease.IsValid()) Asset = Self->FindSourceAsset(Lease.Fingerprint);
            }
            if (Completion) Completion(Lease, Asset, FinalError);
        });
    });
    return RequestId;
}

FSimulatorResourceLease USimulatorSharedResourceSubsystem::RegisterGeneratedResource(const FString& StableContentFingerprint, UObject* Resource)
{
    check(IsInGameThread()); if (StableContentFingerprint.IsEmpty() || !IsValid(Resource)) return {};
    FSimulatorSharedResourceEntry& Entry = GeneratedResources.FindOrAdd(StableContentFingerprint);
    if (!IsValid(Entry.Object)) { Entry.Object = Resource; ++Entry.Generation; }
    ++Entry.ReferenceCount; FSimulatorResourceLease Lease; Lease.Fingerprint = StableContentFingerprint; Lease.Generation = Entry.Generation; Lease.bSourceAsset = false; return Lease;
}

void USimulatorSharedResourceSubsystem::Release(const FSimulatorResourceLease& Lease)
{
    check(IsInGameThread()); if (!Lease.IsValid()) return; TMap<FString,FSimulatorSharedResourceEntry>& Map = Lease.bSourceAsset ? SourceAssets : GeneratedResources;
    FSimulatorSharedResourceEntry* Entry = Map.Find(Lease.Fingerprint); if (!Entry || Entry->Generation != Lease.Generation) return;
    Entry->ReferenceCount = FMath::Max(0, Entry->ReferenceCount - 1); if (Entry->ReferenceCount == 0) Entry->LastReleaseSeconds = FPlatformTime::Seconds();
}

UObject* USimulatorSharedResourceSubsystem::FindSourceAsset(const FString& Fingerprint) const
{
    const FSimulatorSharedResourceEntry* Entry = SourceAssets.Find(Fingerprint); return Entry && IsValid(Entry->Object) ? Entry->Object.Get() : nullptr;
}
UObject* USimulatorSharedResourceSubsystem::FindGeneratedResource(const FString& Fingerprint) const
{
    const FSimulatorSharedResourceEntry* Entry = GeneratedResources.Find(Fingerprint); return Entry && IsValid(Entry->Object) ? Entry->Object.Get() : nullptr;
}

void USimulatorSharedResourceSubsystem::CollectUnused(const float MinimumUnusedSeconds)
{
    check(IsInGameThread()); const double Now = FPlatformTime::Seconds();
    const auto Collect = [Now, MinimumUnusedSeconds](TMap<FString,FSimulatorSharedResourceEntry>& Map)
    {
        for (auto It = Map.CreateIterator(); It; ++It)
        {
            const FSimulatorSharedResourceEntry& Entry = It.Value();
            if (Entry.ReferenceCount == 0 && Now - Entry.LastReleaseSeconds >= FMath::Max(0.0f, MinimumUnusedSeconds)) It.RemoveCurrent();
        }
    };
    Collect(SourceAssets); Collect(GeneratedResources);
}
