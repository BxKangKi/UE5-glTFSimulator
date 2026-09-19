#include "ShaderLibraryModule.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

DEFINE_LOG_CATEGORY_STATIC(LogShaderLibraryModule, Log, All);

void FShaderLibraryModule::StartupModule()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ShaderLibrary"));
    if (!Plugin.IsValid())
    {
        UE_LOG(LogShaderLibraryModule, Error, TEXT("Unable to locate ShaderLibrary plugin while registering shader source directory."));
        return;
    }

    const FString ShaderDirectory = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
    if (!FPaths::DirectoryExists(ShaderDirectory))
    {
        UE_LOG(LogShaderLibraryModule, Error, TEXT("Shader source directory does not exist: %s"), *ShaderDirectory);
        return;
    }

    AddShaderSourceDirectoryMapping(TEXT("/Plugin/ShaderLibrary"), ShaderDirectory);
    UE_LOG(LogShaderLibraryModule, Log, TEXT("Registered shader source mapping /Plugin/ShaderLibrary -> %s"), *ShaderDirectory);
}

void FShaderLibraryModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FShaderLibraryModule, ShaderLibrary)
