# UE5-glTFSimulator

UE5-glTFSimulator is an Unreal Engine 5 C++ project for loading, streaming, exploring, editing, and saving glTF-based 3D scenes at runtime.

The project is built around runtime glTF ingestion through `glTFRuntime`, distance-based model streaming, a controllable skeletal character, simple world management, and an in-game creator workflow for placing prefabs, creating procedural meshes, vehicles, and weapons.

## Highlights

- Runtime loading of `.glb` and `.gltf` assets through the bundled `glTFRuntime` plugin.
- Large-model streaming using node distance checks, instanced static meshes, async loading, and automatic unload boxes.
- LOD-aware mesh loading using model-node naming conventions.
- Runtime material overrides for common simulation materials such as glass, tinted glass, and terrain.
- Optional per-model JSON metadata for collision, entity flags, and runtime point lights.
- Skeletal character loading from glTF with bone-name mapping, default skeleton merging, generated physics, ragdoll blending, walking, crouching, sprinting, flying, swimming, and first-person toggle support.
- Runtime creator mode with a 7-slot toolbar, prefab placement, procedural mesh editing, vehicle placement, weapon equipment, grid snap, and item-list UI integration.
- Runtime scene persistence to `runtime_installed.json` plus a glTF export file.
- World metadata persistence through `level.json` with automatic periodic saving.

## Requirements

| Requirement | Notes |
| --- | --- |
| Unreal Engine | The project file is configured with `EngineAssociation: 5.7`. Use the matching UE version, or update the `.uproject` only after validating plugin compatibility. |
| C++ toolchain | Required because the main simulator systems are implemented as a C++ UE module. |
| Bundled plugins | `glTFRuntime`, `ShaderLibrary`, and `glTFSimulatorEditor` are included under `glTFSimulator/Plugins`. |
| Built-in UE plugins | `ProceduralMeshComponent` and `ModelingToolsEditorMode` are enabled by the project. |

## Repository layout

```text
.
├── README.md
├── LICENSE
├── glTFSimulator/
│   ├── glTFSimulator.uproject
│   ├── Config/
│   ├── Content/
│   │   ├── Blueprints/
│   │   ├── Input/
│   │   ├── Maps/
│   │   └── Resources/
│   ├── Plugins/
│   │   ├── glTFRuntime/
│   │   ├── ShaderLibrary/
│   │   └── glTFSimulatorEditor/
│   └── Source/
│       └── glTFSimulator/
│           ├── Public/
│           └── Private/
```

Important maps:

- `Content/Maps/StartWorld.umap` is configured as both the editor startup map and game default map.
- `Content/Maps/MainWorld.umap` contains the main simulation world flow.

## Getting started

1. Open `glTFSimulator/glTFSimulator.uproject` with the configured Unreal Engine version.
2. Let Unreal rebuild project modules if prompted.
3. If project files are missing, regenerate them from the `.uproject` file and build the `glTFSimulator` target.
4. Open or play from `StartWorld`.
5. Create a save-world folder under the simulator runtime data directory, then place glTF files in the expected subfolders described below.

## Runtime data directory

The simulator reads and writes world data under the user's platform-specific user directory:

```text
<UserDir>/glTFSimulator/SaveData/<WorldFolder>/
```

At runtime, `<WorldFolder>` is the current world folder name. `StartWorld` scans the folders under `SaveData` and reads each folder's `level.json` to display available worlds.

A typical world folder looks like this:

```text
<UserDir>/glTFSimulator/SaveData/DemoWorld/
├── level.json
├── model/
│   ├── city.glb
│   └── city.json
├── player/
│   ├── avatar.glb
│   └── avatar.json
├── prefab/
│   └── table.glb
├── items/
│   ├── rifle.glb
│   └── rifle.json
├── generated/
├── runtime_installed.json
└── runtime_installed.gltf
```

The project also scans these project-relative fallback folders for runtime creator assets:

```text
<ProjectDir>/World/prefab/
<ProjectDir>/World/items/
<ProjectDir>/World/<WorldFolder>/prefab/
<ProjectDir>/World/<WorldFolder>/items/
```

## `level.json`

`level.json` stores world settings, time settings, ocean state, player spawn location, and the optional player glTF file name.

Example:

```json
{
  "WorldName": "Demo World",
  "WorldTime": 0.0,
  "Latitude": 38.0,
  "Longitude": 127.0,
  "AxialTilt": 23.5,
  "OneYearDays": 365.0,
  "OneDayTime": 86400.0,
  "TimeSpeed": 60.0,
  "bOcean": true,
  "X": 0.0,
  "Y": 0.0,
  "Z": 0.0,
  "Player": "avatar.glb"
}
```

Notes:

- World models are loaded from `model/*.glb`.
- The player model is loaded from `player/<Player>`. If loading fails or `Player` is empty, the character falls back to the default mesh.
- World data is saved periodically while the simulation is running.

## Adding static world models

Place `.glb` files in:

```text
<UserDir>/glTFSimulator/SaveData/<WorldFolder>/model/
```

For each `example.glb`, the simulator looks for `example.json` beside it. If the JSON file is missing, a default metadata file is generated automatically.

### Model metadata JSON

The sidecar JSON can override collision behavior, simple colliders, entity flags, and runtime point lights per mesh name.

Minimal generated format:

```json
{
  "X": 0.0,
  "Y": 0.0,
  "Z": 0.0,
  "MeshData": {}
}
```

Example with mesh-specific metadata:

```json
{
  "X": 0.0,
  "Y": 0.0,
  "Z": 0.0,
  "MeshData": {
    "Wall": {
      "ComplexCollision": true,
      "SimpleCollision": false,
      "IsEntity": false,
      "Colliders": [],
      "Lights": []
    },
    "Lamp": {
      "ComplexCollision": false,
      "SimpleCollision": true,
      "IsEntity": true,
      "Colliders": [
        {
          "Type": "Box",
          "X": 0.0,
          "Y": 0.0,
          "Z": 0.0,
          "DX": 30.0,
          "DY": 30.0,
          "DZ": 80.0
        }
      ],
      "Lights": [
        {
          "X": 0.0,
          "Y": 0.0,
          "Z": 120.0,
          "Unit": "Candelas",
          "Intensity": 500.0,
          "SourceRadius": 10.0,
          "SoftSourceRadius": 10.0,
          "AttenuationRadius": 1000.0,
          "Length": 10.0
        }
      ]
    }
  }
}
```

Supported simple collider types are `Box`, `Sphere`, and `Capsule`.

## glTF mesh naming conventions

The loader uses the text before the first semicolon (`;`) as the shared mesh key. Suffixes after the semicolon define loading behavior.

| Suffix | Purpose |
| --- | --- |
| No suffix or `;LOD0` | Primary mesh / LOD0. |
| `;INST` | Instance node that reuses the primary mesh with the same prefix. |
| `;LOD1` | LOD1 mesh for the same prefix. |
| `;LOD2` | LOD2 mesh for the same prefix. |
| `;LOD3` | LOD3 mesh for the same prefix. |
| `;NCOL` | Disables both complex and simple collision for that mesh key. Can be combined with other suffixes. |

Example:

```text
Building
Building;INST
Building;INST.001
Building;LOD1
Building;LOD2
Building;NCOL
```

In this example, `Building;INST` and `Building;INST.001` reuse the mesh data from `Building`, while `Building;LOD1` and `Building;LOD2` are loaded as lower-detail LODs for the same logical mesh.

Recommendations:

- Always keep one primary mesh for each instanced prefix.
- Use `;INST` for repeated nodes to reduce duplicated mesh loading.
- Use LOD suffixes only for alternate meshes that should not be spawned as separate world objects.
- Keep naming consistent between the `.glb` node names and the sidecar JSON `MeshData` keys.

## Reserved material names

The streaming loader overrides specific material names with simulator materials.

Use these exact material names in exported glTF files when you want the simulator override:

```text
glass
tinted_glass
terrain
```

General glTF material types are also mapped to the simulator's default opaque, two-sided, translucent, and two-sided translucent materials.

## Skeletal character models

Player models are loaded from:

```text
<UserDir>/glTFSimulator/SaveData/<WorldFolder>/player/
```

A character file can have an optional sidecar JSON file with the same base name. The sidecar JSON maps simulator target bone names to source glTF bone names.

Example:

```json
{
  "Root": "Root",
  "hips": "mixamorig:Hips",
  "spine": "mixamorig:Spine",
  "chest": "mixamorig:Spine1",
  "neck": "mixamorig:Neck",
  "head": "mixamorig:Head",
  "leftUpperLeg": "mixamorig:LeftUpLeg",
  "rightUpperLeg": "mixamorig:RightUpLeg",
  "leftFoot": "mixamorig:LeftFoot",
  "rightFoot": "mixamorig:RightFoot",
  "hairRoot": "hairRoot",
  "dynRoot": "dynRoot"
}
```

Important target bones used by gameplay code include:

```text
Root
hips
neck
head
leftUpperLeg
rightUpperLeg
leftFoot
rightFoot
hairRoot
dynRoot
```

Guidelines:

- The root bone is expected to be named `Root`; the loader can add it if the source asset is missing it.
- Use a bone-map JSON when the source glTF uses different names, such as Mixamo-style names.
- Keep `hips`, `head`, `neck`, upper-leg, and foot bones valid because movement, ragdoll recovery, water checks, and foot traces depend on them.
- `hairRoot` and `dynRoot` are used for generated physics/collider setup below those bones.
- Invalid or incomplete skeletons may fail to load and fall back to the default character mesh.

## Runtime creator mode

The runtime gameplay manager supports creator-mode interactions driven by UI buttons and input events:

- Place prefabs from `prefab/`.
- Create procedural mesh objects by placing and connecting vertices.
- Edit existing generated meshes.
- Place runtime vehicles.
- Equip and fire weapons from `items/`.
- Toggle grid snap and change toolbar slots.
- Save placed runtime objects and generated meshes.

Runtime creator assets are scanned from both the save-world folders and project-relative `World/` folders.

```text
prefab/  -> placeable `.glb` or `.gltf` prefabs
items/   -> weapon/item `.glb` or `.gltf` files
```

Runtime scene saving writes:

```text
runtime_installed.json
runtime_installed.gltf
```

The JSON manifest is used for reloading objects inside the simulator. The `.gltf` file is a lightweight export of generated meshes and placed-object metadata.

## Default fallback controls

Enhanced Input assets can override or extend controls, but the project keeps fallback key bindings so the simulator remains usable when input assets are not assigned.

| Action | Fallback input |
| --- | --- |
| Move | `W`, `A`, `S`, `D` |
| Look | Mouse X/Y |
| Jump | `Space` |
| Sprint | `Left Shift` |
| Crouch | `Left Ctrl` |
| Pause | `Esc` |
| Runtime primary action / placement | Left mouse button press and release |
| Runtime secondary action / finish vertex edit | Right mouse button |
| Enter or exit vehicle | `F` |
| Toggle first-person view | `V` |
| Scroll toolbar | Mouse wheel |
| Open or close item list | `E` |
| Toggle snap | `G` |

## Weapon item JSON

Weapons can optionally use a JSON file beside the item glTF. The runtime weapon actor reads hold and muzzle settings from this file.

Example:

```json
{
  "Hold": {
    "X": 45.0,
    "Y": 18.0,
    "Z": -18.0,
    "Pitch": 0.0,
    "Yaw": 0.0,
    "Roll": 0.0,
    "Scale": 1.0
  },
  "Muzzle": {
    "X": 70.0,
    "Y": 0.0,
    "Z": 0.0
  },
  "Range": 20000.0,
  "Damage": 20.0,
  "FireInterval": 0.12
}
```

## Notes for asset preparation

- Prefer `.glb` for world models in `model/`, because the world loader scans that folder for `.glb` files.
- `.glb` and `.gltf` are both supported for runtime creator `prefab/` and `items/` folders.
- External glTF resources are allowed by the runtime loader, but packaged `.glb` files are easier to move between world folders.
- This repository's `.gitignore` ignores `*.glb`, so large runtime assets are expected to live outside normal source control unless the ignore rules are changed.
- Keep model sidecar JSON files valid UTF-8 JSON.

## Troubleshooting

### A world does not appear in the start menu

Check that the folder exists under:

```text
<UserDir>/glTFSimulator/SaveData/
```

and that it contains a readable `level.json` with a `WorldName` field.

### A model does not stream in

Check the following:

- The file is a `.glb` inside `<WorldFolder>/model/`.
- The primary mesh exists before using `;INST` nodes.
- The sidecar JSON, if present, uses mesh keys that match the prefix before `;`.
- The player is close enough to the node for the stream-distance check.

### Instanced meshes do not appear

Make sure there is a primary mesh with the same prefix. For example, `Tree;INST.001` expects a primary mesh key named `Tree`.

### Character loading fails

Check the character sidecar JSON and ensure the important gameplay bones are present or mapped. If the custom mesh fails, the project falls back to the default character mesh so the world can continue loading.

## License

This project is licensed under the MIT License. See [`LICENSE`](LICENSE) for details.

## Acknowledgements

- `glTFRuntime` by Roberto De Ioris is used for runtime glTF loading.
- Unreal Engine, Epic Games, and the built-in UE plugin ecosystem provide the underlying rendering, physics, input, UI, and procedural mesh systems.
