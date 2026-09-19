// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#include "glTFSimulatorEditor.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Editor/TransBuffer.h"
#include "Features/IModularFeatures.h"
#include "PhysicsEngine/PhysicsAsset.h"

FglTFSimulatorEditorModule& FglTFSimulatorEditorModule::Get()
{
    return FModuleManager::LoadModuleChecked<FglTFSimulatorEditorModule>(TEXT("glTFSimulatorEditor"));
}

bool FglTFSimulatorEditorModule::IsAvailable()
{
    return FModuleManager::Get().IsModuleLoaded(TEXT("glTFSimulatorEditor"));
}

void FglTFSimulatorEditorModule::StartupModule()
{
    IModularFeatures::Get().RegisterModularFeature(
        IGlTFSimulatorEditorServices::GetModularFeatureName(),
        this);
    bEditorServicesRegistered = true;

    // Keep PIE/SIE transaction cleanup out of gameplay classes. The callback is editor-only and is
    // active before OpenLevel tears down the current play world, which is exactly when stale REINST
    // widget references must be released from the editor undo buffer.
    PreLoadMapHandle = FCoreUObjectDelegates::PreLoadMapWithContext.AddRaw(
        this,
        &FglTFSimulatorEditorModule::HandlePreLoadMap);


}

void FglTFSimulatorEditorModule::ShutdownModule()
{
    if (PreLoadMapHandle.IsValid())
    {
        FCoreUObjectDelegates::PreLoadMapWithContext.Remove(PreLoadMapHandle);
        PreLoadMapHandle.Reset();
    }

    if (bEditorServicesRegistered)
    {
        IModularFeatures::Get().UnregisterModularFeature(
            IGlTFSimulatorEditorServices::GetModularFeatureName(),
            this);
        bEditorServicesRegistered = false;
    }
}

void FglTFSimulatorEditorModule::HandlePreLoadMap(
    const FWorldContext& WorldContext,
    const FString& MapName)
{
    // PreLoadMapWithContext lets the editor module distinguish PIE travel from a normal level open,
    // so a designer's ordinary editor undo history is never cleared by this gameplay workaround.
    if (WorldContext.WorldType != EWorldType::PIE || !GEditor || !GEditor->Trans)
    {
        return;
    }

    const FString Reason = MapName.IsEmpty()
        ? FString(TEXT("glTFSimulator PIE world travel"))
        : FString::Printf(TEXT("glTFSimulator PIE world travel: %s"), *MapName);

    GEditor->Trans->Reset(FText::FromString(Reason));
    UE_LOG(LogTemp, Display,
        TEXT("[glTFSimulatorEditor] Editor transaction buffer reset before PIE world travel: %s"),
        *MapName);
}

void FglTFSimulatorEditorModule::RefreshPhysicsAsset(UPhysicsAsset* PhysicsAsset)
{
    if (!IsValid(PhysicsAsset))
    {
        return;
    }

    PhysicsAsset->InvalidateAllPhysicsMeshes();
    PhysicsAsset->RefreshPhysicsAssetChange();
}

IMPLEMENT_MODULE(FglTFSimulatorEditorModule, glTFSimulatorEditor);
