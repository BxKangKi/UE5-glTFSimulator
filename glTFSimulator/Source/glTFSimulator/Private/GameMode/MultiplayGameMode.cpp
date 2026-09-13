#include "GameMode/MultiplayGameMode.h"

#include "Engine/World.h"

AMultiplayGameMode::AMultiplayGameMode() = default;

void AMultiplayGameMode::BeginPlay()
{
    if (const UWorld* World = GetWorld(); IsValid(World) && World->GetNetMode() == NM_Standalone)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("MultiplayGameMode started in standalone mode. Host/Server travel should use a listen or dedicated net mode."));
    }

    Super::BeginPlay();
}
