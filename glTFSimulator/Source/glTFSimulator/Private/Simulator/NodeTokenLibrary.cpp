/**
 * @file NodeTokenLibrary.cpp
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "Simulator/NodeTokenLibrary.h"

namespace SimulatorNodeTokens
{
    static const FName RenderFamily(TEXT("RenderMode"));
    static const FName InstanceFamily(TEXT("InstanceMode"));
    static const FName NodeTypeFamily(TEXT("NodeType"));
    static const FName CollisionFamily(TEXT("CollisionMode"));
    static const FName VisibilityFamily(TEXT("VisibilityMode"));
    static const FName MobilityFamily(TEXT("MobilityMode"));
    static const FName LightingFamily(TEXT("LightingMode"));

    static bool IsLodToken(const FString& Token)
    {
        // The baked/runtime schema has exactly four render LOD slots. Treat larger numeric
        // suffixes as invalid instead of silently allowing a token that the builder cannot store.
        return Token == TEXT("LOD0")
            || Token == TEXT("LOD1")
            || Token == TEXT("LOD2")
            || Token == TEXT("LOD3");
    }

    static FName ResolveFamily(const FString& Token)
    {
        // Render selection is mutually exclusive.
        if (Token == TEXT("MESH") || IsLodToken(Token))
        {
            return RenderFamily;
        }

        if (Token == TEXT("INST"))
        {
            return InstanceFamily;
        }

        // A node may still have a render token and a node-role token together.
        if (Token == TEXT("COL") || Token == TEXT("COLLIDER") || Token == TEXT("LIGHT") ||
            Token == TEXT("CAMERA") || Token == TEXT("SOCKET") || Token == TEXT("STATIC") || Token == TEXT("PREFAB") /* legacy */ ||
            Token == TEXT("VEHICLE") || Token == TEXT("WHEEL") || Token == TEXT("DOOR") ||
            Token == TEXT("SEAT") || Token == TEXT("SPAWN") || Token == TEXT("NAV") ||
            Token == TEXT("WATER"))
        {
            return NodeTypeFamily;
        }

        if (Token == TEXT("NCOL") || Token == TEXT("NOCOL") || Token == TEXT("FORCECOL"))
        {
            return CollisionFamily;
        }
        if (Token == TEXT("HIDDEN") || Token == TEXT("VISIBLE"))
        {
            return VisibilityFamily;
        }
        if (Token == TEXT("STATIC") || Token == TEXT("STATIONARY") || Token == TEXT("MOVABLE"))
        {
            return MobilityFamily;
        }
        if (Token == TEXT("SHADOW") || Token == TEXT("NOSHADOW"))
        {
            return LightingFamily;
        }
        return NAME_None;
    }
}

bool FSimulatorParsedNodeName::HasEffectiveToken(const FName Token) const
{
    const FString Normalized = Token.ToString().ToUpper();
    return EffectiveTokens.ContainsByPredicate([&Normalized](const FSimulatorEffectiveNodeToken& Item)
    {
        return Item.Token.ToString() == Normalized;
    });
}

FName FSimulatorParsedNodeName::GetEffectiveTokenForFamily(const FName Family) const
{
    const FSimulatorEffectiveNodeToken* Found = EffectiveTokens.FindByPredicate([Family](const FSimulatorEffectiveNodeToken& Item)
    {
        return Item.Family == Family;
    });
    return Found ? Found->Token : NAME_None;
}

FSimulatorParsedNodeName USimulatorNodeTokenLibrary::ParseNodeName(const FString& NodeName)
{
    FSimulatorParsedNodeName Result;
    int32 DelimiterIndex = INDEX_NONE;
    if (!NodeName.FindChar(TEXT(';'), DelimiterIndex))
    {
        Result.BaseName = NodeName;
        return Result;
    }

    Result.bHasDelimiter = true;
    Result.BaseName = NodeName.Left(DelimiterIndex);

    TArray<FString> Segments;
    NodeName.Mid(DelimiterIndex + 1).ParseIntoArray(Segments, TEXT(";"), false);
    TSet<FName> ClaimedFamilies;
    for (int32 Ordinal = 0; Ordinal < Segments.Num(); ++Ordinal)
    {
        const FString Raw = Segments[Ordinal];
        const FString Normalized = Raw.ToUpper();
        if (Raw.IsEmpty() || Raw != Raw.TrimStartAndEnd() || Normalized.Contains(TEXT(" ")))
        {
            Result.InvalidSegments.Add(Raw);
            continue;
        }

        const FName Family = SimulatorNodeTokens::ResolveFamily(Normalized);
        if (Family.IsNone())
        {
            Result.InvalidSegments.Add(Raw);
            continue;
        }

        const FName TokenName(*Normalized);
        Result.Tokens.Add(TokenName);
        if (!ClaimedFamilies.Contains(Family))
        {
            ClaimedFamilies.Add(Family);
            FSimulatorEffectiveNodeToken& Effective = Result.EffectiveTokens.AddDefaulted_GetRef();
            Effective.Family = Family;
            Effective.Token = TokenName;
            Effective.Ordinal = Ordinal;
        }
    }
    return Result;
}

bool USimulatorNodeTokenLibrary::HasEffectiveToken(const FString& NodeName, const FName Token)
{
    return ParseNodeName(NodeName).HasEffectiveToken(Token);
}

FString USimulatorNodeTokenLibrary::GetBaseNodeName(const FString& NodeName)
{
    return ParseNodeName(NodeName).BaseName;
}

FName USimulatorNodeTokenLibrary::GetTokenFamily(const FName Token)
{
    return SimulatorNodeTokens::ResolveFamily(Token.ToString().ToUpper());
}

bool USimulatorNodeTokenLibrary::IsSupportedToken(const FName Token)
{
    return !GetTokenFamily(Token).IsNone();
}
