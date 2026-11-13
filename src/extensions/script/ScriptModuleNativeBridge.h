#pragma once
#include "Horo/Extensions/ScriptModuleRuntime.h"
#include "ModuleScriptHostBridge.h"

namespace Horo::Extensions {
    Result<ScriptModuleImport> BindModuleScriptHostImport(const ModuleScriptHostAuthority &, std::string_view package,
                                                          std::string_view moduleId, std::string_view importId, std::string alias,
                                                          ScriptCapabilityProfile);
}
