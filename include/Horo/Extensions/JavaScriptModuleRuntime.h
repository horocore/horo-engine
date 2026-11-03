#pragma once
/** @file JavaScriptModuleRuntime.h @brief Actual adapter-owned factory; no VM handles escape this header. */
#include "Horo/Extensions/ScriptModuleRuntime.h"

namespace Horo::Extensions {
    [[nodiscard]] ScriptModuleRuntimeIdentity JavaScriptModuleRuntimeIdentity(std::string targetProfile);
    [[nodiscard]] Result<std::unique_ptr<IScriptModuleRuntime>> CreateJavaScriptModuleRuntime(std::shared_ptr<ScriptModuleContext>);
}  // namespace Horo::Extensions
