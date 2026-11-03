#include "ScriptModuleProjection.h"

#include "Horo/Extensions/ExtensionErrors.h"

#include <new>

namespace Horo::Extensions {
    Result<ScriptModuleFunctionView> FindScriptModuleFunction(std::span<const ScriptModuleImport> imports, std::size_t imported,
                                                              std::size_t method, std::size_t count) {
        using Output = Result<ScriptModuleFunctionView>;
        if (imported >= imports.size() || !imports[imported].Backend())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationUnavailable));
        const auto &api = imports[imported].Backend()->Descriptor();
        if (method >= api.functions.size())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
        const auto &function = api.functions[method];
        if (!imports[imported].Backend()->AllowsFunction(function.id) || count > function.parameters.size())
            return Output::Failure(
                MakeError(ExtensionErrors::ScriptInvocationInvalid, "Function is not admitted or argument count exceeds its descriptor."));
        return Output::Success({&imports[imported], &api, &function});
    }

    ScriptCallResult MakeScriptModuleEmergencyResult() {
        auto error = MakeScriptError(
            MakeError(ExtensionErrors::ScriptInvocationAbandoned, "Module execution failed; diagnostic allocation was unavailable."));
        if (error.HasError())
            throw std::bad_alloc{};
        return ScriptCallResult::Failure(std::move(error).Value());
    }

    void SetScriptModuleFailure(std::optional<ScriptCallResult> &result, ScriptCallResult &fallback, const Error &error,
                                const ScriptValueLimits &limits) noexcept {
        try {
            auto converted = MakeScriptError(error, false, limits);
            if (converted.HasValue()) {
                result = ScriptCallResult::Failure(std::move(converted).Value());
                return;
            }
        } catch (...) {
        }
        result = std::move(fallback);
    }
}  // namespace Horo::Extensions
