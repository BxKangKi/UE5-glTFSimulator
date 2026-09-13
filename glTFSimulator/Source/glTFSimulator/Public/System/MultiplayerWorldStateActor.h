// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file MultiplayerWorldStateActor.h
 * 역할: 서버가 선택한 월드 정보를 클라이언트에 복제합니다.
 * 핵심 기능: 월드 폴더 상태 복제, 클라이언트 시작 연계.
 * 인터페이스와 수명·데이터 소유 계약을 선언하며, 동작 구현은 대응 cpp를 참고하십시오.
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MultiplayerWorldStateActor.generated.h"

/**
 * Small replicated authority state that tells clients which downloaded world folder
 * the server is running. Clients still stream their own render-only .gwd data locally;
 * the server stays authoritative for gameplay, collision and simulation.
 */
UCLASS(BlueprintType)
class GLTFSIMULATOR_API AMultiplayerWorldStateActor : public AActor
{
    GENERATED_BODY()

public:
    AMultiplayerWorldStateActor();

    static AMultiplayerWorldStateActor* SpawnOrUpdateForWorld(UObject* WorldContextObject, const FString& InWorldFolderName);

    UFUNCTION(BlueprintCallable, Category="Multiplayer")
    void SetWorldFolderName(const FString& InWorldFolderName);

    UFUNCTION(BlueprintPure, Category="Multiplayer")
    const FString& GetWorldFolderName() const { return WorldFolderName; }

protected:
    virtual void BeginPlay() override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
    UPROPERTY(ReplicatedUsing=OnRep_WorldFolderName)
    FString WorldFolderName;

private:
    UFUNCTION()
    void OnRep_WorldFolderName();

    void ApplyWorldFolderName() const;
};
