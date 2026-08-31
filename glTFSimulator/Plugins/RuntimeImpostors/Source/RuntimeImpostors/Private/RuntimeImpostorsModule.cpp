// Copyright 2026 OpenAI. Licensed under the MIT License.

#include "Modules/ModuleManager.h"

class FRuntimeImpostorsModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FRuntimeImpostorsModule, RuntimeImpostors)
