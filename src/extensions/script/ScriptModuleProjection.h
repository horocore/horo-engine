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
    /** Stable host-captured identity lookup inside the SAME actual admitted context; descriptor offsets are not closure identity. */
    [[nodiscard]] Result<ScriptModuleFunctionView> FindScriptModuleFunction(std::span<const ScriptModuleImport>, std::string_view alias,
                                                                            std::string_view function, std::size_t argumentCount);
    [[nodiscard]] ScriptCallResult MakeScriptModuleEmergencyResult();
    void SetScriptModuleFailure(std::optional<ScriptCallResult> &, ScriptCallResult &fallback, const Error &,
                                const ScriptValueLimits &) noexcept;
}  // namespace Horo::Extensions
