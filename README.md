# glTFSimulator

glTFSimulator is an Unreal Engine 5.8 runtime simulation project built around glTFRuntime. Authoring projects are file-system based: a project is described by `config.json`, and its GLB/WAV assets plus same-basename JSON definitions live recursively under the project's lower-case `resources/` directory. A build converts those authoring files into immutable random-access archives used by runtime streaming.

The runtime archive formats are:

- World project → `glTFSimulator/Worlds/<ProjectFolder>.gworld`
- Character project → `glTFSimulator/Resources/<ProjectName>.gasset`
- Dynamic project → `glTFSimulator/Resources/<ProjectName>.gasset`

Runtime model references use `gworld://model/<UUID>`. Runtime sound references use `gworld://sound/<UUID>`.

## Project layout

`Projects` is authoring input. `Worlds` and the upper-case global `Resources` directory are runtime build outputs/discovery locations. They are intentionally separate.

```text
glTFSimulator/
├─ Projects/
│  └─ MyWorld/
│     ├─ config.json
│     └─ resources/                 # lower-case: authoring input only
│        ├─ city.glb
│        ├─ city.json
│        ├─ ambience.wav
│        ├─ ambience.json
│        └─ subfolders/...          # scanned recursively
├─ Worlds/
│  └─ MyWorld.gworld               # built World archive
├─ Resources/                       # upper-case: built external runtime packs only
│  ├─ MyCharacter.gasset
│  └─ MyDynamicPack.gasset
└─ settings.json
```

A project is discoverable only when both of these exist:

```text
Projects/<ProjectName>/config.json
Projects/<ProjectName>/resources/
```

The Projects UI always inserts `+ Create Project` as the first generated list entry. A project-name input is generated natively when the WBP does not provide one. Creating a project creates the project directory, `config.json`, and `resources/` automatically. For a newly created project, `ProjectType`, `ProjectName`, and `WorldName` are initialized as a World project.

## JSON specifications

All field names and string enum values below are case-sensitive unless stated otherwise. Only the keys and enum values documented below are accepted.

### `Projects/<Project>/config.json`

`ProjectType` accepts exactly `World`, `Character`, or `Dynamic`. If omitted, it defaults to `World`. `ProjectName` defaults to the project folder name when omitted. For a World project, `WorldName` also defaults to the project folder name when omitted.

`bAllowExternalAssets` applies only to World projects. Character and Dynamic asset packs cannot recursively mount other packs.

Minimal World project:

```json
{
  "ProjectType": "World",
  "ProjectName": "MyWorld",
  "WorldName": "MyWorld",
  "bAllowExternalAssets": false
}
```

For a World project, the same `config.json` is embedded in the `.gworld` archive and may also contain the current world configuration:

```json
{
  "Version": "1.0.0",
  "ProjectType": "World",
  "ProjectName": "MyWorld",
  "WorldName": "MyWorld",
  "Latitude": 38.0,
  "Longitude": 127.0,
  "AxialTilt": 23.5,
  "OneYearDays": 365.0,
  "OneDayTime": 86400.0,
  "TimeSpeed": 60.0,
  "bOcean": true,
  "OceanHeightCm": 0.0,
  "bAllowExternalAssets": false,
  "Cloud": {
    "bEnabled": true,
    "Coverage": 0.55,
    "Density": 0.70,
    "Opacity": 1.0,
    "WindSpeed": 1.0,
    "Tint": { "R": 1.0, "G": 1.0, "B": 1.0, "A": 1.0 }
  },
  "Weather": {
    "bEnabled": false,
    "Preset": "Rain",
    "Intensity": 1.0,
    "TickIntervalSeconds": 1.0,
    "bAutoCycle": true,
    "MinDurationTicks": 300,
    "MaxDurationTicks": 1200,
    "ClearWeight": 0.55,
    "RainWeight": 0.35,
    "SnowWeight": 0.10
  },
  "Gameplay": {
    "WorldGameMode": "Default",
    "bCheatsEnabled": false,
    "PlayerMaxHealth": 100.0,
    "PlayerMassKg": 80.0,
    "PlayerPushTractionCoefficient": 0.30
  }
}
```

`Gameplay.WorldGameMode` accepts exactly `Default`, `Creator`, or `RealLife`. Mutable state such as current world time, player location, selected player character, and dynamic entity state is not authored in `config.json`; it is stored separately in the world's runtime `.dat` state.

### Model asset pair: `<name>.glb` + `<name>.json`

Every GLB is paired only with the JSON file that has the exact same basename in the same directory. Asset discovery is recursive below the project's `resources/` directory.

`AssetType` is the top-level asset category. For GLB definitions it may be omitted, in which case it defaults to `Model`. `ModelType` defaults to `Static` when omitted.

Static model:

```json
{
  "AssetType": "Model",
  "UUID": "11111111-1111-1111-1111-111111111111",
  "Name": "building_a",
  "DisplayName": "Building A",
  "ModelType": "Static"
}
```

Dynamic entity model:

```json
{
  "AssetType": "Model",
  "UUID": "22222222-2222-2222-2222-222222222222",
  "Name": "vehicle_a",
  "DisplayName": "Vehicle A",
  "ModelType": "Dynamic",
  "EntityType": "Vehicle"
}
```

For `ModelType: "Dynamic"`, `EntityType` and `ItemType` are mutually exclusive:

- `EntityType`: `Vehicle`, `Prop`, `Animal`
- `ItemType`: `Weapon`, `Tool`, `Misc`

`EntityType` and `ItemType` are invalid for `Static` and `Character` models.

Dynamic Weapon model example:

```json
{
  "Version": "1.0.0",
  "AssetType": "Model",
  "UUID": "33333333-3333-3333-3333-333333333333",
  "Name": "rifle_a",
  "DisplayName": "Rifle A",
  "ModelType": "Dynamic",
  "ItemType": "Weapon",
  "AttachSocketName": "rightHand",
  "HoldTransform": {
    "X": 45.0, "Y": 18.0, "Z": -18.0,
    "Pitch": 0.0, "Yaw": 0.0, "Roll": 0.0,
    "ScaleX": 1.0, "ScaleY": 1.0, "ScaleZ": 1.0
  },
  "RightHandIK": {
    "X": 20.0, "Y": 8.0, "Z": -4.0,
    "Pitch": 0.0, "Yaw": 0.0, "Roll": 0.0,
    "Scale": 1.0
  },
  "LeftHandIK": {
    "X": 65.0, "Y": -9.0, "Z": -4.0,
    "Pitch": 0.0, "Yaw": 0.0, "Roll": 0.0,
    "Scale": 1.0
  },
  "MuzzleOffset": { "X": 95.0, "Y": 0.0, "Z": 0.0 },
  "Range": 20000.0,
  "Damage": 20.0,
  "ImpactImpulse": 24000.0,
  "FireInterval": 0.12,
  "TraceRadius": 0.0,
  "bProjectile": false,
  "ProjectileSpeed": 6500.0,
  "ProjectileLifeSeconds": 5.0
}
```

A transform object supports `X`, `Y`, `Z`, `Pitch`, `Yaw`, `Roll`, and either uniform `Scale` or `ScaleX`/`ScaleY`/`ScaleZ`. `MuzzleOffset` accepts an `{X,Y,Z}` object or a three-number array.

### Sound asset pair: `<name>.wav` + `<name>.json`

A Sound is exactly one WAV and one JSON file with the same case-sensitive basename in the same directory. For example:

```text
resources/audio/ambient_forest.wav
resources/audio/ambient_forest.json
```

The JSON schema is:

```json
{
  "AssetType": "Sound",
  "UUID": "44444444-4444-4444-4444-444444444444",
  "Name": "ambient_forest",
  "DisplayName": "Ambient Forest"
}
```

`AssetType: "Sound"` is mandatory for Sound JSON. If a WAV has no sibling JSON, the builder may generate the JSON with a new UUID and the WAV basename as `Name`/`DisplayName`.

The runtime decoder accepts RIFF/WAVE data containing PCM 8/16/24/32-bit or IEEE float32 samples, 1-8 channels, and an 8,000-384,000 Hz sample rate. Audio is decoded to PCM16 for `USoundWaveProcedural` at runtime.

A GLB and WAV cannot share the same basename in the same directory because both would require the same sibling JSON file. Rename one of the assets before building.

### Character model JSON

A Character model requires `ModelType: "Character"` and a `Bones` object containing exactly the 55 canonical keys listed below. Each JSON key is the canonical glTFSimulator bone name and each value is the source bone name in the imported GLB.

```json
{
  "AssetType": "Model",
  "UUID": "55555555-5555-5555-5555-555555555555",
  "Name": "character_a",
  "DisplayName": "Character A",
  "ModelType": "Character",
  "Bones": {
    "Root": "root",
    "hips": "hips",
    "spine": "spine",
    "chest": "chest",
    "upperChest": "upperChest",
    "neck": "neck",
    "head": "head",
    "leftEye": "leftEye",
    "rightEye": "rightEye",
    "leftShoulder": "leftShoulder",
    "leftUpperArm": "leftUpperArm",
    "leftLowerArm": "leftLowerArm",
    "leftHand": "leftHand",
    "rightShoulder": "rightShoulder",
    "rightUpperArm": "rightUpperArm",
    "rightLowerArm": "rightLowerArm",
    "rightHand": "rightHand",
    "leftUpperLeg": "leftUpperLeg",
    "leftLowerLeg": "leftLowerLeg",
    "leftFoot": "leftFoot",
    "leftToes": "leftToes",
    "rightUpperLeg": "rightUpperLeg",
    "rightLowerLeg": "rightLowerLeg",
    "rightFoot": "rightFoot",
    "rightToes": "rightToes",
    "leftThumbProximal": "leftThumbProximal",
    "leftThumbIntermediate": "leftThumbIntermediate",
    "leftThumbDistal": "leftThumbDistal",
    "leftIndexProximal": "leftIndexProximal",
    "leftIndexIntermediate": "leftIndexIntermediate",
    "leftIndexDistal": "leftIndexDistal",
    "leftMiddleProximal": "leftMiddleProximal",
    "leftMiddleIntermediate": "leftMiddleIntermediate",
    "leftMiddleDistal": "leftMiddleDistal",
    "leftRingProximal": "leftRingProximal",
    "leftRingIntermediate": "leftRingIntermediate",
    "leftRingDistal": "leftRingDistal",
    "leftLittleProximal": "leftLittleProximal",
    "leftLittleIntermediate": "leftLittleIntermediate",
    "leftLittleDistal": "leftLittleDistal",
    "rightThumbProximal": "rightThumbProximal",
    "rightThumbIntermediate": "rightThumbIntermediate",
    "rightThumbDistal": "rightThumbDistal",
    "rightIndexProximal": "rightIndexProximal",
    "rightIndexIntermediate": "rightIndexIntermediate",
    "rightIndexDistal": "rightIndexDistal",
    "rightMiddleProximal": "rightMiddleProximal",
    "rightMiddleIntermediate": "rightMiddleIntermediate",
    "rightMiddleDistal": "rightMiddleDistal",
    "rightRingProximal": "rightRingProximal",
    "rightRingIntermediate": "rightRingIntermediate",
    "rightRingDistal": "rightRingDistal",
    "rightLittleProximal": "rightLittleProximal",
    "rightLittleIntermediate": "rightLittleIntermediate",
    "rightLittleDistal": "rightLittleDistal"
  }
}
```

The 55 source-bone values must be non-empty and unique. Extra or missing canonical keys are rejected. After remapping, the canonical reference hierarchy and reference rotations must match the project's target character skeleton.

Optional secondary-motion subtrees are not part of the required 55-key map. If present, they use the exact root names `hairRoot` and `dynRoot`. Chaos keeps those root bodies kinematic, simulates descendants, preserves authored colliders/constraints, and generates fallback bodies/constraints only where an authored entry is missing.

### `glTFSimulator/settings.json`

`settings.json` stores the current local rendering and streaming settings. The current schema is:

```json
{
  "Version": "1.0.0",
  "BloomIntensity": 0.675,
  "BloomThreshold": -1.0,
  "AmbientOcclusionIntensity": 0.5,
  "Exposure": -11.0,
  "ShadowQuality": 2,
  "TextureQuality": 2,
  "MaxTextureResolution": 768,
  "ViewDistanceQuality": 2,
  "StreamingDistanceMultiplier": 64.0,
  "StreamingUnloadDistanceMultiplier": 1.1,
  "ObjectStreamingRadiusMeters": 2048.0,
  "StreamingSceneSpawnBudget": 32,
  "StreamingNodeBudgetPerFrame": 256,
  "AntiAliasingQuality": 2,
  "PostProcessingQuality": 2,
  "EffectsQuality": 2,
  "FoliageQuality": 2,
  "ShadingQuality": 2,
  "GlobalIlluminationQuality": 2,
  "ReflectionQuality": 2,
  "DynamicGlobalIlluminationMethod": 1,
  "ReflectionMethod": 1,
  "bRayTracing": true,
  "bHeightFog": true,
  "bCloud": true,
  "CelShadingMode": 1.0
}
```

`ShadowQuality`, `TextureQuality`, `ViewDistanceQuality`, `AntiAliasingQuality`, `PostProcessingQuality`, `EffectsQuality`, `FoliageQuality`, `ShadingQuality`, `GlobalIlluminationQuality`, and `ReflectionQuality` use quality indices `0-3`. `CelShadingMode` is normalized to `0.0` or `1.0`.

## Character bone structure

The required canonical character schema contains exactly 55 bones:

- Core: `Root`, `hips`, `spine`, `chest`, `upperChest`, `neck`, `head`
- Eyes: `leftEye`, `rightEye`
- Left arm: `leftShoulder`, `leftUpperArm`, `leftLowerArm`, `leftHand`
- Right arm: `rightShoulder`, `rightUpperArm`, `rightLowerArm`, `rightHand`
- Left leg: `leftUpperLeg`, `leftLowerLeg`, `leftFoot`, `leftToes`
- Right leg: `rightUpperLeg`, `rightLowerLeg`, `rightFoot`, `rightToes`
- Left thumb: `leftThumbProximal`, `leftThumbIntermediate`, `leftThumbDistal`
- Left index: `leftIndexProximal`, `leftIndexIntermediate`, `leftIndexDistal`
- Left middle: `leftMiddleProximal`, `leftMiddleIntermediate`, `leftMiddleDistal`
- Left ring: `leftRingProximal`, `leftRingIntermediate`, `leftRingDistal`
- Left little: `leftLittleProximal`, `leftLittleIntermediate`, `leftLittleDistal`
- Right thumb: `rightThumbProximal`, `rightThumbIntermediate`, `rightThumbDistal`
- Right index: `rightIndexProximal`, `rightIndexIntermediate`, `rightIndexDistal`
- Right middle: `rightMiddleProximal`, `rightMiddleIntermediate`, `rightMiddleDistal`
- Right ring: `rightRingProximal`, `rightRingIntermediate`, `rightRingDistal`
- Right little: `rightLittleProximal`, `rightLittleIntermediate`, `rightLittleDistal`

`Root` must be the single root of the canonical reference skeleton. Character JSON maps each canonical key to exactly one source bone. Secondary Chaos chains are optional additions rooted at `hairRoot` and/or `dynRoot`; these names are reserved and are not replacements for any of the 55 required humanoid keys.
