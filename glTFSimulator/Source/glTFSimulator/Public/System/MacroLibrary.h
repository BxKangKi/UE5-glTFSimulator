// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file MacroLibrary.h
 * Role: Defines this source unit's responsibility within glTFSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "System/StringHelper.h"

#define WORLD_MAX_SIZE 2147483647.0f
#define BOX_MAX_SIZE 65536.0f
#define BOX_BUFFER_SIZE 100.0f

#define DIRECTORY_USER FPlatformProcess::UserDir()
#define DIRECTORY_GAME TEXT("glTFSimulator")
#define DIRECTORY_PROJECTS TEXT("Projects")
#define DIRECTORY_WORLDS TEXT("Worlds")
#define DIRECTORY_RESOURCES TEXT("Resources")
#define DIRECTORY_WORLD_DATA TEXT("Data")
#define DIRECTORY_LOG TEXT("Logs")
#define PATH_PROJECTS FPaths::Combine(DIRECTORY_USER, DIRECTORY_GAME, DIRECTORY_PROJECTS)
#define PATH_WORLDS FPaths::Combine(DIRECTORY_USER, DIRECTORY_GAME, DIRECTORY_WORLDS)
#define PATH_RESOURCES FPaths::Combine(DIRECTORY_USER, DIRECTORY_GAME, DIRECTORY_RESOURCES)
#define PATH_WORLD_DATA FPaths::Combine(PATH_WORLDS, DIRECTORY_WORLD_DATA)
// Compatibility alias: existing gameplay code treats PATH_ROOT as the selected-world namespace.
#define PATH_ROOT PATH_WORLDS
#define PATH_LOG FStringHelper::Append({FPaths::Combine(DIRECTORY_USER, DIRECTORY_GAME, DIRECTORY_LOG), TEXT("/log_"), FDateTime::Now().ToString(TEXT("%Y%m%d")), TEXT(".txt")})

#define EMPTY_STR TEXT("")
#define RAGDOLL TEXT("Ragdoll")
#define JSON TEXT(".json")
#define JSON_VERSION_FIELD TEXT("Version")
#define JSON_SCHEMA_VERSION TEXT("1.0.0")

#define BONE_HAIR_ROOT TEXT("hairRoot")
#define BONE_DYN_ROOT TEXT("dynRoot")
#define BONE_RIGHT_EYE TEXT("rightEye")
#define BONE_LEFT_EYE TEXT("leftEye")
#define BONE_ROOT TEXT("Root")
#define BONE_HIPS TEXT("hips")
#define BONE_LEFT_UPPER_LEG TEXT("leftUpperLeg")
#define BONE_RIGHT_UPPER_LEG TEXT("rightUpperLeg")
#define BONE_RIGHT_FOOT TEXT("rightFoot")
#define BONE_LEFT_FOOT TEXT("leftFoot")
#define BONE_HEAD TEXT("head")
#define BONE_NECK TEXT("neck")
