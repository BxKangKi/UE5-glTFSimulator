# Runtime Impostors for Unreal Engine 5.8

A clean-room runtime impostor implementation designed around Unreal Engine 5.8 native rendering primitives.

## What it does

- Bakes an object from multiple azimuth/elevation views at runtime using `USceneCaptureComponent2D`.
- Packs each captured view into a single atlas render target.
- Displays the atlas on a runtime-generated 2-triangle billboard mesh.
- Selects the closest view from camera direction.
- Cross-fades between source geometry and impostor using distance thresholds.
- Works directly with actors that contain `UStaticMeshComponent`, `UInstancedStaticMeshComponent`, or other primitive components supported by Scene Capture's show-only list.
- Includes an optional glTFRuntime adapter that can build a transient preview actor from a parsed `UglTFRuntimeAsset`.

## Required setup

1. Copy `RuntimeImpostors` into `YourProject/Plugins/RuntimeImpostors`.
2. Enable the plugin and regenerate project files.
3. Add `ProceduralMeshComponent` to the project/plugin dependencies if your project does not already enable it.
4. Add `RuntimeImpostors` to the consuming game module dependencies. Because this build is tailored for the supplied project, the plugin links against `glTFRuntime` directly.
5. In the editor, select an actor containing the `URuntimeImpostorComponent` and press `Create Default Impostor Material`. This creates `/Game/RuntimeImpostorsGenerated/M_RuntimeImpostor`.
6. Call `BakeFromActor(SourceActor)` after the source actor's glTF mesh components are fully loaded.

## glTFRuntime integration

For the supplied `glTFSimulator` project, the preferred path is:

- Let the project's existing `AglTFStreamActor` finish loading its `UglTFRuntimeAsset` and instance components.
- Add `URuntimeImpostorComponent` to a lightweight holder actor or the stream actor's owner.
- Call `BakeFromActor(YourGlTFStreamActor)` to capture the already-streamed components.

This avoids re-parsing or duplicating glTF data.

The `BakeFromGlTFRuntimeAsset(UglTFRuntimeAsset*)` path is available for cases where the parsed asset exists but no source actor hierarchy should be reused. It is synchronous, builds a transient preview from the parsed glTFRuntime node/mesh data, bakes it, and discards that preview; this path therefore produces a standalone impostor without source-actor distance switching.

## Example for the supplied project

```cpp
// In your own gameplay code, after the AglTFStreamActor has loaded its runtime mesh components:
URuntimeImpostorComponent* Impostor = NewObject<URuntimeImpostorComponent>(StreamActor);
Impostor->RegisterComponent();
Impostor->AttachToComponent(StreamActor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
Impostor->BakeFromActor(StreamActor);
```

If you prefer to make it a regular component in Blueprint, add it in the Components panel and expose the same calls as Blueprint nodes.


## Exact hook for the supplied glTFSimulator

The supplied project's `AglTFStreamActor::OnStreamAsyncCompleted()` is the safest point to request a bake because it runs after `UpdateProperties(MapWrapper)`, `bIsLoaded = true`, and `LoadingStatus = 1.0f`. Add the integration call at the end of that function:

```cpp
#include "RuntimeImpostorComponent.h"

// ... at the end of AglTFStreamActor::OnStreamAsyncCompleted(...)
if (GetIsLoaded())
{
    URuntimeImpostorComponent* Impostor = NewObject<URuntimeImpostorComponent>(this);
    Impostor->RegisterComponent();
    if (USceneComponent* Root = GetRootComponent())
    {
        Impostor->AttachToComponent(Root, FAttachmentTransformRules::KeepRelativeTransform);
    }
    Impostor->BakeFromActor(this);
}
```

If you need to avoid re-baking on repeated stream completions, keep the component as a `UPROPERTY(Transient)` member on `AglTFStreamActor` and guard the block with `if (!RuntimeImpostor)`. This is the only project-source change required beyond adding the module dependency.

## Important runtime notes

- Baking is intentionally game-thread and rendering-thread synchronized. It is suitable for amortized LOD preparation, not for doing dozens of bakes in a single frame.
- `bDisableSourceCollisionAfterBake` removes source collision only after the impostor becomes fully active at distance; collision is restored during the near-side blend.
- For many glTF objects, queue bakes one at a time to avoid a spike in Scene Capture and render-target allocations.
- The capture is a color/alpha snapshot. Animated or deforming source actors require a re-bake when their appearance must change.
- The atlas is kept as a transient `UTextureRenderTarget2D`. If you need persistence across sessions, add a project-specific capture/export step.

## Copyright boundary

This package does not redistribute or port source code/assets from the supplied proprietary Unity package. It is a fresh Unreal implementation of the underlying technique using engine-native APIs.
## UE 5.8.1 dependency notes

This package is verified against the UE 5.8 module naming used by the stock ProceduralMeshComponent plugin.
The implementation includes `ProceduralMeshComponent.h` directly (not `Components/ProceduralMeshComponent.h`).
The `.uplugin` explicitly declares both `ProceduralMeshComponent` and `glTFRuntime` plugin dependencies so UnrealBuildTool
does not report a missing plugin dependency when the module is compiled.

The warning about `Plugins/glTFRuntime/glTFRuntime.uplugin` saying its engine version string `"5.8"` could not be parsed
is separate from RuntimeImpostors; it originates in the supplied glTFRuntime descriptor. It does not cause the compiler error
shown in the build log.

