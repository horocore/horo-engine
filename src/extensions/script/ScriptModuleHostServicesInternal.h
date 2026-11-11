#include "Horo/Extensions/ExtensionErrors.h"
#pragma once
#include "Horo/Extensions/JavaScriptModuleRuntime.h"
#include "Horo/Extensions/LuaScriptModuleRuntime.h"
#include "ModuleScriptHostBridge.h"
#include "ScriptModuleHostServices.h"
#include "ScriptModuleNativeBridge.h"
#include "ScriptModuleReferenceInstallation.h"

#include <array>

namespace Horo::Extensions {
    struct ScriptModuleHostServices::Impl final {
        std::shared_ptr<ModuleScriptHostAuthority> authority;
        const JobSystem &jobs;
        ScriptCapabilityProfile profile;
        ScriptModuleBindingCache cache;
        std::unique_ptr<ScriptModuleReferenceInstallation> reference;
        std::unique_ptr<ScriptModuleReferenceInstallation> predecessorReference;
        const std::thread::id owner{std::this_thread::get_id()};
        std::size_t activeOperations{};
        // Explicit finite embedding profile. Capacity failure never drops or truncates an operation.
        static constexpr std::size_t MaximumOperations = 4;
        std::array<ScriptModuleOperation *, MaximumOperations> operations{};

        Impl(std::shared_ptr<ModuleScriptHostAuthority> actual, const JobSystem &system, ScriptCapabilityProfile shape)
            : authority(std::move(actual)), jobs(system), profile(shape) {}
    };

    struct ScriptModuleOperation::Impl final {
        std::shared_ptr<ScriptModuleHostServices> owner;
        std::unique_ptr<IScriptModuleRuntime> runtime;
        std::shared_ptr<ScriptModuleContext> context;
        std::shared_ptr<const ScriptModuleProgram> program;
        std::optional<ScriptCallResult> result;
        ScriptCallResult emergency;
        std::thread::id lane{std::this_thread::get_id()};
        bool counted{};
        ~Impl();
        Impl(std::shared_ptr<ScriptModuleHostServices> host, std::unique_ptr<IScriptModuleRuntime> vm,
             std::shared_ptr<ScriptModuleContext> admitted, std::shared_ptr<const ScriptModuleProgram> captured);
    };

    [[nodiscard]] std::vector<ExtensionCapabilityId> ScriptModuleRequestedCapabilities(const ScriptModuleProgram &);
    [[nodiscard]] ScriptCapabilityContextDescriptor ScriptModuleOperationContext(const ScriptModuleProgram &, ScriptModuleOperationPolicy,
                                                                                 ScriptCapabilityProfile, std::uint64_t revision);
}  // namespace Horo::Extensions
