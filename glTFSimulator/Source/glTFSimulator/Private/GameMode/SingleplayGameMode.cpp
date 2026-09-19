#include "GameMode/SingleplayGameMode.h"

#include "Engine/World.h"

ASingleplayGameMode::ASingleplayGameMode() = default;

void ASingleplayGameMode::BeginPlay()
{
    if (const UWorld* World = GetWorld(); IsValid(World) && World->GetNetMode() != NM_Standalone)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("SingleplayGameMode started with NetMode=%d. Use MultiplayGameMode for listen/dedicated sessions."),
            static_cast<int32>(World->GetNetMode()));
    }

    Super::BeginPlay();
}
