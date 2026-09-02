#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SimulatorNodeTokenLibrary.generated.h"

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FSimulatorEffectiveNodeToken
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="glTF|Node Tokens")
    FName Family = NAME_None;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="glTF|Node Tokens")
    FName Token = NAME_None;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="glTF|Node Tokens")
    int32 Ordinal = INDEX_NONE;
};

USTRUCT(BlueprintType)
struct GLTFSIMULATOR_API FSimulatorParsedNodeName
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="glTF|Node Tokens")
    FString BaseName;

    /** All syntactically valid tokens, normalized to upper case, in source order. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="glTF|Node Tokens")
    TArray<FName> Tokens;

    /** One winning token per functional family. The first token in a family wins. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="glTF|Node Tokens")
    TArray<FSimulatorEffectiveNodeToken> EffectiveTokens;

    /** Unknown or malformed segments are retained for diagnostics and never execute. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="glTF|Node Tokens")
    TArray<FString> InvalidSegments;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="glTF|Node Tokens")
    bool bHasDelimiter = false;

    /** C++ convenience helper. Blueprint callers should use USimulatorNodeTokenLibrary. */
    bool HasEffectiveToken(FName Token) const;

    /** C++ convenience helper. Blueprint callers should use USimulatorNodeTokenLibrary. */
    FName GetEffectiveTokenForFamily(FName Family) const;
};

/**
 * Single source of truth for reserved node-name directives.
 *
 * Grammar: <ordinary base name>[;<reserved token>[;<reserved token>...]]
 * A bare word, substring, prefix, suffix or underscore alias is never a directive.
 * Tokens from different families may coexist. Within one family the left-most token
 * wins, so "Body;INST;LOD0" executes INST and ignores LOD0.
 */
UCLASS()
class GLTFSIMULATOR_API USimulatorNodeTokenLibrary final : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintPure, Category="glTF|Node Tokens")
    static FSimulatorParsedNodeName ParseNodeName(const FString& NodeName);

    UFUNCTION(BlueprintPure, Category="glTF|Node Tokens")
    static bool HasEffectiveToken(const FString& NodeName, FName Token);

    UFUNCTION(BlueprintPure, Category="glTF|Node Tokens")
    static FString GetBaseNodeName(const FString& NodeName);

    /** Returns NAME_None for an unsupported token. */
    UFUNCTION(BlueprintPure, Category="glTF|Node Tokens")
    static FName GetTokenFamily(FName Token);

    UFUNCTION(BlueprintPure, Category="glTF|Node Tokens")
    static bool IsSupportedToken(FName Token);
};
