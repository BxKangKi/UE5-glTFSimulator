// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "System/StringHelper.h"

#define WORLD_MAX_SIZE 2147483647.0f
#define BOX_MAX_SIZE 65536.0f
#define BOX_BUFFER_SIZE 100.0f

#define DIRECTORY_USER FPlatformProcess::UserDir()
#define DIRECTORY_GAME TEXT("glTFSimulator")
#define DIRECTORY_SAVE TEXT("SaveData/")
#define DIRECTORY_LOG TEXT("Logs")
#define PATH_ROOT FPaths::Combine(DIRECTORY_USER, DIRECTORY_GAME, DIRECTORY_SAVE)
#define PATH_LOG UStringFuntionLibrary::Append({FPaths::Combine(DIRECTORY_USER, DIRECTORY_GAME, DIRECTORY_LOG), TEXT("/log_"), FDateTime::Now().ToString(TEXT("%Y%m%d")), TEXT(".txt")})

#define EMPTY_STR TEXT("")
#define RAGDOLL TEXT("Ragdoll")
#define JSON TEXT(".json")

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
#define MAX_INT32 TNumericLimits<int32>::Max()
#define MAX_INT64 TNumericLimits<int64>::Max()
#define MAX_FLOAT TNumericLimits<float>::Max()
#define MAX_DOUBLE =TNumericLimits<double>::Max()

template <typename... Args>
FORCEINLINE bool CheckValid(const Args &...args)
{
    return (... && (::IsValid(GetRawPointer(args))));
}

template <typename T>
FORCEINLINE T *GetRawPointer(T *Ptr) { return Ptr; }

template <typename T>
FORCEINLINE T *GetRawPointer(const TObjectPtr<T> &Ptr) { return Ptr.Get(); }