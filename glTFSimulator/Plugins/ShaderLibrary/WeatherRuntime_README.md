# Weather runtime integration

The ShaderLibrary plugin owns the weather camera, Niagara rain capture, weather post process, and surface coverage state.

## Runtime flow

- `UWeatherRuntimeSubsystem` owns the weather lifecycle per game instance.
- `AWeatherRuntimeActor` follows the player camera with a top-down `USceneCaptureComponent2D`.
- `RT_Rain` is the capture target used by `NG_Rain` through the `Matrix` parameter for depth collision.
- `PP_Weather` is added as an unbound post process blendable and driven through `Precipitation` plus `WeatherMode`.
- Rain raises wetness over time and dries when precipitation stops.
- Snow raises surface coverage over time and melts slowly when precipitation stops.
- The post process rejects sheltered surfaces with a screen/depth roof mask, so ground under a ceiling is not wetted/snow-covered.

## Project-side API

The project only forwards the current weather data (`Preset`, `Intensity`, `bEnabled`) and the active player camera to `UWeatherRuntimeSubsystem`. Weather capture distances, camera offset, Niagara parameter names, and accumulation rates are plugin-owned.

## Plugin dependency

`ShaderLibrary.uplugin` declares Niagara as a plugin dependency because the runtime module instantiates and controls `UNiagaraComponent`.
