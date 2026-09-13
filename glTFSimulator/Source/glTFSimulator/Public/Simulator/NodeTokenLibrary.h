/**
 * @file NodeTokenLibrary.h
 * 역할: glTF 노드 이름의 동작 토큰을 해석합니다.
 * 핵심 기능: 토큰 정규화, 유효 토큰 조회, 충돌·LOD·물 태그 해석.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NodeTokenLibrary.generated.h"

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
 * Tokens from different families may coexist. Within one family the left-most token wins.
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
