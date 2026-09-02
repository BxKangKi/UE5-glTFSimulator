#include "RuntimeFramework/SimulatorAssetPathLibrary.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace SimulatorAssetPaths
{
    static FString Canonicalize(const FString& Input)
    {
        FString P = FPaths::ConvertRelativePathToFull(Input); FPaths::NormalizeFilename(P); FPaths::CollapseRelativeDirectories(P); return P;
    }
    static bool IsUnderRoot(const FString& Path, const FString& Root)
    {
        const FString CPath = Canonicalize(Path); FString CRoot = Canonicalize(Root); CRoot.RemoveFromEnd(TEXT("/"));
        return CPath.Equals(CRoot, ESearchCase::IgnoreCase) || CPath.StartsWith(CRoot + TEXT("/"), ESearchCase::IgnoreCase);
    }
}

bool USimulatorAssetPathLibrary::ResolveAssetReference(const FString& Reference, const TArray<FString>& AllowedRoots, FString& OutCanonicalPath, FString& OutError)
{
    OutCanonicalPath.Reset(); OutError.Reset(); const FString Clean = Reference.TrimStartAndEnd();
    if (Clean.IsEmpty()) { OutError = TEXT("Asset reference is empty"); return false; }
    if (AllowedRoots.IsEmpty()) { OutError = TEXT("No allowed asset root is configured"); return false; }
    TArray<FString> BaseCandidates;
    if (FPaths::IsRelative(Clean)) { for (const FString& Root : AllowedRoots) BaseCandidates.Add(FPaths::Combine(Root, Clean)); }
    else BaseCandidates.Add(Clean);
    static const TCHAR* Extensions[] = { TEXT(""), TEXT(".glb"), TEXT(".gltf"), TEXT(".json") };
    for (const FString& Base : BaseCandidates)
    {
        for (const TCHAR* Extension : Extensions)
        {
            FString Candidate = Base; if (Extension[0] != 0 && FPaths::GetExtension(Candidate).IsEmpty()) Candidate += Extension;
            Candidate = SimulatorAssetPaths::Canonicalize(Candidate);
            const bool bAllowed = AllowedRoots.ContainsByPredicate([&Candidate](const FString& Root){ return SimulatorAssetPaths::IsUnderRoot(Candidate, Root); });
            if (bAllowed && IFileManager::Get().FileExists(*Candidate)) { OutCanonicalPath = Candidate; return true; }
        }
    }
    OutError = FString::Printf(TEXT("Asset '%s' was not found under an allowed root"), *Reference); return false;
}

bool USimulatorAssetPathLibrary::ResolveSelectedReference(const FString& ExplicitReference, const TArray<FString>& References, const int32 SelectedIndex,
    const TArray<FString>& AllowedRoots, FString& OutCanonicalPath, FString& OutError)
{
    FString Reference = ExplicitReference.TrimStartAndEnd();
    if (Reference.IsEmpty() && References.IsValidIndex(SelectedIndex)) Reference = References[SelectedIndex];
    if (Reference.IsEmpty()) { OutError = TEXT("Neither the UI reference nor the selected item contains an asset path"); OutCanonicalPath.Reset(); return false; }
    return ResolveAssetReference(Reference, AllowedRoots, OutCanonicalPath, OutError);
}
