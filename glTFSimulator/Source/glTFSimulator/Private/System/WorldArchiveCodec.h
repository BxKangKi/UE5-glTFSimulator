// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldArchiveCodec.h
 * 역할: gworld 내부 dat 멤버의 바이너리 코덱입니다.
 * 핵심 기능: little-endian 메시·스킨·머티리얼·텍스처·manifest 변환.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "System/WorldArchive.h"

/** Stable little-endian codecs for the independently addressable .dat archive members. */
namespace WorldArchiveCodec
{
    bool SerializeMesh(const FGWorldBakedMesh& Value, TArray<uint8>& Out, FString& Error);
    bool DeserializeMesh(const TArray<uint8>& Bytes, FGWorldBakedMesh& Out, FString& Error);
    bool SerializeSkin(const FGWorldBakedSkin& Value, TArray<uint8>& Out, FString& Error);
    bool DeserializeSkin(const TArray<uint8>& Bytes, FGWorldBakedSkin& Out, FString& Error);
    bool SerializeMaterial(const FGWorldBakedMaterial& Value, TArray<uint8>& Out, FString& Error);
    bool DeserializeMaterial(const TArray<uint8>& Bytes, FGWorldBakedMaterial& Out, FString& Error);
    bool SerializeTexture(const FGWorldBakedTexture& Value, TArray<uint8>& Out, FString& Error);
    bool DeserializeTexture(const TArray<uint8>& Bytes, FGWorldBakedTexture& Out, FString& Error);
    bool SerializeManifest(const FGWorldModelManifest& Value, TArray<uint8>& Out, FString& Error);
    bool DeserializeManifest(const TArray<uint8>& Bytes, FGWorldModelManifest& Out, FString& Error);
}
