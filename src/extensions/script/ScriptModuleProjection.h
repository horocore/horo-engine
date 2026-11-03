#pragma once
#include "Horo/Extensions/ScriptModuleRuntime.h"

namespace Horo::Extensions {
    struct ScriptModuleFunctionView final {
        const ScriptModuleImport *imported{};
        const ScriptExportDescriptor *api{};
        const ScriptExportFunctionDescriptor *function{};
    };

    [[nodiscard]] Result<ScriptModuleFunctionView> FindScriptModuleFunction(std::span<const ScriptModuleImport>, std::size_t, std::size_t,
                                                                            std::size_t);
    [[nodiscard]] ScriptCallResult MakeScriptModuleEmergencyResult();
    void SetScriptModuleFailure(std::optional<ScriptCallResult> &, ScriptCallResult &fallback, const Error &,
                                const ScriptValueLimits &) noexcept;
}  // namespace Horo::Extensions
