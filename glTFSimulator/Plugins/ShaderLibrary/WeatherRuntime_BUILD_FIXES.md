ShaderLibrary Weather Runtime - build fixes

UE 5.8.1 build-log fixes applied:
1. Removed the local NAME_Matrix symbol from WeatherRuntimeActor.cpp to avoid collision with Unreal's registered NAME_Matrix EName.
2. Niagara remains declared as a plugin dependency in ShaderLibrary.uplugin and as a private module dependency in ShaderLibrary.Build.cs.
3. Weather runtime asset paths and existing ShaderLibrary content paths are unchanged.
