// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file RuntimeModelResolver.h
 * 역할: 런타임 모델 참조를 활성 아카이브에서 해석합니다.
 * 핵심 기능: UUID 조회, immutable 모델 정의·리더 전달.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "Simulator/ModelDefinitionJson.h"

class FGWorldArchiveReader;
class UWorldBakedModelAsset;

/** Immutable lookup result safe to capture in a worker after it was assembled on the game thread. */
struct GLTFSIMULATOR_API FResolvedRuntimeModel
{
    FGuid UUID;
    FModelDefinition Definition;
    FString Reference;
    FString DefinitionJson;
    TSharedPtr<FGWorldArchiveReader, ESPMode::ThreadSafe> ArchiveReader;

    bool IsValid() const;
};

/**
 * Single runtime gateway for built models. It intentionally has no filename fallback: source GLBs
 * are consumed only by the world builder, never by gameplay actors after startup.
 */
class GLTFSIMULATOR_API FRuntimeModelResolver
{
public:
    /** Game-thread lookup against the immutable model directory. */
    static bool Resolve(
        const UObject* WorldContextObject,
        const FString& Reference,
        FResolvedRuntimeModel& OutModel,
        FString& OutError);

    /**
     * Creates a lightweight facade containing only node metadata and .dat ranges. No GLB byte
     * array or source-document parser is created at runtime.
     */
    static UWorldBakedModelAsset* LoadAssetSynchronously(
        const FResolvedRuntimeModel& Model,
        FString& OutError);
};
