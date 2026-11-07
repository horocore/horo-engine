#pragma once
/** @file LuaScriptModuleRuntime.h @brief Actual adapter-owned factory; no VM handles escape this header. */
#include "Horo/Extensions/ScriptModuleRuntime.h"

namespace Horo::Extensions {
    [[nodiscard]] ScriptModuleRuntimeIdentity LuaScriptModuleRuntimeIdentity(std::string targetProfile);
    [[nodiscard]] Result<std::unique_ptr<IScriptModuleRuntime>> CreateLuaScriptModuleRuntime(std::shared_ptr<ScriptModuleContext>);
}  // namespace Horo::Extensions
