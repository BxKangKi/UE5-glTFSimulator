// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"

namespace RagdollRecoveryMath
{
    inline FTransform RebaseRoot(const FTransform& LocalRoot, const FTransform& OldMeshWorld,
        const FTransform& NewMeshWorld)
    {
        FTransform Result = (LocalRoot * OldMeshWorld).GetRelativeTransform(NewMeshWorld);
        Result.NormalizeRotation();
        return Result;
    }
}
