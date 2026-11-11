#include "ScriptModuleHostServicesInternal.h"

#include <algorithm>

namespace Horo::Extensions {
    std::vector<ExtensionCapabilityId> ScriptModuleRequestedCapabilities(const ScriptModuleProgram &program) {
        std::vector<ExtensionCapabilityId> result;
        result.reserve(program.Module().imports.size() + 1);
        for (const auto &imported : program.Module().imports)
            result.emplace_back(imported.service);
        if (program.RequestsReference())
            result.emplace_back("horo.script.reference");
        std::ranges::sort(result, {}, &ExtensionCapabilityId::value);
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    ScriptCapabilityContextDescriptor ScriptModuleOperationContext(const ScriptModuleProgram &program, ScriptModuleOperationPolicy policy,
                                                                   ScriptCapabilityProfile profile, std::uint64_t revision) {
        auto requested = ScriptModuleRequestedCapabilities(program);
        return {.scriptId = program.ScriptId(),
                .policyRevision = revision,
                .profile = profile,
                .packageVerified = true,
                .packageTrusted = true,
                .scriptTrusted = true,
                .runtimeCompatible = true,
                .packageCapabilities = requested,
                .scriptCapabilities = std::move(requested),
                .projectCapabilities = std::move(policy.projectCapabilities),
                .contextCapabilities = std::move(policy.contextCapabilities),
                .scopes = std::move(policy.scopes)};
    }
}  // namespace Horo::Extensions
