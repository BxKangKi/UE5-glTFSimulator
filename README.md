# UE5-glTFSimulator

This `Source` tree contains the glTFRuntime-based world build/streaming code. The current world storage architecture separates the **authoring project** from the **runtime world**.

## User Data Folder Structure

The runtime root is `FPlatformProcess::UserDir()/glTFSimulator`.

```text
glTFSimulator/
├─ Logs/                         # Existing logs
├─ Projects/                     # Authoring/build input
│  └─ MyProject/
│     ├─ config.json
│     └─ resources/
│        ├─ City.glb
│        ├─ City.json            # Model definition for City.glb
│        └─ ...                  # Recursive subfolder search is supported
├─ Worlds/                       # Runtime worlds searched by Singleplay / Multiplay
│  ├─ MyProject.gwd              # Immutable, shareable, read-only world data
│  └─ Data/
│     └─ MyProject.dat           # Mutable state such as time/player/dynamic entities
└─ settings.json
```

`Projects` is read **only during builds**. The normal game startup path does not fall back to `Projects` or the original GLB files and opens only `Worlds/<World>.gwd`. Therefore, authoring-side `Projects` data does not need to be shipped with a deployed build.

During a project build, `config.json` is copied into the `.gwd` as an independent CRC-validated member. The `Worlds/*.gwd` list uses the embedded config's `WorldName` as its display name.

## GameInstance / Central Asset Registry

Asset/class/world references that were previously scattered across multiple Actor/Controller Blueprints are consolidated into a single `UGlTFSimulatorAssetRegistry`. The configuration path now uses **only one Registry Class**.

1. Create a Blueprint Class derived from `UGlTFSimulatorAssetRegistry`.
2. In that Blueprint Class's Class Defaults, assign actors, UI, worlds, meshes, materials, input assets, and other resources.
3. In a GameInstance Blueprint derived from `UGlTFSimulatorGameInstance`, assign only that Registry Blueprint Class to `AssetRegistryClass`.
4. Set that GameInstance as the Game Instance Class in Project Settings.

The GameInstance no longer exposes an external `AssetRegistry` instance variable for configuration. At runtime, it internally creates one transient registry instance from `AssetRegistryClass` and keeps it only through a private strong reference.

Actor classes, WBP classes, worlds, meshes, materials, Character assets, InputActions, and IMCs inside the Registry use `TSoftObjectPtr`/`TSoftClassPtr`. As a result, loading the Registry itself does not make all referenced packages resident in memory. Assets are resolved only when the relevant system actually needs them, and resolved UObjects are strongly held only for the required lifetime through the consuming Actor/Subsystem's `UPROPERTY`/`TObjectPtr`/`TSubclassOf`.

The recommended setup order is as follows.

1. Create one Blueprint Registry derived from `UGlTFSimulatorAssetRegistry`.
2. Assign the Static/Dynamic/Vehicle/Weapon/Projectile/WorldEnv/Water/Weather Actor classes, along with `MainWorld` and the gameplay/host/client worlds.
3. Assign each C++ GameMode or its Blueprint subclass to `SingleplayGameModeClass` and `MultiplayGameModeClass`. If left empty, the native GameMode is used as a fallback.
4. Assign `SkyboxMesh`, Cloud/Water assets, Character SkeletalMesh/PhysicsAsset/Skeleton/Material/**AnimInstanceClass**, Weapon/glTF materials, InputActions, and IMCs to the same Registry.
5. Also assign the Start/WorldSelection/Project/Pause/Settings/CreatorHUD/Debug/Loading WBP classes to the Registry.
6. Assign the Registry Blueprint Class to `AssetRegistryClass` in a Blueprint derived from `UGlTFSimulatorGameInstance`.
7. Select that GameInstance Blueprint in Project Settings. Assign `AMainGameMode` to MainWorld and `ASingleplayGameMode`/`AMultiplayGameMode` to gameplay worlds. Registry-based initialization will then proceed automatically.

The `Skybox` component of `AWorldEnvManager` itself is created in C++, while the StaticMesh it uses is loaded from the Registry's `SkyboxMesh` only when needed at BeginPlay. `ACharacterController` likewise applies the Registry's default SkeletalMesh/PhysicsAsset/Material and `DefaultCharacterAnimInstanceClass` during BeginPlay. When replacing the character mesh with a glTF character mesh, the current AnimBP state is temporarily preserved and restored after the mesh swap so the AnimInstance configuration is not lost during loading.

`AStaticActor`, `ADynamicActor`, `AWorldEnvManager`, `AWaterActor`, `AWeaponActor`, `ACharacterController`, `APlayerCharacterController`, and related classes no longer own their own editor asset slots. Required assets are obtained from the Registry. If the Registry is missing or an invalid class is assigned, native-class fallbacks are used where possible, while required functionality emits explicit logs.

## GameMode-Based Initialization and Actor Spawn Ownership

The initialization lifecycle is no longer owned by level-placed `StartActor`/`GameManagerActor` instances. Responsibilities are divided among three C++ GameModes.

- `AMainGameMode`: Owns the MainWorld menu state, world list, Singleplay/Multiplay travel, and Settings transitions. Projects are also built inside MainWorld by toggling the visibility of the registered Project WBP instead of traveling to another world.
- `ASingleplayGameMode`: Owns the authority lifecycle of the Singleplay gameplay world and starts/stops the shared gameplay session.
- `AMultiplayGameMode`: Owns the gameplay lifecycle for listen/dedicated servers. Because GameMode does not exist on clients, the PlayerController starts only the render-side session using the replicated GameMode class/defaults and world state.
- `AGlTFSimulatorGameplayGameModeBase`: Provides only the common grid/placement/save settings for Singleplay/Multiplay and the connection to the `UGameManagerSubSystem` session lifecycle.

Set **World Settings → GameMode Override** in MainWorld to `AMainGameMode` or one of its Blueprint subclasses. The shared gameplay map uses the Registry's `SingleplayGameModeClass` / `MultiplayGameModeClass` as travel overrides. The old `StartActor`, `GameManagerActor`, and `WorldBootstrapSubsystem` sources, along with the automatic actor-spawn fallback, have been removed. If a `.gwd` world is opened with an incorrect gameplay GameMode, the system logs a configuration error instead of silently spawning a replacement manager actor.

Replaceable Actor classes in the Registry are instantiated only when they are actually needed. Their current ownership paths are as follows.

- `WorldEnvManagerClass`: Ensured by the gameplay GameMode when the session starts, before world-data I/O begins. A placed actor is reused if present; if the configured Blueprint fails to spawn, the native `AWorldEnvManager` is used as a fallback.
- `StaticActorClass`: Created when needed for WorldStream distance streaming, `.dat` Static chunk restoration, Creator Static placement, or Held Static placement.
- `DynamicActorClass`: Created only when a general Dynamic chunk from `.dat` is loaded.
- `VehiclePawnClass`: Created when restoring a Vehicle chunk or placing a vehicle through Creator.
- `WeaponActorClass`: Created only when the user equips a weapon.
- `WeaponProjectileActorClass`: Created only when a projectile weapon actually fires.
- `WaterActorClass`: Created only in worlds with an enabled ocean or when a Static `;WATER` node is actually loaded.
- `RainWeatherActorClass`: Created only when the weather system is active and the preset is not `clear`; it is removed when weather is cleared/stopped.

Unlike the classes above, `ACharacterController` is the Pawn owned by the GameMode's `DefaultPawnClass`. In other words, the Registry does not spawn duplicate character instances on its own. The central Registry provides only the Character content configuration such as mesh, physics asset, material, and AnimInstance.

Configured Blueprint Actor classes are validated immediately before use for base-class compatibility, abstract status, and deprecation. For key creation paths such as Static/Dynamic/Vehicle/Weapon/WorldEnv, the system retries with the native class if spawning the Blueprint itself fails. GameMode is not replaced with a separate manager actor; the correct GameMode configuration is required.

## Actor Terminology and Roles

The runtime model uses three major categories: `Static`, `Dynamic`, and `Character`. The previous terms `WorldSceneActor` and placeable `PrefabActor` have been removed, and static model representation is unified under `AStaticActor`. Existing WorldStream hierarchy names such as `UWorldSceneStreamingSubsystem` and `UWorldSceneStreamAction` remain unchanged because they describe storage/streaming responsibilities.

The previous path in which `PrefabActor` also handled some generic Dynamic models has been separated into `ADynamicActor`. As a result, `AStaticActor` handles only Static/WorldStream representation, while `ADynamicActor` handles physics/collision proxies for general Dynamic models that are not Vehicles. Vehicles continue to use the existing `AVehiclePawn` path, and Characters continue to use the existing Character runtime path.

## UI Lifetime Rules and Shared Settings

Top-level UI is automatically created by C++ based on Registry classes. `AMainGameMode` creates the Start/WorldSelection/Multiplayer/Project/Settings widgets, while `APlayerCharacterController` creates the CreatorHUD/Debug/Pause/Settings widgets. The Loading widget is created immediately by `UGameManagerSubSystem` when needed, and the same instance is reused even if the PlayerController starts first.

Blueprints are primarily responsible for **layout and wiring internal widgets**. For example, a WBP passes its internal Button/Panel references to setters such as `SetStartButton()`, `SetProjectsButton()`, and `SetProjectListPanel()` so that native delegates and list logic can be connected. A separate `CreateWidget → AddToViewport → Set...Widget` BeginPlay graph is not required in the default configuration.

Registration functions such as `SetStartMenuWidget`, `SetPauseMenuWidget`, and `SetSettingsMenuWidget` are retained for compatibility/custom overrides rather than removed. If a Blueprint registers a specific instance first in `ReceiveBeginPlay`, C++ automatic initialization does not overwrite that slot.

Both Start and Pause Settings can use the **same WBP class derived from `USettingsMenuWidget`**. A separate instance is automatically created for each context, but the Apply/Confirm/Back/Cancel behavior, saved-value loading, and close-delegate contract remain identical.

### GameMode / World Configuration

1. In MainWorld's World Settings, set GameMode Override to `AMainGameMode` or one of its Blueprint subclasses. The Registry's menu WBP classes are then created automatically.
2. The Singleplay gameplay map uses the `ASingleplayGameMode` hierarchy, while the multiplayer host/server map uses the `AMultiplayGameMode` hierarchy. If a travel override is configured in the Registry, that class is used.
3. `AGlTFSimulatorGameplayGameModeBase` uses `APlayerCharacterController` and `ACharacterController` as its native defaults, so a standard simulator setup does not need to reassign these two classes in a Blueprint GameMode.
4. Pause Exit returns to the single `MainWorld` configured in the Registry. `MainGameMode` consumes the request flag and shows the WorldSelection UI inside the same MainWorld. Separate WorldSelectionWorld/MainMenuWorld/BuildWorld maps are not used.

## Projects Screen Configuration

Projects do not use a separate Unreal world. If the Registry's `ProjectSelectionWidgetClass` is configured, `AMainGameMode` automatically creates it when MainWorld starts and registers it with the Start WBP.

1. The main-menu WBP inherits from `UStartWorldWidget` and passes the Projects button to `SetProjectsButton()`.
2. The project WBP inherits from `UProjectSelectionWidget` and is assigned to the Registry's `ProjectSelectionWidgetClass`. No separate Blueprint bootstrap creation/registration is required.
3. The project WBP passes a `UPanelWidget`, such as a ScrollBox/VerticalBox, to `SetProjectListPanel()` and passes the Back button to `SetBackButton()`. If a Refresh button exists, `SetRefreshButton()` can also be used.
4. The Projects button changes the registered Project WBP to `Visible` and the main WBP to `Collapsed`. Back changes the Project WBP to `Collapsed` and restores the main WBP. C++ does not call `RemoveFromParent()` on top-level widgets.
5. Only repeated project entries inside `ProjectListPanel` are built natively from transient buttons/text.

Among entries under `Projects/*`, only those that contain both `config.json` and `resources/`, and whose `config.json` contains a non-empty `WorldName`, are displayed automatically. Clicking a project entry calls `UGameManagerSubSystem::BuildProjectByName()`, which validates `Projects/<Project>` and generates `Worlds/<Project>.gwd`. A single-build guard prevents two projects from being prepared/built at the same time.

## Model JSON

> For the complete current schema, Dynamic/Vehicle/Weapon options, `MeshData`, and collision/light formats, refer to [`MODEL_JSON_GUIDE.md`](MODEL_JSON_GUIDE.md). The section below summarizes only the core classifications.

Every GLB is paired with a JSON file with the same basename in the same folder. Example: `Building.glb` ↔ `Building.json`. If the JSON file does not exist, a UUID is generated during build preparation and a default `ModelType: "Static"` definition is created atomically.

### Static

Replaces the previous `Scene` type. Used for static models placed in the world.

```json
{
  "UUID": "3f8434b5-f21a-48ce-9542-cbcf30c78b03",
  "Name": "Building_A",
  "DisplayName": "Building A",
  "ModelType": "Static"
}
```

### Dynamic

The parent type replacing the previous `Entity` and `Item` types. Exactly **one** of `EntityType` or `ItemType` may optionally be specified.

```json
{
  "UUID": "4638205b-4b21-4cd0-98cb-2f4a3b7ca329",
  "Name": "Sedan",
  "DisplayName": "Sedan",
  "ModelType": "Dynamic",
  "EntityType": "Vehicle"
}
```

Supported `EntityType` values are `Vehicle`, `Prop`, and `Animal`.

```json
{
  "UUID": "031956f7-9844-4445-9e03-3c75ab6c2820",
  "Name": "Hammer",
  "DisplayName": "Hammer",
  "ModelType": "Dynamic",
  "ItemType": "Tool"
}
```

Supported `ItemType` values are `Weapon`, `Tool`, and `Misc`. Definitions that specify both `EntityType` and `ItemType` are rejected during validation.

### Character

Character definitions require a `Bones` object. In JSON, use the form **semantic alias → actual GLB bone name**.

```json
{
  "UUID": "8ebcf0ea-390e-4077-8ee6-a8b595593fbf",
  "Name": "Human_A",
  "DisplayName": "Human A",
  "ModelType": "Character",
  "Bones": {
    "Hips": "mixamorig:Hips",
    "Head": "mixamorig:Head",
    "LeftHand": "mixamorig:LeftHand",
    "RightHand": "mixamorig:RightHand"
  }
}
```

For rebuilding older projects, read compatibility is retained for `Scene → Static`, `Entity → Dynamic`, `Item → Dynamic`, and `None/missing → Static`. New canonical JSON and new projects should use only `Static`, `Dynamic`, and `Character`.

## glTF Node Tokens and `;INST`

Node directives are expressed as semicolon-separated tokens. For example:

```text
Tree
Tree;LOD0
Tree;LOD1
Tree;LOD2
Tree;INST
Tree;LOD0;INST
```

`INST` belongs to a separate token family from LOD tokens, so it can be combined as in `;LOD0;INST`. Substring/underscore aliases such as `_INST` are not recognized as tokens.

During a build, `;INST` nodes use `BaseName + LOD` as the canonical key. If a non-INST mesh with the same key exists, its payload is shared. For LOD0, an explicit `Base;LOD0` has higher priority as the canonical candidate than the bare `Base`. Therefore, duplicate mesh payloads belonging to `Tree;INST` itself are removed from the bake list, while the node transforms remain in the static placement metadata.

Important details:

- The **reference position of `Tree` or `Tree;LOD0` is also preserved as an actual placement**.
- Each `Tree;INST` position/rotation/scale is preserved as an individual placement.
- `LOD1`–`LOD3` nodes are used only for LOD mesh definitions and do not create separate world placements.
- Supported LODs are `LOD0`–`LOD3`; numeric LOD tokens `LOD4` and above are rejected as build errors.
- Even if the same glTF node name appears multiple times, all nodes are preserved by NodeIndex, while runtime map keys are safely uniquified.
- For GLBs with parent/child hierarchies, the placement stores the model-space transform produced by composing all parent transforms.

When possible, provide an explicit non-INST `Base` or `Base;LOD0` reference mesh for `;INST`. An independent `;INST` mesh with no canonical reference is not arbitrarily merged with unrelated geometry.

## 8192 m / 512 m Hierarchical Static Streaming

Because Unreal units are centimeters, the following cell sizes are used during builds.

- coarse cell: `8192 m = 819200 cm`
- fine cell: `512 m = 51200 cm`
- fine cells per coarse-cell axis: 16

Each Static placement, including `;INST`, stores its `CoarseChunk` and its coarse-local `FineChunk(0..15)` in advance based on its model-space position. When a runtime ISM group is created, it builds the following native index once.

```text
8192m coarse cell
  └─ 512m fine cell
      └─ node keys
```

Streaming updates do not copy the entire NodeMap on every update. Instead, they snapshot only **nearby coarse/fine candidates + currently loaded nodes + AlwaysLoaded nodes**. Because currently loaded nodes remain in the candidate set, instances that move out of range can still be removed correctly.

If the maximum axis of a transformed mesh AABB, including rotation and non-uniform scale, is **larger than 8192 m, it is classified as `AlwaysLoaded`** and is not removed by distance-based streaming for the lifetime of the scene. If coordinates or scale values are invalid and a safe chunk address cannot be calculated, the system also favors safety over geometry loss and keeps the geometry as a persistent candidate.

### Performance Guide for Map Authors

**Avoid creating one enormous single mesh/node larger than 8192 m.** Such a mesh becomes AlwaysLoaded and cannot effectively benefit from distance-based streaming. Loading it also makes a large amount of vertex/index/material data resident at once.

For assets spanning wide areas, such as cities, terrain structures, long roads/walls, and large industrial facilities, it is better to **split them into multiple glTF nodes and appropriately sized meshes** according to visual/spatial regions. For repeated objects such as trees, streetlights, and columns, use one reference `Base`/`Base;LOD0` mesh and multiple `Base;INST` placements.

## `.gwd` Memory / Disk Structure

A `.gwd` file is **not loaded into RAM in its entirety**.

`FGWorldArchiveReader::Open()` reads and retains only the fixed header and a limited root directory. Large data blocks are stored as separately checksummed ranges. When required, only the relevant range is read through a private file handle, validated, and decompressed.

The main independent members are:

- embedded `config.json`
- model definition JSON / Bones
- node/LOD/chunk metadata
- model manifest
- each mesh payload
- skin payload
- material payload
- texture payload

A mesh request range-reads only the requested mesh and the material/texture/skin dependencies referenced by that mesh. Other models or unrequested mesh payloads are not loaded into memory merely because they are stored in the same `.gwd`.

`gwd://<UUID>` is the runtime model-reference format. `gworld://<UUID>` is retained only for parser compatibility with older saved data/references.

The build process does not copy the source GLB itself into the `.gwd`. Instead, it decodes the GLB once through glTFRuntime and stores Unreal-ready data together with static metadata. Before publishing, it rechecks the source size/timestamp, validates member CRCs, bounds, and the manifest, and uses transactional replacement with a backup when an existing `.gwd` is present so that a failed update preserves the previous valid file.

## `Worlds/Data/<World>.dat`

`.dat` stores data that can change during gameplay. It does not store static resources such as meshes, textures, or materials. World time, selected player/state, and dynamic entity/object chunks are stored here.

Dynamic objects retain the existing append/commit + footer recovery structure. If the process terminates before a normal commit and the tail is truncated, the file is designed to recover to the last valid commit. Multiplayer clients continue to follow the existing server-authority rule and do not write unauthorized local state as the authoritative save.

## UE 5.8 Build Log Notes

The large number of `C4430`, `C2143`, and `GENERATED_BODY()` errors in the provided logs appeared simultaneously across multiple reflected classes and cascaded into secondary errors where `Super::BeginPlay()` was interpreted as `UObject`. Because the log came from a `-ModuleWithSuffix` hot-reload style build, a **clean full rebuild** is recommended after reflected-header changes so stale generated code is not reused.

Recommended sequence:

1. Completely close Unreal Editor and Live Coding.
2. Delete the project's `Binaries/` and `Intermediate/` directories.
3. If stale generated output from source-built plugins is suspected, also delete the plugin's `Binaries/` and `Intermediate/` directories.
4. Regenerate the project files.
5. Perform a full `Development Editor / Win64` build first. Do not use Live Coding/Hot Reload for the initial verification.
6. Launch the Editor only after the full build succeeds.

In addition, in response to UE 5.8 incremental GC warnings, the world-bake request guard's raw `UObject*` storage was changed to `TObjectPtr<UObject>`. Replicated world/model keys that failed because `DOREPLIFETIME` accessed private reflected members were also moved to a protected scope accessible by the macro.

## Stability Principles

- The runtime does not automatically fall back to source GLB files.
- Project building and game-world loading are kept separate.
- Archive/mutable-state file reads validate size/range/CRC/structural limits before consuming data.
- For corrupted chunk metadata, explicit failure or a safe fallback is preferred over silently losing geometry.
- Worker threads process native snapshots only; UObject lifetime transitions are handled on the game thread.
- Unnecessary copying of enormous full node maps/material/texture collections is avoided.

For detailed storage formats, see `WORLD_STREAMING_FORMAT.md`. For a review summary of the current changes, see `IMPLEMENTATION_REVIEW.md`.
