// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file WorldArchiveCodec.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
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
